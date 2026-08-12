#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the task that keeps time-sensitive parts of the screen current
 * without a client push: the departure strip and Wi-Fi signal every minute,
 * today's price chart on each quarter-hour boundary. Call after LVGL and
 * SNTP are up.
 */
esp_err_t ui_tick_start(void);

#ifdef __cplusplus
}
#endif
