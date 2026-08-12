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

/* Bounding box of everything written since the last panel update. Tracked so
 * a one-line change (the departure strip ticking over) costs a ~200ms partial
 * update instead of a ~4s full-screen clear-and-redraw. */
static int s_dx1, s_dy1, s_dx2, s_dy2;
static int s_partials_since_full;

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
    if (!s_dirty) {
        s_dx1 = x1; s_dy1 = y1; s_dx2 = x2; s_dy2 = y2;
    } else {
        if (x1 < s_dx1) s_dx1 = x1;
        if (y1 < s_dy1) s_dy1 = y1;
        if (x2 > s_dx2) s_dx2 = x2;
        if (y2 > s_dy2) s_dy2 = y2;
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
        int x1 = s_dx1, y1 = s_dy1, x2 = s_dx2, y2 = s_dy2;
        s_dirty = false;
        xSemaphoreGive(s_fb_mutex);

        if (!dirty) {
            continue;
        }

        /* Align to 4 pixels, not 2: epd_clear_area_cycles() indexes its mask
         * with (area.x % 4), so a merely byte-aligned rectangle comes out
         * shifted on the panel. */
        x1 &= ~3;
        x2 |= 3;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 >= EPD_WIDTH) x2 = EPD_WIDTH - 1;
        if (y2 >= EPD_HEIGHT) y2 = EPD_HEIGHT - 1;

        /* One row of margin on each side: the copied rows carry their real
         * framebuffer content, so redrawing them is harmless, and it keeps a
         * one-row seam from appearing at the boundary. */
        if (y1 > 0) y1--;
        if (y2 < EPD_HEIGHT - 1) y2++;

        int aw = x2 - x1 + 1;
        int ah = y2 - y1 + 1;
        long area_px = (long)aw * ah;
        long full_px = (long)EPD_WIDTH * EPD_HEIGHT;

        /* Partial updates leave a little residue behind, so fall back to a
         * full clear when the change is large anyway, and force one
         * periodically to stop ghosting accumulating over a long run of
         * minute-by-minute strip updates. */
        bool go_full = (area_px * 2 > full_px) ||
                       (s_partials_since_full >= CONFIG_APP_FULL_REFRESH_EVERY);

        ESP_LOGD(TAG, "refresh: begin %s %dx%d at (%d,%d)",
                 go_full ? "FULL" : "partial", aw, ah, x1, y1);
        epd_poweron();
        if (go_full) {
            epd_clear();
            epd_draw_grayscale_image(epd_full_screen(), s_framebuffer);
            s_partials_since_full = 0;
            ESP_LOGI(TAG, "full refresh");
        } else {
            /* epd_draw_grayscale_image() wants a buffer sized to the area, so
             * copy the rows out of the full framebuffer. */
            size_t row_bytes = (size_t)aw / 2;
            uint8_t *sub = heap_caps_malloc(row_bytes * ah, MALLOC_CAP_SPIRAM);
            if (sub) {
                for (int y = 0; y < ah; y++) {
                    memcpy(sub + (size_t)y * row_bytes,
                           s_framebuffer + (size_t)(y1 + y) * (EPD_WIDTH / 2) + x1 / 2,
                           row_bytes);
                }
                /* Draw shifted against the cleared rectangle to cancel the
                 * driver's asymmetric row pipelining -- see
                 * CONFIG_APP_PARTIAL_Y_SHIFT. */
                int draw_y = y1 + CONFIG_APP_PARTIAL_Y_SHIFT;
                if (draw_y < 0) draw_y = 0;
                if (draw_y + ah > EPD_HEIGHT) draw_y = EPD_HEIGHT - ah;
                Rect_t area = { .x = x1, .y = y1, .width = aw, .height = ah };
                Rect_t draw_area = { .x = x1, .y = draw_y, .width = aw, .height = ah };
                /* E-paper is persistent: drawing without clearing first
                 * overlays the new image on the old one instead of replacing
                 * it. The full-refresh path calls epd_clear() for the same
                 * reason. */
                epd_clear_area(area);
                epd_draw_grayscale_image(draw_area, sub);
                heap_caps_free(sub);
                s_partials_since_full++;
                ESP_LOGI(TAG, "partial refresh %dx%d at (%d,%d)", aw, ah, x1, y1);
            } else {
                epd_clear();
                epd_draw_grayscale_image(epd_full_screen(), s_framebuffer);
                s_partials_since_full = 0;
            }
        }
        epd_poweroff();
        ESP_LOGD(TAG, "refresh: end");
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
