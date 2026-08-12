#pragma once

#include "sdkconfig.h"

/*
 * Shared, non-Kconfig constants. Anything the user is likely to want to
 * tweak per-board lives in Kconfig (see Kconfig.projbuild) instead.
 */

#define APP_SD_MOUNT_POINT      CONFIG_APP_SD_MOUNT_POINT
#define APP_SD_MAX_FILES_OPEN   4

/* Custom monochrome/grayscale raw image format written by tools/epd_client.py
 * and consumed by api_display_image_handler():
 *
 *   offset 0: uint16_t width_px   (little-endian)
 *   offset 2: uint16_t height_px  (little-endian)
 *   offset 4: uint8_t  pixels[width_px * height_px]   (8-bit grayscale,
 *                                                       row-major, 1 byte/px)
 *
 * 8 bits/pixel is used (rather than 1bpp) because it maps 1:1 onto LVGL's
 * LV_COLOR_DEPTH=8 pixel format, so no per-pixel conversion is needed when
 * handing the buffer to lv_img_dsc_t.
 */
#define APP_IMAGE_HEADER_SIZE 4

#define APP_NETWORK_CORE  CONFIG_APP_TASK_CORE_NETWORK
#define APP_DISPLAY_CORE  CONFIG_APP_TASK_CORE_DISPLAY
