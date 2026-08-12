#include "ui_tick.h"

#include <time.h>

#include "app_config.h"
#include "chart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "time_sync.h"
#include "ui_card.h"
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

/*
 * Drawn once at startup, before any client has pushed anything.
 *
 * Without this the panel stays blank after a reboot until a dashboard push
 * happens to arrive -- which on e-paper means an indefinitely empty screen,
 * and the per-minute strip refresh cannot help because it has no cell to
 * paint into yet. Everything here comes from what the device knows on its
 * own: the clock, Wi-Fi, and the departures it polls itself.
 */
static void draw_boot_screen(void)
{
    if (!lvgl_port_lock(2000)) {
        return;
    }

    /* Agenda cell with no calendar data: renders the frame, the "keine
     * Termine" / "nichts offen" hints, and -- crucially -- the transit strip,
     * which is device-side data and therefore already real. */
    widget_agenda_spec_t agenda = {0};
    agenda.title = "Termine & Aufgaben";
    ui_card_slot_rect("top-left", &agenda.x, &agenda.y, &agenda.w, &agenda.h);
    widget_agenda_draw(&agenda);

    /* The other three cells have no device-side source, so they say so
     * instead of showing a blank rectangle. */
    widget_weather_spec_t weather = {0};
    weather.title = "Wetter";
    weather.condition = "";
    ui_card_slot_rect("top-right", &weather.x, &weather.y, &weather.w, &weather.h);
    widget_weather_draw(&weather);

    for (int i = 0; i < 2; i++) {
        chart_spec_t c;
        chart_spec_defaults(&c);
        c.slot_idx = i;
        c.count = 0;
        c.values = NULL;
        c.title = i ? "Strompreis morgen" : "Strompreis heute";
        c.empty_text = "Noch keine Daten";
        c.empty_hint = "warte auf Dashboard-Push";
        ui_card_slot_rect(i ? "bottom-right" : "bottom-left", &c.x, &c.y, &c.w, &c.h);
        chart_draw(&c);
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "boot screen drawn");
}

static void ui_tick_task(void *arg)
{
    (void)arg;
    int last_min = -1;
    int last_quarter = -1;

    /* Give SNTP and the first departure fetch a moment, so the boot screen
     * shows real times rather than placeholders it would immediately replace. */
    vTaskDelay(pdMS_TO_TICKS(12000));
    draw_boot_screen();

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
