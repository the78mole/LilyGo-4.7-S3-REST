#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHART_MAX_SLOTS 96 /* 24h at 15-minute resolution */

typedef struct {
    const char *title;
    int x, y, w, h;
    const float *values;
    int count;
    int interval_min;   /* minutes per slot; 15 -> 96 slots/day */
    float y_min, y_max; /* fixed axis range -- see chart_spec_defaults() */
    bool highlight_now; /* mark the slot matching the device's local clock */
    const char *unit;
} chart_spec_t;

/*
 * Fills `spec` with the firmware-side defaults (fixed 0..CONFIG_APP_CHART_Y_MAX
 * ct/kWh scale, CONFIG_APP_CHART_INTERVAL_MIN resolution, the bottom-left cell
 * of the 2x2 dashboard grid). A REST client then only has to supply the data
 * points -- everything else has a sensible on-device default it can override.
 */
void chart_spec_defaults(chart_spec_t *spec);

/*
 * Renders a bar chart onto lv_scr_act() at the given position.
 *
 * Drawn on an lv_canvas rather than lv_chart: lv_chart cannot style an
 * individual bar differently, and the whole point here is to highlight the
 * slot that is current *right now*. The canvas also keeps this to a single
 * LVGL object instead of ~96, which matters on a panel this size.
 *
 * The y axis is fixed (not auto-scaled) so charts stay comparable between
 * days; values above y_max are clamped and flagged with a marker rather than
 * silently drawn as if they were exactly y_max.
 *
 * The caller must hold the LVGL lock.
 */
esp_err_t chart_draw(const chart_spec_t *spec);

#ifdef __cplusplus
}
#endif
