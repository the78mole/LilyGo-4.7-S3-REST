idf_component_register(
    SRCS
        "app_main.c"
        "wifi_manager.c"
        "sd_card.c"
        "display_manager.c"
        "lvgl_port.c"
        "http_server.c"
        "api_handlers.c"
        "chart.c"
        "ui_card.c"
        "widgets.c"
        "time_sync.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        fatfs
        sdmmc
        driver
        json
        esp_timer
        vfs
        lwip
        # Managed component pulled in via idf_component.yml.
        lvgl
        # Local component: components/lilygo_epd47 (vendored LilyGo-EPD47).
        lilygo_epd47
)
