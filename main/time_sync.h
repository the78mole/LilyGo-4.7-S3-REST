#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SNTP time synchronisation. Needed so the device can decide *itself* which
 * slot of a price curve is "now" -- the REST client only supplies the curve,
 * not the highlight index, so a client with a wrong clock cannot mislabel the
 * display.
 *
 * Call after Wi-Fi is up. Non-blocking: it kicks off SNTP and returns; use
 * time_sync_is_valid() to find out whether the clock can be trusted yet.
 */
esp_err_t time_sync_start(void);

/* True once SNTP has delivered a plausible wall-clock time. */
bool time_sync_is_valid(void);

/*
 * Index of the current local-time slot within the day, for a curve sampled
 * every `interval_min` minutes (e.g. 15 -> 96 slots/day, returns 0..95).
 * Returns -1 if the clock is not synchronised, so callers can render the
 * chart without a highlight rather than highlighting an arbitrary bar.
 */
int time_sync_current_slot(int interval_min);

/* "HH:MM" in local time, or "--:--" when unsynchronised. Buffer >= 6 bytes. */
void time_sync_format_hhmm(char *out, size_t out_len);

/* "DD.MM." in local time, or "--.--." when unsynchronised. Buffer >= 7 bytes. */
void time_sync_format_ddmm(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
