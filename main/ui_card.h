#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared building blocks for the dashboard cells.
 *
 * Every cell (chart, task list, weather) is one lv_canvas carrying a titled
 * card. Keeping the frame, palette and 2x2 grid geometry here means the cells
 * stay visually consistent and the individual widgets only worry about their
 * own content.
 */

/* LV_COLOR_DEPTH=8 is RGB332, so only a few greys survive the round trip
 * through lvgl_port's luminance conversion. Use well-separated levels. */
static inline lv_color_t ui_grey(uint8_t v)
{
    return lv_color_make(v, v, v);
}

#define UI_BLACK ui_grey(0)
#define UI_DARK  ui_grey(85)
#define UI_MID   ui_grey(150)
#define UI_LIGHT ui_grey(200)
#define UI_WHITE ui_grey(255)

#define UI_TITLE_H 26

typedef struct {
    lv_obj_t *canvas;
    lv_color_t *buf;
    int w, h;          /* canvas size */
    int cx0, cy0;      /* content origin, i.e. below the title bar */
    int cw, ch;        /* content size */
} ui_card_t;

/* Resolves "top-left" | "top-right" | "bottom-left" | "bottom-right" into the
 * corresponding cell rectangle of the 2x2 grid. Unknown names fall back to
 * top-left. */
void ui_card_slot_rect(const char *slot, int *x, int *y, int *w, int *h);

/*
 * Creates the canvas (PSRAM-backed), paints the frame and the inverted title
 * bar, and fills in the content box. Caller must hold the LVGL lock.
 * On failure `card->canvas` is NULL and nothing was allocated.
 */
esp_err_t ui_card_begin(ui_card_t *card, int x, int y, int w, int h, const char *title);

/* Convenience wrappers operating in *content* coordinates. */
void ui_card_rect(ui_card_t *card, int x, int y, int w, int h, lv_color_t color);
void ui_card_text(ui_card_t *card, int x, int y, int max_w, const char *txt,
                  lv_color_t color, const lv_font_t *font);

#ifdef __cplusplus
}
#endif
