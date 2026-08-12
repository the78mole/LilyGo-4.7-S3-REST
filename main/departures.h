#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEPARTURES_MAX 2

typedef struct {
    char line[8];   /* "S1", "285" */
    char time[8];   /* planned departure, "HH:MM" */
    int delay_min;  /* realtime minus planned; 0 when no realtime data */
    bool valid;     /* false when the last fetch found no matching service */
} departure_t;

/*
 * Public-transport departures, fetched by the device itself from the VGN EFA
 * departure monitor.
 *
 * Unlike the calendar/price data this is NOT pushed in over REST: the panel
 * queries it directly, so the times stay current between dashboard pushes and
 * do not depend on a host being awake. A background task on the network core
 * refreshes the cache; drawing code only ever reads the cached copy, so a
 * slow or failing HTTP request can never stall a render.
 */
esp_err_t departures_start(void);

/*
 * Copies up to `max` cached entries into `out`, returning how many were
 * written. Entries whose service was not found carry valid == false so the
 * caller can render a placeholder rather than a stale time.
 */
int departures_get(departure_t *out, int max);

/* Seconds since the last successful refresh, or -1 if there has not been one
 * yet -- lets the UI mark the data as stale instead of quietly showing an old
 * timetable. */
int departures_age_s(void);

#ifdef __cplusplus
}
#endif
