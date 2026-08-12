#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIDGET_LIST_MAX_ITEMS    8
#define WIDGET_FORECAST_MAX      4
#define WIDGET_SERIES_MAX        24  /* 24h of hourly sparkline samples */
#define WIDGET_TEXT_MAX          48

typedef struct {
    char text[WIDGET_TEXT_MAX];
    bool done;
} widget_list_item_t;

typedef struct {
    const char *title;
    int x, y, w, h;
    const widget_list_item_t *items;
    int count;
} widget_list_spec_t;

#define WIDGET_AGENDA_MAX_EVENTS 6
#define WIDGET_AGENDA_MAX_TODOS  6
#define WIDGET_AGENDA_MAX_WASTE  4

typedef struct {
    char when[16];               /* "Do 16:30", or "Do" for all-day entries */
    char text[WIDGET_TEXT_MAX];
    char src[8];                 /* origin abbreviation, e.g. "Da", "EG" */
} widget_event_t;

typedef struct {
    char text[WIDGET_TEXT_MAX];
    char src[8];
} widget_todo_t;

typedef struct {
    char letter[4];              /* R = Restmuell, G = Gelber Sack, B = Bio, P = Papier */
    char label[8];               /* e.g. "Fr 14." */
} widget_waste_t;

/* Two-column cell: appointments on the left, open tasks on the right, with
 * the waste-collection badges tucked into the bottom-right corner. */
typedef struct {
    const char *title;
    int x, y, w, h;
    const widget_event_t *events;
    int event_count;
    const widget_todo_t *todos;
    int todo_count;
    const widget_waste_t *waste;
    int waste_count;
} widget_agenda_spec_t;

typedef struct {
    char label[8];   /* e.g. "21h" */
    float temp;
} widget_forecast_t;

typedef struct {
    const char *title;
    int x, y, w, h;
    const char *condition;  /* Home Assistant condition slug, e.g. "partlycloudy" */
    float temp;
    float temp_min, temp_max;
    float precip;           /* mm */
    float precip_prob;      /* % */
    float wind;             /* km/h */
    bool has_range;
    bool has_precip_prob;
    /* Next-24h hourly series for the two sparklines. Either may be empty. */
    const float *temp_series;
    int temp_series_count;
    const float *pop_series;   /* precipitation probability, 0..100 */
    int pop_series_count;
    const widget_forecast_t *forecast;
    int forecast_count;
} widget_weather_spec_t;

/* Both render one dashboard cell onto lv_scr_act().
 * The caller must hold the LVGL lock. */
esp_err_t widget_list_draw(const widget_list_spec_t *spec);
esp_err_t widget_agenda_draw(const widget_agenda_spec_t *spec);

/*
 * Redraws only the departures + Wi-Fi part of the agenda cell, in place, and
 * invalidates just that rectangle. Called once a minute: the rest of the cell
 * (appointments, tasks, waste) changes far more slowly, and repainting it
 * would turn a small partial panel update into a large one.
 * No-op until widget_agenda_draw() has run once.
 */
esp_err_t widget_agenda_refresh_transit(void);
esp_err_t widget_weather_draw(const widget_weather_spec_t *spec);

#ifdef __cplusplus
}
#endif
