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
    draw_icon(&card, ic, 46, 44);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f C", spec->temp);
    ui_card_text(&card, 116, 6, 160, buf, UI_BLACK, &lv_font_montserrat_40);
    ui_card_text(&card, 118, 56, card.cw - 118, icon_label(ic), UI_BLACK,
                 &lv_font_montserrat_14);

    /* Key/value strip. Anchored to the content top so it cannot collide with
     * the outlook row below (a bug the Pillow version had). */
    int ky = 96;
    if (spec->has_range) {
        snprintf(buf, sizeof(buf), "%.0f / %.0f C", spec->temp_min, spec->temp_max);
        ui_card_text(&card, 0, ky, 110, "Min / Max", UI_MID, &lv_font_montserrat_14);
        ui_card_text(&card, 118, ky, 120, buf, UI_BLACK, &lv_font_montserrat_14);
        ky += 20;
    }
    snprintf(buf, sizeof(buf), "%.1f mm", spec->precip);
    ui_card_text(&card, 0, ky, 110, "Niederschlag", UI_MID, &lv_font_montserrat_14);
    ui_card_text(&card, 118, ky, 120, buf, UI_BLACK, &lv_font_montserrat_14);
    ky += 20;

    snprintf(buf, sizeof(buf), "%.0f km/h", spec->wind);
    ui_card_text(&card, 0, ky, 110, "Wind", UI_MID, &lv_font_montserrat_14);
    ui_card_text(&card, 118, ky, 120, buf, UI_BLACK, &lv_font_montserrat_14);
    ky += 26;

    if (spec->forecast && spec->forecast_count > 0) {
        int n = spec->forecast_count > WIDGET_FORECAST_MAX
                    ? WIDGET_FORECAST_MAX : spec->forecast_count;
        ui_card_rect(&card, 0, ky, card.cw, 1, UI_LIGHT);
        int step = card.cw / n;
        for (int i = 0; i < n; i++) {
            int sx = i * step;
            ui_card_text(&card, sx, ky + 6, step, spec->forecast[i].label,
                         UI_MID, &lv_font_montserrat_14);
            snprintf(buf, sizeof(buf), "%.0f C", spec->forecast[i].temp);
            ui_card_text(&card, sx, ky + 24, step, buf, UI_BLACK,
                         &lv_font_montserrat_18);
        }
    }

    ESP_LOGI(TAG, "weather '%s': %s %.1fC, %d slots",
             spec->title, spec->condition ? spec->condition : "?",
             spec->temp, spec->forecast_count);
    return ESP_OK;
}
