#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mounts the SD card over SPI at APP_SD_MOUNT_POINT using the ESP-IDF VFS
 * FATFS driver. Deliberately does NOT auto-format on mount failure: a
 * corrupt/unrecognized card must be treated as an operator error, not
 * silently wiped.
 */
esp_err_t sd_card_init(void);

/*
 * All VFS access to the card (from both the REST API's upload/read paths
 * and anything else touching /sdcard) must be bracketed by these calls.
 * The mutex is recursive-safe is NOT assumed -- do not nest lock calls
 * from the same task.
 *
 * Returns false if the lock could not be acquired within `timeout_ticks`.
 */
bool sd_card_lock(TickType_t timeout_ticks);
void sd_card_unlock(void);

const char *sd_card_mount_point(void);

#ifdef __cplusplus
}
#endif
