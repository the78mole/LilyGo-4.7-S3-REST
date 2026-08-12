#include "ui_tick.h"

#include <time.h>

#include "app_config.h"
#include "chart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "time_sync.h"
#include "widgets.h"

static const char *TAG = "ui_tick";

/*
 * Drives the parts of the screen that go stale on their own, so the panel
 * stays correct between dashboard pushes:
 *
 *   - every minute        the departure strip and Wi-Fi signal
 *   - every quarter hour  today's price chart, to move the "now" marker
 *
 * Both repaint in place and invalidate only their own rectangle, so each
 * costs a small partial panel update rather than a full-screen redraw.
 * Boundaries are detected by watching the wall clock rather than by counting
 * elapsed time, so the quarter-hour redraw lands on :00/:15/:30/:45 instead of
 * drifting away from them.
 */

static void ui_tick_task(void *arg)
{
    (void)arg;
    int last_min = -1;
    int last_quarter = -1;

    for (;;) {
        if (time_sync_is_valid()) {
            time_t now = 0;
            time(&now);
            struct tm lt;
            localtime_r(&now, &lt);
            int quarter = lt.tm_hour * 4 + lt.tm_min / 15;

            if (lt.tm_min != last_min) {
                last_min = lt.tm_min;
                if (lvgl_port_lock(2000)) {
                    widget_agenda_refresh_transit();
                    lvgl_port_unlock();
                }
            }

            if (quarter != last_quarter) {
                /* Skip the very first pass: the chart has just been drawn (or
                 * has no data yet), so redrawing immediately would only cost
                 * a panel update. */
                if (last_quarter >= 0) {
                    ESP_LOGI(TAG, "quarter-hour boundary, redrawing today's chart");
                    if (lvgl_port_lock(2000)) {
                        chart_redraw(0);
                        lvgl_port_unlock();
                    }
                }
                last_quarter = quarter;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t ui_tick_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(ui_tick_task, "ui_tick", 4096, NULL,
                                             tskIDLE_PRIORITY + 2, NULL,
                                             APP_DISPLAY_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
