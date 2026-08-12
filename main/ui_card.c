#include "ui_card.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ui_card";

/*
 * Canvases are cached per screen position and reused.
 *
 * Without this every dashboard push created a fresh canvas stacked on top of
 * the previous one: it leaked an LVGL object plus its PSRAM buffer each time,
 * and -- worse -- it buried older objects. The transit strip created at boot
 * ended up underneath the newer agenda card, so repainting it once a minute
 * had no visible effect at all.
 */
#define UI_CARD_CACHE 6

typedef struct {
    int x, y, w, h;
    lv_obj_t *canvas;
    lv_color_t *buf;
    bool used;
} card_slot_t;

static card_slot_t s_slots[UI_CARD_CACHE];

static card_slot_t *slot_for(int x, int y, int w, int h)
{
    for (int i = 0; i < UI_CARD_CACHE; i++) {
        if (s_slots[i].used && s_slots[i].x == x && s_slots[i].y == y) {
            if (s_slots[i].w == w && s_slots[i].h == h) {
                return &s_slots[i];   /* reuse as-is */
            }
            /* Same position, different size -- drop and rebuild. */
            lv_obj_del(s_slots[i].canvas);
            heap_caps_free(s_slots[i].buf);
            s_slots[i].used = false;
        }
    }
    for (int i = 0; i < UI_CARD_CACHE; i++) {
        if (!s_slots[i].used) {
            return &s_slots[i];
        }
    }
    return NULL;
}

#define PANEL_W 960
#define PANEL_H 540

void ui_card_slot_rect(const char *slot, int *x, int *y, int *w, int *h)
{
    const int m = CONFIG_APP_CHART_MARGIN;
    const int half_w = (PANEL_W - 3 * m) / 2;
    const int half_h = (PANEL_H - 3 * m) / 2;

    bool right = slot && strstr(slot, "right") != NULL;
    bool bottom = slot && strstr(slot, "bottom") != NULL;

    *x = right ? (m * 2 + half_w) : m;
    *y = bottom ? (m * 2 + half_h) : m;
    *w = half_w;
    *h = half_h;
}

esp_err_t ui_card_begin(ui_card_t *card, int x, int y, int w, int h, const char *title)
{
    if (!card || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(card, 0, sizeof(*card));

    card_slot_t *slot = slot_for(x, y, w, h);
    if (!slot) {
        ESP_LOGE(TAG, "no free canvas slot for (%d,%d)", x, y);
        return ESP_ERR_NO_MEM;
    }

    if (!slot->used) {
        size_t buf_bytes = (size_t)w * h * sizeof(lv_color_t);
        slot->buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
        if (!slot->buf) {
            ESP_LOGE(TAG, "canvas alloc failed (%u bytes)", (unsigned)buf_bytes);
            return ESP_ERR_NO_MEM;
        }
        slot->canvas = lv_canvas_create(lv_scr_act());
        if (!slot->canvas) {
            heap_caps_free(slot->buf);
            slot->buf = NULL;
            return ESP_ERR_NO_MEM;
        }
        /* Position BEFORE giving the object any size or content. LVGL creates
         * objects at (0,0), and lv_obj_set_pos() invalidates both the old and
         * the new area -- so positioning last makes every widget dirty the
         * region back to the screen origin, defeating partial refresh. */
        lv_obj_set_pos(slot->canvas, x, y);
        lv_canvas_set_buffer(slot->canvas, slot->buf, w, h, LV_IMG_CF_TRUE_COLOR);
        slot->x = x; slot->y = y; slot->w = w; slot->h = h;
        slot->used = true;
    }

    card->canvas = slot->canvas;
    card->buf = slot->buf;
    lv_canvas_fill_bg(card->canvas, UI_WHITE, LV_OPA_COVER);

    card->w = w;
    card->h = h;

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;
    rect.bg_color = UI_WHITE;
    rect.border_color = UI_BLACK;
    rect.border_width = 2;
    rect.border_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(card->canvas, 0, 0, w, h, &rect);

    rect.border_width = 0;
    rect.bg_color = UI_BLACK;
    lv_canvas_draw_rect(card->canvas, 2, 2, w - 4, UI_TITLE_H, &rect);

    ui_text_ex(card->canvas, 10, 5, w - 20, title ? title : "",
               UI_WHITE, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT, true);

    card->cx0 = 10;
    card->cy0 = UI_TITLE_H + 6;
    card->cw = w - 20;
    card->ch = h - card->cy0 - 8;
    return ESP_OK;
}

void ui_text_ex(lv_obj_t *canvas, int x, int y, int max_w, const char *txt,
                lv_color_t color, const lv_font_t *font,
                lv_text_align_t align, bool bold)
{
    if (!canvas || !txt) {
        return;
    }
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = font ? font : &lv_font_montserrat_14;
    label.color = color;
    label.align = align;

    lv_canvas_draw_text(canvas, x, y, max_w, &label, txt);
    if (bold) {
        /* One horizontal and one vertical repeat. Horizontal alone leaves the
         * horizontal strokes thin, which is exactly where white-on-black loses
         * definition on this panel. */
        lv_canvas_draw_text(canvas, x + 1, y, max_w, &label, txt);
        lv_canvas_draw_text(canvas, x, y + 1, max_w, &label, txt);
    }
}

int ui_card_badge(ui_card_t *card, int x, int y, int h, const char *label)
{
    if (!card || !card->canvas || !label) {
        return 0;
    }
    const lv_font_t *font = &lv_font_montserrat_14;
    int tw = lv_txt_get_width(label, strlen(label), font, 0, LV_TEXT_FLAG_NONE);
    int w = tw + 12;
    if (w < h) {
        w = h; /* keep single characters square rather than letting them squash */
    }

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;
    rect.bg_color = UI_BLACK;
    rect.radius = 4;
    rect.border_width = 0;
    lv_canvas_draw_rect(card->canvas, card->cx0 + x, card->cy0 + y, w, h, &rect);

    /* Bold, because knocked-out white text on black loses definition on
     * e-paper at this size. */
    ui_text_ex(card->canvas, card->cx0 + x, card->cy0 + y + (h - 16) / 2, w,
               label, UI_WHITE, font, LV_TEXT_ALIGN_CENTER, true);
    return w;
}

void ui_card_rect(ui_card_t *card, int x, int y, int w, int h, lv_color_t color)
{
    if (!card || !card->canvas || w <= 0 || h <= 0) {
        return;
    }
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;
    rect.bg_color = color;
    rect.border_width = 0;
    lv_canvas_draw_rect(card->canvas, card->cx0 + x, card->cy0 + y, w, h, &rect);
}

void ui_card_text(ui_card_t *card, int x, int y, int max_w, const char *txt,
                  lv_color_t color, const lv_font_t *font)
{
    if (!card || !card->canvas || !txt) {
        return;
    }
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = font ? font : &lv_font_montserrat_14;
    label.color = color;
    lv_canvas_draw_text(card->canvas, card->cx0 + x, card->cy0 + y, max_w, &label, txt);
}
