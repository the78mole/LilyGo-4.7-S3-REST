#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEPARTURES_MAX 3

typedef struct {
    char line[8];   /* "S1", "285" */
    char dir[8];    /* direction abbreviation shown next to the line, e.g. "N" */
    char time[8];   /* planned departure, "HH:MM" */
    int delay_min;  /* realtime minus planned; 0 when no realtime data */
    bool cancelled; /* trip is cancelled -- time and delay are meaningless */
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
 * written. Entries carry valid == false when no departure is known, so the
 * caller renders a placeholder rather than a stale time.
 *
 * A failed fetch does not by itself invalidate an entry: a departure that has
 * not left yet (delay included) survives until its time has passed, so a
 * transient network failure no longer blanks the line. Anything already gone
 * is dropped, so a shown time is never in the past.
 */
int departures_get(departure_t *out, int max);

/* Seconds since the last successful refresh, or -1 if there has not been one
 * yet -- lets the UI mark the data as stale instead of quietly showing an old
 * timetable. */
int departures_age_s(void);

#ifdef __cplusplus
}
#endif
