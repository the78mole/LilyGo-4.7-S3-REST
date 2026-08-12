#include "chart.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "time_sync.h"
#include "ui_card.h"

static const char *TAG = "chart";

#define C_BLACK UI_BLACK
#define C_DARK  UI_DARK
#define C_MID   UI_MID
#define C_LIGHT UI_LIGHT
#define C_WHITE UI_WHITE

#define TITLE_H     UI_TITLE_H
#define AXIS_LEFT   30
#define AXIS_BOTTOM 16
#define CAPTION_H   14

void chart_spec_defaults(chart_spec_t *spec)
{
    if (!spec) {
        return;
    }
    memset(spec, 0, sizeof(*spec));
    spec->title = "Strompreis";
    spec->unit = "ct/kWh";
    spec->interval_min = CONFIG_APP_CHART_INTERVAL_MIN;
    spec->y_min = 0.0f;
    spec->y_max = (float)CONFIG_APP_CHART_Y_MAX;
    spec->highlight_now = true;
    /* Bottom-left cell of the 2x2 dashboard grid. */
    spec->x = CONFIG_APP_CHART_MARGIN;
    spec->y = 540 / 2 + CONFIG_APP_CHART_MARGIN / 2;
    spec->w = (960 - 3 * CONFIG_APP_CHART_MARGIN) / 2;
    spec->h = 540 / 2 - CONFIG_APP_CHART_MARGIN - CONFIG_APP_CHART_MARGIN / 2;
}

esp_err_t chart_draw(const chart_spec_t *spec)
{
    if (!spec || !spec->values || spec->count <= 0 ||
        spec->count > CHART_MAX_SLOTS || spec->w <= 0 || spec->h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int w = spec->w, h = spec->h;

    /* One canvas buffer per chart, kept alive for the object's lifetime (see
     * the lifecycle note in api_handlers.c -- nothing evicts these yet). */
    size_t buf_bytes = (size_t)w * h * sizeof(lv_color_t);
    lv_color_t *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "canvas alloc failed (%u bytes)", (unsigned)buf_bytes);
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *canvas = lv_canvas_create(lv_scr_act());
    if (!canvas) {
        heap_caps_free(buf);
        return ESP_ERR_NO_MEM;
    }
    lv_canvas_set_buffer(canvas, buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(canvas, C_WHITE, LV_OPA_COVER);
    lv_obj_set_pos(canvas, spec->x, spec->y);

    /* Needed before the title bar is drawn: the current price is shown there. */
    int now_slot = spec->highlight_now ? time_sync_current_slot(spec->interval_min) : -1;
    if (now_slot >= spec->count) {
        now_slot = -1;
    }

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &lv_font_montserrat_14;
    label.color = C_BLACK;

    /* --- frame + title bar (white on black, matching the image cards) --- */
    rect.bg_color = C_WHITE;
    rect.border_color = C_BLACK;
    rect.border_width = 2;
    rect.border_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(canvas, 0, 0, w, h, &rect);

    rect.border_width = 0;
    rect.bg_color = C_BLACK;
    lv_canvas_draw_rect(canvas, 2, 2, w - 4, TITLE_H, &rect);

    ui_text_ex(canvas, 10, 5, w - 20, spec->title, C_WHITE,
               &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT, true);

    /* Current price, right-aligned inside the title bar. Deliberately not
     * placed inside the plot area: the evening peak reaches the top-right
     * corner there and would sit underneath it. */
    if (now_slot >= 0) {
        char nowbuf[32];
        snprintf(nowbuf, sizeof(nowbuf), "jetzt %.1f ct", spec->values[now_slot]);
        ui_text_ex(canvas, w / 2, 4, w / 2 - 12, nowbuf, C_WHITE,
                   &lv_font_montserrat_18, LV_TEXT_ALIGN_RIGHT, true);
    }

    /* --- statistics --- */
    float mn = spec->values[0], mx = spec->values[0], sum = 0.0f;
    for (int i = 0; i < spec->count; i++) {
        float v = spec->values[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    float avg = sum / spec->count;

    /* Fixed axis: comparability across days beats making each chart fill its
     * box. Guard against a degenerate range coming in over REST. */
    float lo = spec->y_min;
    float hi = spec->y_max;
    if (!(hi > lo)) {
        lo = 0.0f;
        hi = (float)CONFIG_APP_CHART_Y_MAX;
    }

    /* --- plot geometry --- */
    const int plot_x0 = AXIS_LEFT;
    const int plot_y0 = TITLE_H + 4 + CAPTION_H;
    const int plot_x1 = w - 6;
    const int plot_y1 = h - 6 - AXIS_BOTTOM;
    const int plot_w = plot_x1 - plot_x0;
    const int plot_h = plot_y1 - plot_y0;
    if (plot_w <= 0 || plot_h <= 0) {
        lv_obj_del(canvas);
        heap_caps_free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    char caption[64];
    snprintf(caption, sizeof(caption), "avg %.1f  min %.0f  max %.0f %s",
             avg, mn, mx, spec->unit ? spec->unit : "");
    lv_canvas_draw_text(canvas, plot_x0, TITLE_H + 4, plot_w, &label, caption);

    /* --- horizontal grid + y labels --- */
    rect.bg_color = C_LIGHT;
    for (int k = 0; k <= 3; k++) {
        float frac = k / 3.0f;
        int gy = plot_y1 - (int)(frac * plot_h);
        lv_canvas_draw_rect(canvas, plot_x0, gy, plot_w, 1, &rect);

        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d", (int)(lo + frac * (hi - lo) + 0.5f));
        lv_canvas_draw_text(canvas, 2, gy - 8, AXIS_LEFT - 4, &label, lbl);
    }

    /* --- bars --- */

    float slot_w = (float)plot_w / spec->count;
    int bar_w = (int)(slot_w * 0.8f);
    if (bar_w < 1) bar_w = 1;

    for (int i = 0; i < spec->count; i++) {
        float v = spec->values[i];
        bool clipped = v > hi;
        float vc = v;
        if (vc > hi) vc = hi;
        if (vc < lo) vc = lo;
        int bh = (int)((vc - lo) / (hi - lo) * plot_h);
        if (bh < 1) bh = 1;
        int bx = plot_x0 + (int)(i * slot_w);
        int by = plot_y1 - bh;

        /* Cheap slots light, expensive slots dark -- legible without colour. */
        rect.bg_color = (v < avg * 0.85f) ? C_LIGHT : (v < avg * 1.15f ? C_MID : C_DARK);
        rect.border_width = 0;
        lv_canvas_draw_rect(canvas, bx, by, bar_w, bh, &rect);

        if (clipped) {
            /* Do not let an out-of-range value masquerade as exactly y_max. */
            rect.bg_color = C_BLACK;
            lv_canvas_draw_rect(canvas, bx, plot_y0 - 3, bar_w, 2, &rect);
        }

        if (i == now_slot) {
            /* Outline the current slot and run a marker up the full height so
             * it stays findable even when the bar itself is short. */
            rect.bg_color = C_BLACK;
            lv_canvas_draw_rect(canvas, bx - 1, by - 3, bar_w + 2, 3, &rect);
            lv_canvas_draw_rect(canvas, bx - 1, by - 3, 1, bh + 3, &rect);
            lv_canvas_draw_rect(canvas, bx + bar_w, by - 3, 1, bh + 3, &rect);
        }
    }

    /* --- average line (dashed) --- */
    rect.bg_color = C_BLACK;
    int ay = plot_y1 - (int)((avg - lo) / (hi - lo) * plot_h);
    for (int dx = plot_x0; dx < plot_x1; dx += 8) {
        lv_canvas_draw_rect(canvas, dx, ay, 4, 1, &rect);
    }

    /* --- baseline + hour ticks --- */
    lv_canvas_draw_rect(canvas, plot_x0, plot_y1, plot_w, 2, &rect);

    int slots_per_hour = (spec->interval_min > 0) ? 60 / spec->interval_min : 1;
    if (slots_per_hour < 1) slots_per_hour = 1;
    int label_every_h = 4;
    for (int hour = 0; hour * slots_per_hour < spec->count; hour += label_every_h) {
        int i = hour * slots_per_hour;
        int lx = plot_x0 + (int)(i * slot_w);
        lv_canvas_draw_rect(canvas, lx, plot_y1 + 2, 1, 3, &rect);
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%02d", hour);
        lv_canvas_draw_text(canvas, lx - 8, plot_y1 + 4, 24, &label, lbl);
    }

    /* --- "now" readout, so an unsynced clock is visible rather than silent --- */
    char hhmm[8];
    time_sync_format_hhmm(hhmm, sizeof(hhmm));
    char nowbuf[32];
    if (spec->highlight_now) {
        snprintf(nowbuf, sizeof(nowbuf), "%s%s", hhmm, now_slot < 0 ? " (no NTP)" : "");
        lv_canvas_draw_text(canvas, plot_x1 - 78, plot_y1 + 4, 78, &label, nowbuf);
    }

    ESP_LOGI(TAG, "chart '%s': %d slots @%dmin, scale %.0f..%.0f, now_slot=%d, now=%.1f, avg=%.1f",
             spec->title, spec->count, spec->interval_min, lo, hi, now_slot,
             now_slot >= 0 ? spec->values[now_slot] : -1.0f, avg);
    return ESP_OK;
}
