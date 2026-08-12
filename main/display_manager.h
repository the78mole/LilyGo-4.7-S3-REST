#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Owns the LilyGo-EPD47 panel driver and a single full-screen 4bpp
 * framebuffer in PSRAM. LVGL never talks to the driver directly --
 * lvgl_port.c's flush callback calls display_manager_write_pixels() to copy
 * rendered pixels into that framebuffer, and a dedicated core-1 task here
 * performs the actual (slow, ~1s) panel refresh asynchronously so that LVGL
 * flush callbacks stay fast.
 */
esp_err_t display_manager_init(void);

int display_manager_width(void);
int display_manager_height(void);

/*
 * Copies an 8-bit grayscale, row-major pixel buffer (LV_COLOR_DEPTH=8, one
 * byte per pixel) into the panel framebuffer at [x1,y1]-[x2,y2] (inclusive)
 * and marks the panel dirty. `stride_px` is the buffer's row width in
 * pixels (>= x2 - x1 + 1). Safe to call from any task/core; internally
 * synchronized.
 */
void display_manager_write_pixels(int x1, int y1, int x2, int y2,
                                   const uint8_t *gray8, int stride_px);

/* Blocks until any refresh triggered by prior writes has been pushed to
 * the panel. Mostly useful for the demo/test path. */
void display_manager_wait_idle(void);

#ifdef __cplusplus
}
#endif
