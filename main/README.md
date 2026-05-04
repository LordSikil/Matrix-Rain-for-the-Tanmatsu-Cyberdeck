# Matrix Rain Terminal 🟩
### for the Tanmatsu Cyberdeck — Nicolai Electronics

A full-screen Matrix-style "digital rain" effect for the Tanmatsu's
800 × 480 MIPI DSI display, written in C using ESP-IDF + LVGL.

---

## Hardware

| Property | Value |
|---|---|
| MCU | ESP32-P4 (dual-core 400 MHz RISC-V) |
| Display | 3.97″ 800 × 480 MIPI DSI (ST7701S) |
| Keyboard | 69-key QWERTY via CH32V203 coprocessor (I²C) |
| PSRAM | 32 MB (canvas buffer lives here) |

---

## Project layout

```
matrix_rain/
├── CMakeLists.txt          ← top-level project
├── sdkconfig.defaults      ← Tanmatsu-tuned Kconfig defaults
└── main/
    ├── CMakeLists.txt      ← main component
    ├── idf_component.yml   ← component dependencies
    └── main.c              ← all application logic
```

---

## Prerequisites

1. **ESP-IDF ≥ 5.3** installed and sourced:
   ```bash
   . $IDF_PATH/export.sh
   ```

2. **Tanmatsu BSP** and coprocessor components are declared in
   `main/idf_component.yml` and will be downloaded automatically
   by the IDF component manager on first build.

---

## Build & flash

```bash
# Clone / enter project
cd matrix_rain

# Set target (only needed once per clone)
idf.py set-target esp32p4

# Download components, configure, and build
idf.py build

# Flash to Tanmatsu (replace /dev/ttyUSB0 with your port)
idf.py -p /dev/ttyUSB0 flash

# Optional: monitor serial output
idf.py -p /dev/ttyUSB0 monitor
```

> **Tip:** Hold the **↓ (volume down)** button while applying power to
> put the ESP32-P4 into USB download mode if the normal flash fails.

### Running as a Launcher app (AppFS)

If you want the app to appear in the Tanmatsu launcher, build and
install it as an AppFS binary.  The launcher SDK (`tools/plugin-sdk`
in the tanmatsu-launcher repository) provides the `make install`
workflow for this.  Refer to the launcher documentation for details.

---

## Controls

| Key | Action |
|---|---|
| **Q** or **ESC** | Exit back to launcher |
| **↑ Up** | Speed up rain (−10 ms/tick) |
| **↓ Down** | Slow down rain (+10 ms/tick) |
| **→ Right** | Increase column density (+10 %) |
| **← Left** | Decrease column density (−10 %) |
| **C** | Cycle colour theme (Green → Cyan → Amber → White) |
| **R** | Reset and re-randomise all columns |

---

## How it works

### Rendering model

The app allocates a single **full-screen LVGL canvas**
(`800 × 480 × 2 bytes = 768 KB`) in PSRAM and draws directly into it
each tick.  LVGL's DMA-capable flush path pushes the buffer to the
MIPI DSI panel via the Tanmatsu BSP.

### Rain simulation

Each of the **80 columns** (800 px / 10 px per cell) has independent state:

```c
typedef struct {
    int  head_row;      // current leading-drop row
    int  length;        // trail length (rows)
    int  speed;         // rows per 2 ticks (1–3)
    bool active;        // spawned on screen?
    int  pause;         // cooldown before next spawn
    char glyphs[ROWS];  // per-cell character
} column_t;
```

On every tick:
1. Each column advances its head by `speed / 2` rows.
2. The head cell is drawn in **white** (bright flash).
3. Cells 1–3 behind the head use the theme's **bright** colour.
4. The middle section uses **mid** colour.
5. The tail uses **dim** colour.
6. The cell that just scrolled off the bottom is erased to black.

### Colour themes

Four built-in themes, cycled with **C**:

| # | Name | Head | Body |
|---|---|---|---|
| 0 | Classic Green | White | `#00FF41` → `#004008` |
| 1 | Cyan | White | `#00FFFF` → `#003040` |
| 2 | Amber | Warm white | `#FFB000` → `#401800` |
| 3 | Silver/White | White | `#C0C0C0` → `#202020` |

---

## Tuning

| `sdkconfig` / source constant | Effect |
|---|---|
| `CELL_W` / `CELL_H` (main.c) | Glyph cell size in pixels |
| `g_tick_ms` initial value | Starting animation speed |
| `g_density` initial value | Starting column density (%) |
| `CONFIG_LV_FONT_UNSCII_8` | Font — swap for a wider font for larger cells |

---

## Licence

MIT — do whatever you like with it.
