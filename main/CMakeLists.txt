idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        # Tanmatsu Board Support Package (display + input)
        tanmatsu_bsp
        # LVGL graphics library via BSP interface
        esp_lvgl_port
        # FreeRTOS
        freertos
        # Logging
        log
        # Random number generation
        esp_hw_support
)
