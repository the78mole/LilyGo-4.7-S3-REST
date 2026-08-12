#include "display_manager.h"

#include <string.h>

#include "app_config.h"
#include "epd_driver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/*
 * -----------------------------------------------------------------------
 * Panel driver: LilyGo-EPD47 (vendored in components/lilygo_epd47)
 * -----------------------------------------------------------------------
 * This board (2021-6-10 / V2.3) sequences panel power over a shift-register
 * config bus, not the I2C PCA9535 + TPS65185 of the later "S3 Pro" boards,
 * so upstream epdiy does not fit it. See components/lilygo_epd47/CMakeLists.txt
 * for the full rationale.
 *
 * We own a single full-screen 4bpp framebuffer in PSRAM. LVGL's flush
 * callback pokes 8-bit grayscale pixels into it via
 * display_manager_write_pixels(); a dedicated core-1 task then pushes the
 * whole buffer to the panel. That keeps the slow (~1s) physical refresh off
 * the LVGL task entirely.
 * -----------------------------------------------------------------------
 */

static const char *TAG = "display_manager";

/* 4bpp packed: two pixels per byte, EPD_WIDTH/2 bytes per row. */
static uint8_t *s_framebuffer;
static SemaphoreHandle_t s_fb_mutex;
static volatile bool s_dirty;
static TaskHandle_t s_refresh_task;

#define FRAMEBUFFER_SIZE (EPD_WIDTH * EPD_HEIGHT / 2)
#define EPD_WHITE_BYTE   0xFF

static inline void set_pixel_gray4(int x, int y, uint8_t gray8)
{
    /* Nibble order must match LilyGo-EPD47's epd_draw_pixel():
     *   even x -> LOW nibble, odd x -> HIGH nibble.
     * (Note this is the opposite of epdiy's packing -- getting it backwards
     * swaps every horizontally-adjacent pixel pair.)
     * 0x0 = black, 0xF = white. */
    uint8_t gray4 = gray8 >> 4;
    size_t byte_index = (size_t)y * (EPD_WIDTH / 2) + (x / 2);
    uint8_t current = s_framebuffer[byte_index];
    if (x & 1) {
        s_framebuffer[byte_index] = (current & 0x0F) | (uint8_t)(gray4 << 4);
    } else {
        s_framebuffer[byte_index] = (current & 0xF0) | gray4;
    }
}

void display_manager_write_pixels(int x1, int y1, int x2, int y2,
                                   const uint8_t *gray8, int stride_px)
{
    if (!s_framebuffer) {
        return;
    }

    /* Clip to the panel so a bad REST payload can't scribble out of bounds. */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= EPD_WIDTH) x2 = EPD_WIDTH - 1;
    if (y2 >= EPD_HEIGHT) y2 = EPD_HEIGHT - 1;
    if (x1 > x2 || y1 > y2) {
        return;
    }

    xSemaphoreTake(s_fb_mutex, portMAX_DELAY);
    for (int y = y1; y <= y2; y++) {
        const uint8_t *src_row = gray8 + (size_t)(y - y1) * stride_px;
        for (int x = x1; x <= x2; x++) {
            set_pixel_gray4(x, y, src_row[x - x1]);
        }
    }
    s_dirty = true;
    xSemaphoreGive(s_fb_mutex);

    /* Wake the refresh task; it debounces bursts of small flushes into a
     * single panel update. */
    if (s_refresh_task) {
        xTaskNotifyGive(s_refresh_task);
    }
}

void display_manager_wait_idle(void)
{
    while (s_dirty) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Wait for a wake-up, then debounce for a short window so several
         * back-to-back LVGL flushes coalesce into one physical refresh. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_LVGL_HANDLER_PERIOD_MS * 2));

        xSemaphoreTake(s_fb_mutex, portMAX_DELAY);
        bool dirty = s_dirty;
        s_dirty = false;
        xSemaphoreGive(s_fb_mutex);

        if (!dirty) {
            continue;
        }

        /* Clear-then-draw: the framebuffer always holds the complete composed
         * screen, so a full clear before drawing costs an extra ~600ms but
         * guarantees no ghosting from the previous frame. If update latency
         * matters more than contrast for your use case, drop the epd_clear()
         * and accept some ghosting (or add periodic full-clear cycles). */
        ESP_LOGD(TAG, "refresh: start");
        epd_poweron();
        epd_clear();
        epd_draw_grayscale_image(epd_full_screen(), s_framebuffer);
        epd_poweroff();
        ESP_LOGD(TAG, "refresh: done");
    }
}

esp_err_t display_manager_init(void)
{
    s_fb_mutex = xSemaphoreCreateMutex();
    if (!s_fb_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_framebuffer = heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_framebuffer) {
        ESP_LOGE(TAG, "Failed to allocate %d byte framebuffer in PSRAM",
                 FRAMEBUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }
    memset(s_framebuffer, EPD_WHITE_BYTE, FRAMEBUFFER_SIZE);

    epd_init();

    epd_poweron();
    epd_clear();
    epd_poweroff();

    BaseType_t ok = xTaskCreatePinnedToCore(refresh_task, "epd_refresh", 4096,
                                             NULL, tskIDLE_PRIORITY + 3,
                                             &s_refresh_task, APP_DISPLAY_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LilyGo-EPD47 panel ready: %dx%d (core %d)",
             EPD_WIDTH, EPD_HEIGHT, APP_DISPLAY_CORE);
    return ESP_OK;
}

int display_manager_width(void)
{
    return EPD_WIDTH;
}

int display_manager_height(void)
{
    return EPD_HEIGHT;
}
