#include "esp_log.h"

#include "app_config.h"
#include "display_manager.h"
#include "http_server.h"
#include "lvgl_port.h"
#include "sd_card.h"
#include "departures.h"
#include "time_sync.h"
#include "wifi_manager.h"

static const char *TAG = "app_main";

void app_main(void)
{
    /* Panel BEFORE SD card, deliberately: epd_init() sets up the LCD_CAM/i80
     * bus and its GDMA channel, and doing that after the SD card's SPI+DMA is
     * already running leaves the card wedged (every later access returns
     * ESP_ERR_TIMEOUT until a power cycle). Verified by bisect on hardware:
     * epd_init() alone is enough to break it -- no panel power-on or refresh
     * required -- so this is a peripheral/DMA init-order constraint, not a
     * current-draw brownout. */
    ESP_LOGI(TAG, "Initializing e-paper panel...");
    ESP_ERROR_CHECK(display_manager_init());

    ESP_LOGI(TAG, "Mounting SD card...");
    esp_err_t err = sd_card_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed (%s) -- /api/upload and "
                      "/api/display/image will not work", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Starting LVGL (core %d)...", APP_DISPLAY_CORE);
    ESP_ERROR_CHECK(lvgl_port_init());

    ESP_LOGI(TAG, "Connecting to Wi-Fi (core %d)...", APP_NETWORK_CORE);
    if (wifi_manager_start() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi did not connect -- REST API will only be reachable "
                      "once connectivity is restored");
    }

    /* After Wi-Fi: charts derive their "now" marker from this clock, so a
     * client never has to send a highlight index. */
    if (time_sync_start() != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed -- charts will render without a now-marker");
    }

    /* Departures are fetched by the device itself, so they stay current
     * between dashboard pushes instead of ageing with the last REST call. */
    if (departures_start() != ESP_OK) {
        ESP_LOGW(TAG, "departure poller failed to start");
    }

    ESP_LOGI(TAG, "Starting REST API...");
    ESP_ERROR_CHECK(http_server_start());

    ESP_LOGI(TAG, "Ready.");
}
