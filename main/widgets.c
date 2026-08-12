#include "widgets.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ui_card.h"

static const char *TAG = "widgets";

/* ---------------------------------------------------------------- list --- */

esp_err_t widget_list_draw(const widget_list_spec_t *spec)
{
    if (!spec || !spec->items || spec->count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ui_card_t card;
    esp_err_t err = ui_card_begin(&card, spec->x, spec->y, spec->w, spec->h, spec->title);
    if (err != ESP_OK) {
        return err;
    }

    int count = spec->count > WIDGET_LIST_MAX_ITEMS ? WIDGET_LIST_MAX_ITEMS : spec->count;

    /* Reserve the last line for the summary, then distribute what is left. */
    const int summary_h = 22;
    int avail = card.ch - summary_h;
    int row_h = count > 0 ? avail / count : avail;
    if (row_h > 34) row_h = 34;
    if (row_h < 16) row_h = 16;

    const int box = 14;
    int done_count = 0;

    for (int i = 0; i < count; i++) {
        const widget_list_item_t *it = &spec->items[i];
        int y = i * row_h;
        if (y + row_h > avail) {
            break;
        }
        int box_y = y + (row_h - box) / 2;

        /* Checkbox: 2px frame drawn as four rects (canvas has no outline-only
         * primitive that respects our content offset cleanly). */
        ui_card_rect(&card, 0, box_y, box, 2, UI_BLACK);
        ui_card_rect(&card, 0, box_y + box - 2, box, 2, UI_BLACK);
        ui_card_rect(&card, 0, box_y, 2, box, UI_BLACK);
        ui_card_rect(&card, box - 2, box_y, 2, box, UI_BLACK);

        if (it->done) {
            done_count++;
            /* Filled block reads as "checked" at this size far better than a
             * two-stroke tick, which the panel would smear. */
            ui_card_rect(&card, 3, box_y + 3, box - 6, box - 6, UI_BLACK);
        }

        ui_card_text(&card, box + 10, y + (row_h - 16) / 2, card.cw - box - 10,
                     it->text, it->done ? UI_MID : UI_BLACK, &lv_font_montserrat_14);

        if (it->done) {
            /* Strike-through. Width is approximated from the glyph count --
             * exact text metrics would need a draw pass we do not have here. */
            int approx = (int)strlen(it->text) * 7;
            if (approx > card.cw - box - 10) approx = card.cw - box - 10;
            ui_card_rect(&card, box + 10, y + row_h / 2, approx, 1, UI_MID);
        }
    }

    ui_card_rect(&card, 0, avail + 2, card.cw, 1, UI_LIGHT);
    char summary[48];
    snprintf(summary, sizeof(summary), "%d von %d erledigt", done_count, count);
    ui_card_text(&card, 0, avail + 6, card.cw, summary, UI_MID, &lv_font_montserrat_14);

    ESP_LOGI(TAG, "list '%s': %d items, %d done", spec->title, count, done_count);
    return ESP_OK;
}

/* ------------------------------------------------------------- weather --- */

/* Home Assistant condition slugs -> a coarse class we can actually draw. */
typedef enum { ICON_SUN, ICON_PARTLY, ICON_CLOUD, ICON_RAIN, ICON_SNOW, ICON_FOG } icon_t;

static icon_t icon_for(const char *cond)
{
    if (!cond) return ICON_CLOUD;
    if (strstr(cond, "sunny") || strstr(cond, "clear")) return ICON_SUN;
    if (strstr(cond, "partly")) return ICON_PARTLY;
    if (strstr(cond, "snow")) return ICON_SNOW;
    if (strstr(cond, "rain") || strstr(cond, "pour")) return ICON_RAIN;
    if (strstr(cond, "light") || strstr(cond, "storm")) return ICON_RAIN;
    if (strstr(cond, "fog")) return ICON_FOG;
    return ICON_CLOUD;
}

static const char *icon_label(icon_t ic)
{
    switch (ic) {
        case ICON_SUN:    return "sonnig";
        case ICON_PARTLY: return "wechselnd";
        case ICON_RAIN:   return "Regen";
        case ICON_SNOW:   return "Schnee";
        case ICON_FOG:    return "Nebel";
        default:          return "bewoelkt";
    }
}

/* Filled disc via a rect with circular radius. */
static void disc(ui_card_t *card, int cx, int cy, int r, lv_color_t color)
{
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;
    rect.bg_color = color;
    rect.radius = LV_RADIUS_CIRCLE;
    rect.border_width = 0;
    lv_canvas_draw_rect(card->canvas, card->cx0 + cx - r, card->cy0 + cy - r,
                        2 * r, 2 * r, &rect);
}

static void draw_icon(ui_card_t *card, icon_t ic, int cx, int cy)
{
    switch (ic) {
        case ICON_SUN:
            disc(card, cx, cy, 20, UI_BLACK);
            disc(card, cx, cy, 15, UI_WHITE);
            for (int i = 0; i < 4; i++) {
                int len = 10, off = 26;
                if (i == 0) ui_card_rect(card, cx - 1, cy - off - len, 3, len, UI_BLACK);
                if (i == 1) ui_card_rect(card, cx - 1, cy + off, 3, len, UI_BLACK);
                if (i == 2) ui_card_rect(card, cx - off - len, cy - 1, len, 3, UI_BLACK);
                if (i == 3) ui_card_rect(card, cx + off, cy - 1, len, 3, UI_BLACK);
            }
            break;
        case ICON_PARTLY:
            disc(card, cx - 8, cy - 8, 16, UI_BLACK);
            disc(card, cx - 8, cy - 8, 11, UI_WHITE);
            /* fall through to draw the cloud on top */
            __attribute__((fallthrough));
        case ICON_CLOUD:
        case ICON_FOG:
            disc(card, cx - 12, cy + 8, 14, UI_BLACK);
            disc(card, cx + 8, cy + 6, 17, UI_BLACK);
            disc(card, cx - 12, cy + 8, 11, UI_WHITE);
            disc(card, cx + 8, cy + 6, 14, UI_WHITE);
            ui_card_rect(card, cx - 24, cy + 18, 46, 3, UI_BLACK);
            if (ic == ICON_FOG) {
                ui_card_rect(card, cx - 22, cy + 26, 44, 2, UI_MID);
                ui_card_rect(card, cx - 16, cy + 32, 38, 2, UI_MID);
            }
            break;
        case ICON_RAIN:
        case ICON_SNOW:
            disc(card, cx - 12, cy + 2, 14, UI_BLACK);
            disc(card, cx + 8, cy, 17, UI_BLACK);
            disc(card, cx - 12, cy + 2, 11, UI_WHITE);
            disc(card, cx + 8, cy, 14, UI_WHITE);
            ui_card_rect(card, cx - 24, cy + 12, 46, 3, UI_BLACK);
            for (int i = 0; i < 3; i++) {
                int dx = cx - 16 + i * 15;
                if (ic == ICON_RAIN) {
                    ui_card_rect(card, dx, cy + 20, 2, 9, UI_DARK);
                } else {
                    ui_card_rect(card, dx, cy + 22, 5, 5, UI_DARK);
                }
            }
            break;
    }
}

/* Filled-area sparkline with auto-scaled y range; min/max are annotated so
 * the curve shape stays interpretable without gridlines. */
static void spark_area(ui_card_t *card, int x, int y, int w, int h,
                       const float *v, int n, const char *title, const char *unit)
{
    ui_card_text(card, x, y, w, title, UI_MID, &lv_font_montserrat_14);
    int py = y + 18;
    int ph = h - 18;
    if (n < 2 || ph < 10) {
        return;
    }

    float mn = v[0], mx = v[0];
    for (int i = 0; i < n; i++) {
        if (v[i] < mn) mn = v[i];
        if (v[i] > mx) mx = v[i];
    }
    float span = mx - mn;
    if (span < 1.0f) span = 1.0f;          /* flat curve -> centre it */
    float lo = mn - span * 0.15f;
    float hi = mx + span * 0.15f;

    ui_card_rect(card, x, py + ph, w, 1, UI_LIGHT);

    float step = (float)w / (n - 1);
    for (int i = 0; i < n; i++) {
        int cx = x + (int)(i * step);
        int cy = py + ph - (int)((v[i] - lo) / (hi - lo) * ph);
        /* Column fill reads better than a 1px polyline after the panel's
         * 4bpp squash. */
        ui_card_rect(card, cx, cy, 2, py + ph - cy, UI_LIGHT);
        ui_card_rect(card, cx, cy, 2, 2, UI_BLACK);
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f%s", mx, unit);
    ui_card_text(card, x + w - 42, py - 2, 44, buf, UI_DARK, &lv_font_montserrat_14);
    snprintf(buf, sizeof(buf), "%.0f%s", mn, unit);
    ui_card_text(card, x + w - 42, py + ph - 16, 44, buf, UI_DARK, &lv_font_montserrat_14);
}

/* Bar sparkline on a fixed 0..100 scale (probabilities). */
static void spark_bars(ui_card_t *card, int x, int y, int w, int h,
                       const float *v, int n, const char *title)
{
    ui_card_text(card, x, y, w, title, UI_MID, &lv_font_montserrat_14);
    int py = y + 18;
    int ph = h - 18;
    if (n < 1 || ph < 10) {
        return;
    }

    /* 50% guide line: without it a bar chart with no axis is hard to read. */
    for (int dx = 0; dx < w; dx += 6) {
        ui_card_rect(card, x + dx, py + ph / 2, 3, 1, UI_LIGHT);
    }
    ui_card_rect(card, x, py + ph, w, 1, UI_LIGHT);

    float step = (float)w / n;
    int bw = (int)(step * 0.8f);
    if (bw < 1) bw = 1;
    for (int i = 0; i < n; i++) {
        float p = v[i];
        if (p < 0.0f) p = 0.0f;
        if (p > 100.0f) p = 100.0f;
        int bh = (int)(p / 100.0f * ph);
        int bx = x + (int)(i * step);
        if (bh < 1) {
            /* Draw a hairline for 0% so the slot is visibly "no rain" rather
             * than looking like missing data. */
            ui_card_rect(card, bx, py + ph - 1, bw, 1, UI_LIGHT);
        } else {
            ui_card_rect(card, bx, py + ph - bh, bw, bh, p >= 50.0f ? UI_DARK : UI_MID);
        }
    }

    ui_card_text(card, x + w - 34, py - 2, 36, "100%", UI_DARK, &lv_font_montserrat_14);
}

esp_err_t widget_weather_draw(const widget_weather_spec_t *spec)
{
    if (!spec) {
        return ESP_ERR_INVALID_ARG;
    }

    ui_card_t card;
    esp_err_t err = ui_card_begin(&card, spec->x, spec->y, spec->w, spec->h, spec->title);
    if (err != ESP_OK) {
        return err;
    }

    icon_t ic = icon_for(spec->condition);

    /* Two columns: the left carries the "now" block, the right the 24h
     * sparklines that used to be dead space. */
    const int col_r = card.cw / 2 + 8;
    const int col_r_w = card.cw - col_r;

    draw_icon(&card, ic, 40, 40);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f C", spec->temp);
    ui_card_text(&card, 92, 4, col_r - 92, buf, UI_BLACK, &lv_font_montserrat_40);
    ui_card_text(&card, 94, 52, col_r - 94, icon_label(ic), UI_BLACK,
                 &lv_font_montserrat_14);

    /* Key/value strip, anchored to the content top so it cannot collide with
     * the outlook row below. */
    int ky = 84;
    const int kv_step = 19;
    if (spec->has_range) {
        snprintf(buf, sizeof(buf), "%.0f / %.0f C", spec->temp_min, spec->temp_max);
        ui_card_text(&card, 0, ky, 96, "Min/Max", UI_MID, &lv_font_montserrat_14);
        ui_card_text(&card, 100, ky, 110, buf, UI_BLACK, &lv_font_montserrat_14);
        ky += kv_step;
    }
    if (spec->has_precip_prob) {
        snprintf(buf, sizeof(buf), "%.0f %%", spec->precip_prob);
        ui_card_text(&card, 0, ky, 96, "Regenrisiko", UI_MID, &lv_font_montserrat_14);
        ui_card_text(&card, 100, ky, 110, buf, UI_BLACK, &lv_font_montserrat_14);
        ky += kv_step;
    }
    snprintf(buf, sizeof(buf), "%.1f mm", spec->precip);
    ui_card_text(&card, 0, ky, 96, "Regen 24h", UI_MID, &lv_font_montserrat_14);
    ui_card_text(&card, 100, ky, 110, buf, UI_BLACK, &lv_font_montserrat_14);
    ky += kv_step;

    snprintf(buf, sizeof(buf), "%.0f km/h", spec->wind);
    ui_card_text(&card, 0, ky, 96, "Wind", UI_MID, &lv_font_montserrat_14);
    ui_card_text(&card, 100, ky, 110, buf, UI_BLACK, &lv_font_montserrat_14);

    /* --- right column: 24h sparklines --- */
    const int spark_h = 76;
    if (spec->temp_series && spec->temp_series_count >= 2) {
        spark_area(&card, col_r, 0, col_r_w, spark_h,
                   spec->temp_series, spec->temp_series_count, "Temp 24h", "");
    }
    if (spec->pop_series && spec->pop_series_count >= 1) {
        spark_bars(&card, col_r, spark_h + 10, col_r_w, spark_h,
                   spec->pop_series, spec->pop_series_count, "Regenrisiko 24h");
    }

    /* --- bottom: concrete look-ahead numbers, full width --- */
    if (spec->forecast && spec->forecast_count > 0) {
        int n = spec->forecast_count > WIDGET_FORECAST_MAX
                    ? WIDGET_FORECAST_MAX : spec->forecast_count;
        int fy = card.ch - 40;
        ui_card_rect(&card, 0, fy - 6, card.cw, 1, UI_LIGHT);
        int step = card.cw / n;
        for (int i = 0; i < n; i++) {
            int sx = i * step;
            ui_card_text(&card, sx, fy, step, spec->forecast[i].label,
                         UI_MID, &lv_font_montserrat_14);
            snprintf(buf, sizeof(buf), "%.0f C", spec->forecast[i].temp);
            ui_card_text(&card, sx + 34, fy, step - 34, buf, UI_BLACK,
                         &lv_font_montserrat_18);
        }
    }

    ESP_LOGI(TAG, "weather '%s': %s %.1fC, %d slots",
             spec->title, spec->condition ? spec->condition : "?",
             spec->temp, spec->forecast_count);
    return ESP_OK;
}
