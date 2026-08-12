#include "chart.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "time_sync.h"
#include "ui_card.h"

static const char *TAG = "chart";

#define CHART_SLOTS 2

/* Canvas is reused per slot: chart_redraw() runs every quarter hour, and
 * creating a fresh canvas each time would leak both an LVGL object and its
 * PSRAM buffer. Also stores the data so a redraw needs no client round-trip. */
static lv_obj_t *s_canvas[CHART_SLOTS];
static lv_color_t *s_cbuf[CHART_SLOTS];
static int s_cw[CHART_SLOTS], s_ch[CHART_SLOTS];
static float s_vals[CHART_SLOTS][CHART_MAX_SLOTS];
static chart_spec_t s_spec[CHART_SLOTS];
static char s_title[CHART_SLOTS][64];
static char s_empty_text[CHART_SLOTS][48];
static char s_empty_hint[CHART_SLOTS][64];
static bool s_have[CHART_SLOTS];

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
    spec->y_max = (float)CONFIG_APP_CHART_Y_MAX;
    spec->y_min_neg = (float)CONFIG_APP_CHART_Y_MIN_NEG;
    spec->highlight_now = true;
    /* Bottom-left cell of the 2x2 dashboard grid. */
    spec->x = CONFIG_APP_CHART_MARGIN;
    spec->y = 540 / 2 + CONFIG_APP_CHART_MARGIN / 2;
    spec->w = (960 - 3 * CONFIG_APP_CHART_MARGIN) / 2;
    spec->h = 540 / 2 - CONFIG_APP_CHART_MARGIN - CONFIG_APP_CHART_MARGIN / 2;
}

static esp_err_t chart_draw_internal(const chart_spec_t *spec)
{
    /* count == 0 is explicitly allowed and renders a "no data yet" card:
     * tomorrow's day-ahead prices do not exist before ~13:00, and an empty
     * framed card communicates that far better than a missing cell. */
    if (!spec || spec->count < 0 || spec->count > CHART_MAX_SLOTS ||
        spec->w <= 0 || spec->h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (spec->count > 0 && !spec->values) {
        return ESP_ERR_INVALID_ARG;
    }

    const int w = spec->w, h = spec->h;
    int idx = spec->slot_idx;
    if (idx < 0 || idx >= CHART_SLOTS) {
        idx = 0;
    }

    /* Reuse this slot's canvas when the geometry is unchanged. */
    if (s_canvas[idx] && (s_cw[idx] != w || s_ch[idx] != h)) {
        lv_obj_del(s_canvas[idx]);
        heap_caps_free(s_cbuf[idx]);
        s_canvas[idx] = NULL;
        s_cbuf[idx] = NULL;
    }
    if (!s_canvas[idx]) {
        size_t buf_bytes = (size_t)w * h * sizeof(lv_color_t);
        s_cbuf[idx] = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
        if (!s_cbuf[idx]) {
            ESP_LOGE(TAG, "canvas alloc failed (%u bytes)", (unsigned)buf_bytes);
            return ESP_ERR_NO_MEM;
        }
        s_canvas[idx] = lv_canvas_create(lv_scr_act());
        if (!s_canvas[idx]) {
            heap_caps_free(s_cbuf[idx]);
            s_cbuf[idx] = NULL;
            return ESP_ERR_NO_MEM;
        }
        /* Position before content -- see the note in ui_card_begin(). */
        lv_obj_set_pos(s_canvas[idx], spec->x, spec->y);
        lv_canvas_set_buffer(s_canvas[idx], s_cbuf[idx], w, h, LV_IMG_CF_TRUE_COLOR);
        s_cw[idx] = w;
        s_ch[idx] = h;
    }
    lv_obj_t *canvas = s_canvas[idx];
    lv_canvas_fill_bg(canvas, C_WHITE, LV_OPA_COVER);

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

    if (spec->count == 0) {
        const int cy = TITLE_H + (h - TITLE_H) / 2;
        ui_text_ex(canvas, 0, cy - 26, w, spec->empty_text ? spec->empty_text
                                                           : "Noch keine Daten",
                   C_BLACK, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER, true);
        if (spec->empty_hint) {
            ui_text_ex(canvas, 0, cy + 2, w, spec->empty_hint, C_MID,
                       &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER, false);
        }
        ESP_LOGI(TAG, "chart '%s': no data", spec->title);
        return ESP_OK;
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
     * box. The ceiling is a decision threshold rather than a display range --
     * above it nothing large should be switched on regardless of the actual
     * price, so clipping loses no actionable information. The title bar still
     * shows the true current value. Guard against a degenerate range coming
     * in over REST. */
    /* Lower bound is data-dependent: stay at 0 on an ordinary day so the full
     * plot height is spent on the range that matters, and only open up the
     * negative band when prices actually go negative. The floor is capped
     * (y_min_neg, default -10) for the same reason the ceiling is capped --
     * once it is deeply negative the answer is "switch everything on", and
     * how far below it goes changes nothing. */
    float hi = spec->y_max;
    float lo = (mn < 0.0f) ? spec->y_min_neg : 0.0f;
    if (lo > 0.0f) {
        lo = 0.0f;
    }
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
        return ESP_ERR_INVALID_ARG;
    }

    char caption[64];
    snprintf(caption, sizeof(caption), "avg %.1f  min %.0f  max %.0f %s",
             avg, mn, mx, spec->unit ? spec->unit : "");
    lv_canvas_draw_text(canvas, plot_x0, TITLE_H + 4, plot_w, &label, caption);

    /* Pixel row of the zero line -- the baseline every bar grows from. With
     * lo == 0 this is the bottom of the plot, i.e. identical to the previous
     * behaviour. */
    const int zero_y = plot_y1 - (int)((0.0f - lo) / (hi - lo) * plot_h);

    /* --- horizontal grid + y labels --- */
    rect.bg_color = C_LIGHT;
    for (int k = 0; k <= 3; k++) {
        float frac = k / 3.0f;
        int gy = plot_y1 - (int)(frac * plot_h);
        if (lo < 0.0f && abs(gy - zero_y) < 10) {
            continue; /* would collide with the zero line drawn below */
        }
        lv_canvas_draw_rect(canvas, plot_x0, gy, plot_w, 1, &rect);

        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d", (int)(lo + frac * (hi - lo) + 0.5f));
        lv_canvas_draw_text(canvas, 2, gy - 8, AXIS_LEFT - 4, &label, lbl);
    }

    if (lo < 0.0f) {
        /* Zero gets its own labelled line: with a negative band present it is
         * the reference that decides "costs money" vs "pays you". */
        rect.bg_color = C_MID;
        lv_canvas_draw_rect(canvas, plot_x0, zero_y, plot_w, 1, &rect);
        lv_canvas_draw_text(canvas, 2, zero_y - 8, AXIS_LEFT - 4, &label, "0");
    }

    /* --- bars --- */

    float slot_w = (float)plot_w / spec->count;
    int bar_w = (int)(slot_w * 0.8f);
    if (bar_w < 1) bar_w = 1;

    for (int i = 0; i < spec->count; i++) {
        float v = spec->values[i];
        bool over = v > hi;
        bool under = v < lo;
        float vc = v;
        if (vc > hi) vc = hi;
        if (vc < lo) vc = lo;

        int vy = plot_y1 - (int)((vc - lo) / (hi - lo) * plot_h);
        int bx = plot_x0 + (int)(i * slot_w);

        /* Grow from the zero line: upward when the price is positive,
         * downward when it is negative. */
        int by, bh;
        if (vy <= zero_y) {
            by = vy;
            bh = zero_y - vy;
        } else {
            by = zero_y;
            bh = vy - zero_y;
        }
        if (bh < 1) bh = 1;

        /* Cheap slots light, expensive slots dark -- legible without colour.
         * Negative prices get the lightest fill plus an outline: they are the
         * "switch everything on" case and should stand out from merely cheap.
         * The avg-relative thresholds are only meaningful for a positive
         * average, so fall back to absolute ones otherwise. */
        rect.border_width = 0;
        if (v < 0.0f) {
            rect.bg_color = C_WHITE;
        } else if (avg > 0.0f) {
            rect.bg_color = (v < avg * 0.85f) ? C_LIGHT : (v < avg * 1.15f ? C_MID : C_DARK);
        } else {
            rect.bg_color = (v < hi * 0.33f) ? C_LIGHT : (v < hi * 0.66f ? C_MID : C_DARK);
        }
        lv_canvas_draw_rect(canvas, bx, by, bar_w, bh, &rect);

        if (v < 0.0f) {
            /* Outline, otherwise a white bar on white background is invisible. */
            rect.bg_color = C_BLACK;
            lv_canvas_draw_rect(canvas, bx, by + bh - 1, bar_w, 1, &rect);
            lv_canvas_draw_rect(canvas, bx, by, 1, bh, &rect);
            lv_canvas_draw_rect(canvas, bx + bar_w - 1, by, 1, bh, &rect);
        }

        if (over) {
            /* Do not let an out-of-range value masquerade as exactly y_max. */
            rect.bg_color = C_BLACK;
            lv_canvas_draw_rect(canvas, bx, plot_y0 - 3, bar_w, 2, &rect);
        }
        if (under) {
            /* Same, at the other end of the scale. */
            rect.bg_color = C_BLACK;
            lv_canvas_draw_rect(canvas, bx, plot_y1 + 1, bar_w, 2, &rect);
        }

        if (i == now_slot) {
            /* Outline the current slot and run a marker along it so it stays
             * findable even when the bar itself is short. */
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
    lv_canvas_draw_rect(canvas, plot_x0, zero_y, plot_w, 2, &rect);

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


esp_err_t chart_draw(const chart_spec_t *spec)
{
    if (!spec) {
        return ESP_ERR_INVALID_ARG;
    }
    int idx = spec->slot_idx;
    if (idx < 0 || idx >= CHART_SLOTS) {
        idx = 0;
    }

    /* Keep a private copy so chart_redraw() can re-render on the quarter hour
     * without the client having to re-send the curve. The spec carries
     * pointers into caller-owned buffers, so the strings are copied too. */
    int n = spec->count > CHART_MAX_SLOTS ? CHART_MAX_SLOTS : spec->count;
    if (n > 0 && spec->values) {
        memcpy(s_vals[idx], spec->values, (size_t)n * sizeof(float));
    }
    s_spec[idx] = *spec;
    s_spec[idx].count = n;
    s_spec[idx].values = n > 0 ? s_vals[idx] : NULL;
    strlcpy(s_title[idx], spec->title ? spec->title : "", sizeof(s_title[idx]));
    s_spec[idx].title = s_title[idx];
    strlcpy(s_empty_text[idx], spec->empty_text ? spec->empty_text : "",
            sizeof(s_empty_text[idx]));
    s_spec[idx].empty_text = s_empty_text[idx][0] ? s_empty_text[idx] : NULL;
    strlcpy(s_empty_hint[idx], spec->empty_hint ? spec->empty_hint : "",
            sizeof(s_empty_hint[idx]));
    s_spec[idx].empty_hint = s_empty_hint[idx][0] ? s_empty_hint[idx] : NULL;
    s_spec[idx].unit = "ct/kWh";
    s_have[idx] = true;

    return chart_draw_internal(&s_spec[idx]);
}

esp_err_t chart_redraw(int slot_idx)
{
    if (slot_idx < 0 || slot_idx >= CHART_SLOTS || !s_have[slot_idx]) {
        return ESP_ERR_INVALID_STATE;
    }
    return chart_draw_internal(&s_spec[slot_idx]);
}
