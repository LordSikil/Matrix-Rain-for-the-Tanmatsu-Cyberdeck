/**
 * Matrix Rain Terminal — Tanmatsu Cyberdeck
 * Nicolai Electronics / ESP32-P4
 *
 * Classic "Matrix digital rain" effect on the 800x480 MIPI DSI display.
 * - Full 800x480 rendering via LVGL canvas on the Tanmatsu BSP
 * - Configurable speed, density, and color via keyboard
 * - ESC or Q exits back to the launcher (AppFS return)
 *
 * Controls:
 *   ESC / Q    — exit
 *   UP / DOWN  — increase / decrease fall speed
 *   LEFT/RIGHT — decrease / increase column density
 *   C          — cycle color (green, cyan, white, amber)
 *   R          — reset / randomize
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"

#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/tanmatsu.h"

#include "lvgl.h"

/* ── Display geometry ─────────────────────────────────────────── */
#define SCREEN_W   800
#define SCREEN_H   480

/* ── Glyph grid (half-width katakana-style cells) ─────────────── */
#define CELL_W     10   /* pixels per character cell (x) */
#define CELL_H     16   /* pixels per character cell (y) */
#define COLS       (SCREEN_W / CELL_W)   /* 80 columns */
#define ROWS       (SCREEN_H / CELL_H)   /* 30 rows    */

static const char *TAG = "matrix_rain";

/* ── Colour themes ────────────────────────────────────────────── */
typedef struct {
    lv_color_t head;   /* bright leading glyph */
    lv_color_t bright; /* near-head glyphs     */
    lv_color_t mid;    /* middle of trail      */
    lv_color_t dim;    /* tail of trail        */
} theme_t;

static const theme_t THEMES[] = {
    /* Classic green */
    {
        .head   = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
        .bright = LV_COLOR_MAKE(0x00, 0xFF, 0x41),
        .mid    = LV_COLOR_MAKE(0x00, 0xC0, 0x20),
        .dim    = LV_COLOR_MAKE(0x00, 0x40, 0x08),
    },
    /* Cyan */
    {
        .head   = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
        .bright = LV_COLOR_MAKE(0x00, 0xFF, 0xFF),
        .mid    = LV_COLOR_MAKE(0x00, 0xA0, 0xC0),
        .dim    = LV_COLOR_MAKE(0x00, 0x30, 0x40),
    },
    /* Amber / orange */
    {
        .head   = LV_COLOR_MAKE(0xFF, 0xFF, 0xCC),
        .bright = LV_COLOR_MAKE(0xFF, 0xB0, 0x00),
        .mid    = LV_COLOR_MAKE(0xC0, 0x60, 0x00),
        .dim    = LV_COLOR_MAKE(0x40, 0x18, 0x00),
    },
    /* White / silver */
    {
        .head   = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
        .bright = LV_COLOR_MAKE(0xC0, 0xC0, 0xC0),
        .mid    = LV_COLOR_MAKE(0x70, 0x70, 0x70),
        .dim    = LV_COLOR_MAKE(0x20, 0x20, 0x20),
    },
};
#define NUM_THEMES  (sizeof(THEMES) / sizeof(THEMES[0]))

/* ── Rain state ───────────────────────────────────────────────── */
typedef struct {
    int  head_row;   /* current head position (row index, -n = above screen) */
    int  length;     /* trail length in rows */
    int  speed;      /* rows advanced per tick (1 = slow, 3 = fast) */
    int  speed_acc;  /* accumulator for sub-speed stepping */
    bool active;     /* column has an active drop */
    int  pause;      /* ticks to wait before spawning next drop */
    char glyphs[ROWS]; /* per-cell character, randomised each tick at head */
} column_t;

static column_t columns[COLS];

/* ── App state ────────────────────────────────────────────────── */
static volatile bool g_running   = true;
static int           g_theme_idx = 0;
static int           g_tick_ms   = 60;   /* ms between simulation ticks */
static int           g_density   = 70;   /* % chance a column spawns a new drop */

/* ── LVGL objects ─────────────────────────────────────────────── */
static lv_obj_t    *g_canvas     = NULL;
static lv_color_t   g_cbuf[SCREEN_W * SCREEN_H];   /* full-screen canvas buffer */

/* ── Katakana-ish glyph set (printable ASCII + some specials) ─── */
static const char GLYPHS[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "!@#$%^&*()_+-=[]{}|;:,./<>?";
#define GLYPH_COUNT  (sizeof(GLYPHS) - 1)

static inline char random_glyph(void) {
    return GLYPHS[esp_random() % GLYPH_COUNT];
}

/* ── Column helpers ──────────────────────────────────────────────*/
static void reset_column(int col) {
    column_t *c = &columns[col];
    c->active    = false;
    c->head_row  = -(int)(esp_random() % ROWS);   /* stagger start */
    c->length    = 4 + (esp_random() % (ROWS / 2));
    c->speed     = 1 + (esp_random() % 3);
    c->speed_acc = 0;
    c->pause     = esp_random() % 60;
    for (int r = 0; r < ROWS; r++) {
        c->glyphs[r] = random_glyph();
    }
}

static void init_columns(void) {
    for (int col = 0; col < COLS; col++) {
        reset_column(col);
        /* Stagger initial heads so they don't all appear at once */
        columns[col].head_row = -(int)(esp_random() % (ROWS * 2));
    }
}

/* ── Draw a single cell ──────────────────────────────────────────*/
static void draw_cell(lv_obj_t *canvas, int col, int row,
                      char ch, lv_color_t color) {
    if (row < 0 || row >= ROWS) return;

    int px = col * CELL_W;
    int py = row * CELL_H;

    /* Clear cell background to black */
    lv_canvas_draw_rect(canvas, px, py, CELL_W, CELL_H,
                        &(lv_draw_rect_dsc_t){
                            .bg_color = LV_COLOR_MAKE(0, 0, 0),
                            .bg_opa   = LV_OPA_COVER,
                        });

    /* Draw character */
    char buf[2] = {ch, '\0'};
    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.color   = color;
    ldsc.font    = &lv_font_unscii_8;   /* compact monospace font */
    lv_canvas_draw_text(canvas, px + 1, py + 4, CELL_W - 1, &ldsc, buf);
}

/* ── Simulation tick ─────────────────────────────────────────────*/
static void tick_rain(void) {
    const theme_t *th = &THEMES[g_theme_idx];

    for (int col = 0; col < COLS; col++) {
        column_t *c = &columns[col];

        /* Pause handling — wait before (re)spawning */
        if (!c->active && c->head_row < -(int)(c->length)) {
            if (c->pause > 0) {
                c->pause--;
                continue;
            }
            /* Decide whether to spawn based on density */
            if ((int)(esp_random() % 100) >= g_density) {
                c->pause = 10 + (esp_random() % 40);
                continue;
            }
            c->active   = true;
            c->head_row = 0;
        }

        /* Advance speed accumulator */
        c->speed_acc += c->speed;
        int steps = c->speed_acc / 2;
        c->speed_acc %= 2;

        for (int s = 0; s < steps; s++) {
            /* Erase the tail cell that's scrolling off */
            int tail = c->head_row - c->length;
            if (tail >= 0 && tail < ROWS) {
                draw_cell(g_canvas, col, tail, ' ', LV_COLOR_MAKE(0,0,0));
            }

            /* Advance head */
            c->head_row++;

            /* Randomise head glyph occasionally */
            if (c->head_row >= 0 && c->head_row < ROWS) {
                if ((esp_random() & 0x3) == 0) {
                    c->glyphs[c->head_row] = random_glyph();
                }
            }
        }

        /* Paint visible trail cells */
        for (int t = 0; t < c->length + 1; t++) {
            int row = c->head_row - t;
            if (row < 0 || row >= ROWS) continue;

            lv_color_t col_color;
            if (t == 0) {
                col_color = th->head;
                /* Randomise head glyph every frame for flicker */
                c->glyphs[row] = random_glyph();
            } else if (t <= 3) {
                col_color = th->bright;
            } else if (t <= c->length / 2) {
                col_color = th->mid;
            } else {
                col_color = th->dim;
            }

            draw_cell(g_canvas, col, row, c->glyphs[row], col_color);
        }

        /* Column done: tail has left the screen */
        if (c->head_row - c->length >= ROWS) {
            reset_column(col);
            /* Randomise pause so columns don't all sync up */
            c->pause = esp_random() % 80;
        }
    }
}

/* ── Keyboard input handler ──────────────────────────────────────*/
static void handle_input(const bsp_input_event_t *event) {
    if (event->type != BSP_INPUT_EVENT_TYPE_NAVIGATION &&
        event->type != BSP_INPUT_EVENT_TYPE_KEYBOARD) return;

    if (event->state != BSP_INPUT_STATE_PRESSED) return;

    switch (event->navigation_key) {
        /* Exit */
        case BSP_INPUT_NAVIGATION_KEY_ESCAPE:
            g_running = false;
            return;
        /* Speed */
        case BSP_INPUT_NAVIGATION_KEY_UP:
            if (g_tick_ms > 20) g_tick_ms -= 10;
            ESP_LOGI(TAG, "Speed up: %d ms/tick", g_tick_ms);
            return;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (g_tick_ms < 200) g_tick_ms += 10;
            ESP_LOGI(TAG, "Speed down: %d ms/tick", g_tick_ms);
            return;
        /* Density */
        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
            if (g_density < 100) g_density += 10;
            ESP_LOGI(TAG, "Density: %d%%", g_density);
            return;
        case BSP_INPUT_NAVIGATION_KEY_LEFT:
            if (g_density > 10) g_density -= 10;
            ESP_LOGI(TAG, "Density: %d%%", g_density);
            return;
        default:
            break;
    }

    /* Character key handling */
    if (event->type == BSP_INPUT_EVENT_TYPE_KEYBOARD) {
        switch (event->character) {
            case 'q':
            case 'Q':
                g_running = false;
                break;
            case 'c':
            case 'C':
                g_theme_idx = (g_theme_idx + 1) % NUM_THEMES;
                ESP_LOGI(TAG, "Theme: %d", g_theme_idx);
                break;
            case 'r':
            case 'R':
                init_columns();
                ESP_LOGI(TAG, "Reset columns");
                break;
            default:
                break;
        }
    }
}

/* ── HUD overlay (help text, top-left corner) ────────────────────*/
static void draw_hud(void) {
    static char hud[128];
    const char *theme_names[] = {"Green", "Cyan", "Amber", "White"};

    snprintf(hud, sizeof(hud),
             " [C]olor:%s [UP/DN]Speed:%dms [LT/RT]Density:%d%% [Q]uit",
             theme_names[g_theme_idx % NUM_THEMES],
             g_tick_ms,
             g_density);

    /* Dark background strip */
    lv_canvas_draw_rect(g_canvas, 0, 0, SCREEN_W, CELL_H,
                        &(lv_draw_rect_dsc_t){
                            .bg_color = LV_COLOR_MAKE(0, 0, 0),
                            .bg_opa   = LV_OPA_50,
                        });

    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.color = LV_COLOR_MAKE(0x80, 0x80, 0x80);
    ldsc.font  = &lv_font_unscii_8;
    lv_canvas_draw_text(g_canvas, 2, 4, SCREEN_W - 4, &ldsc, hud);
}

/* ── Main LVGL task ──────────────────────────────────────────────*/
static void matrix_task(void *arg) {
    ESP_LOGI(TAG, "Matrix Rain starting (%dx%d, %d cols x %d rows)",
             SCREEN_W, SCREEN_H, COLS, ROWS);

    /* Register input callback */
    bsp_input_register_callback(handle_input);

    /* Create full-screen canvas */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, LV_COLOR_MAKE(0, 0, 0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    g_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(g_canvas, g_cbuf, SCREEN_W, SCREEN_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(g_canvas, 0, 0);
    lv_canvas_fill_bg(g_canvas, LV_COLOR_MAKE(0, 0, 0), LV_OPA_COVER);

    /* Seed columns */
    init_columns();

    TickType_t last_tick = xTaskGetTickCount();

    while (g_running) {
        /* Wait for next tick interval */
        vTaskDelayUntil(&last_tick, pdMS_TO_TICKS(g_tick_ms));

        /* Acquire LVGL mutex */
        if (bsp_display_lock(10)) {
            tick_rain();
            draw_hud();
            bsp_display_unlock();
        }
    }

    /* Clean up */
    bsp_input_unregister_callback(handle_input);
    if (g_canvas) {
        lv_obj_del(g_canvas);
        g_canvas = NULL;
    }

    ESP_LOGI(TAG, "Matrix Rain exiting");
    vTaskDelete(NULL);
}

/* ── Entry point ─────────────────────────────────────────────────*/
void app_main(void) {
    ESP_LOGI(TAG, "Matrix Rain — Tanmatsu Cyberdeck");

    /* Initialise BSP (display + input) */
    bsp_display_start();
    bsp_display_backlight_on();

    /* Run effect in its own task (needs a large stack for canvas operations) */
    xTaskCreate(matrix_task, "matrix", 16 * 1024, NULL,
                tskIDLE_PRIORITY + 2, NULL);

    /* app_main returns; FreeRTOS keeps the task alive */
}
