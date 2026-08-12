#include "lvgl_port.h"

#include <string.h>

#include "app_config.h"
#include "display_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "lvgl_port";

static SemaphoreHandle_t s_lvgl_mutex;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static uint8_t *s_gray_scratch; /* full-panel 8bpp staging buffer, PSRAM */
static esp_timer_handle_t s_tick_timer;

static inline uint8_t lv_color8_to_gray8(lv_color_t c)
{
    /* LV_COLOR_DEPTH=8 packs pixels as RGB332 (red:3 green:3 blue:2), not
     * as raw grayscale, so convert via luminance rather than using the
     * byte value directly. */
    uint8_t raw = c.full;
    uint8_t r3 = (raw >> 5) & 0x07;
    uint8_t g3 = (raw >> 2) & 0x07;
    uint8_t b2 = raw & 0x03;

    uint8_t r8 = (uint8_t)(r3 * 255 / 7);
    uint8_t g8 = (uint8_t)(g3 * 255 / 7);
    uint8_t b8 = (uint8_t)(b2 * 255 / 3);

    return (uint8_t)((r8 * 299 + g8 * 587 + b8 * 114) / 1000);
}

static void epd_rounder_cb(lv_disp_drv_t *drv, lv_area_t *area)
{
    (void)drv;
    /* The panel framebuffer packs 2 pixels/byte; keep flushed areas on
     * even x boundaries so nibble packing in display_manager stays
     * aligned. */
    area->x1 &= ~1;
    if ((area->x2 & 1) == 0) {
        area->x2 += 1;
    }
}

static void epd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    int32_t panel_w = display_manager_width();

    for (int32_t y = 0; y < h; y++) {
        uint8_t *dst_row = s_gray_scratch + (size_t)(area->y1 + y) * panel_w + area->x1;
        lv_color_t *src_row = color_p + y * w;
        for (int32_t x = 0; x < w; x++) {
            dst_row[x] = lv_color8_to_gray8(src_row[x]);
        }
    }

    display_manager_write_pixels(area->x1, area->y1, area->x2, area->y2,
                                  s_gray_scratch + (size_t)area->y1 * panel_w + area->x1,
                                  panel_w);

    lv_disp_flush_ready(drv);
}

static void tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(CONFIG_APP_LVGL_TICK_PERIOD_MS);
}

/* lv_timer_handler() returns the time until the next scheduled timer, which is
 * LV_NO_TIMER_READY (UINT32_MAX) whenever nothing is pending. Feeding that
 * straight into vTaskDelay() puts this task to sleep for ~49 days, so the
 * initial screen renders and every later change (e.g. an object created by a
 * REST handler) is never drawn. Always clamp the result. */
#define LVGL_TASK_MAX_SLEEP_MS 50
#define LVGL_TASK_MIN_SLEEP_MS 1

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t delay_ms = LVGL_TASK_MAX_SLEEP_MS;
        if (lvgl_port_lock(portMAX_DELAY)) {
            delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_SLEEP_MS) {
            delay_ms = LVGL_TASK_MAX_SLEEP_MS;
        } else if (delay_ms < LVGL_TASK_MIN_SLEEP_MS) {
            delay_ms = LVGL_TASK_MIN_SLEEP_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mutex, ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    xSemaphoreGive(s_lvgl_mutex);
}

esp_err_t lvgl_port_init(void)
{
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (!s_lvgl_mutex) {
        return ESP_ERR_NO_MEM;
    }

    int width = display_manager_width();
    int height = display_manager_height();

    s_gray_scratch = heap_caps_malloc((size_t)width * height, MALLOC_CAP_SPIRAM);
    if (!s_gray_scratch) {
        ESP_LOGE(TAG, "Failed to allocate %dx%d grayscale scratch buffer in PSRAM",
                 width, height);
        return ESP_ERR_NO_MEM;
    }
    memset(s_gray_scratch, 0xFF, (size_t)width * height);

    lv_init();

    /* Partial draw buffers (a fraction of the screen each) -- LVGL renders
     * into these, then epd_flush_cb() converts+copies into s_gray_scratch
     * and the panel framebuffer. Both buffers live in PSRAM. */
    size_t buf_pixels = (size_t)width * 40; /* ~40 rows/flush */
    lv_color_t *buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = width;
    s_disp_drv.ver_res = height;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = epd_flush_cb;
    s_disp_drv.rounder_cb = epd_rounder_cb;
    s_disp_drv.full_refresh = false;
    lv_disp_drv_register(&s_disp_drv);

    const esp_timer_create_args_t tick_timer_args = {
        .callback = &tick_timer_cb,
        .name = "lv_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(
        s_tick_timer, CONFIG_APP_LVGL_TICK_PERIOD_MS * 1000ULL));

    TaskHandle_t task_handle;
    BaseType_t ok = xTaskCreatePinnedToCore(
        lvgl_task, "lvgl", CONFIG_APP_LVGL_TASK_STACK, NULL,
        CONFIG_APP_LVGL_TASK_PRIORITY, &task_handle, APP_DISPLAY_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LVGL ready (%dx%d, draw buffers + scratch in PSRAM)", width, height);
    return ESP_OK;
}
