#include "departures.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "departures";

/*
 * Ported from the reference implementation in
 * the78mole/Playground/vgn-abfahrten/vgn_abfahrten.py.
 *
 * Direction is matched on servingLine.liErgRiProj.direction ("H"/"R") rather
 * than the displayed destination name: the destination varies with short
 * workings (the S1 towards Nuremberg shows up as "Lauf (li Pegn)",
 * "Hartmannshof", ...), while the direction code does not.
 */

#define EFA_URL_FMT \
    "https://efa.vgn.de/vgnExt_oeffi/XML_DM_REQUEST" \
    "?language=de&mode=direct&type_dm=stop&name_dm=%s" \
    "&outputFormat=JSON&useRealtime=1&limit=%d"

#define EFA_LIMIT 20
/* The full monitor response is ~24 kB; cap generously but finitely so a
 * misbehaving endpoint cannot exhaust PSRAM. */
#define RESPONSE_MAX_BYTES (64 * 1024)

typedef struct {
    const char *stop_id;
    const char *line;
    const char *direction;  /* EFA code H/R */
    const char *label;      /* short direction hint shown on the panel */
} query_t;

static const query_t s_queries[DEPARTURES_MAX] = {
    { CONFIG_APP_DEP1_STOP, CONFIG_APP_DEP1_LINE, CONFIG_APP_DEP1_DIR, CONFIG_APP_DEP1_LABEL },
    { CONFIG_APP_DEP2_STOP, CONFIG_APP_DEP2_LINE, CONFIG_APP_DEP2_DIR, CONFIG_APP_DEP2_LABEL },
    { CONFIG_APP_DEP3_STOP, CONFIG_APP_DEP3_LINE, CONFIG_APP_DEP3_DIR, CONFIG_APP_DEP3_LABEL },
};

static departure_t s_cache[DEPARTURES_MAX];
static SemaphoreHandle_t s_mutex;
static time_t s_last_ok;

/* ------------------------------------------------------------- fetching --- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} resp_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !ctx || ctx->overflow) {
        return ESP_OK;
    }
    if (ctx->len + evt->data_len + 1 > ctx->cap) {
        size_t want = (ctx->cap ? ctx->cap * 2 : 8192);
        while (want < ctx->len + evt->data_len + 1) {
            want *= 2;
        }
        if (want > RESPONSE_MAX_BYTES) {
            ESP_LOGW(TAG, "response exceeds %d bytes, discarding", RESPONSE_MAX_BYTES);
            ctx->overflow = true;
            return ESP_OK;
        }
        char *grown = heap_caps_realloc(ctx->buf, want, MALLOC_CAP_SPIRAM);
        if (!grown) {
            ctx->overflow = true;
            return ESP_OK;
        }
        ctx->buf = grown;
        ctx->cap = want;
    }
    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

static char *fetch_json(const char *stop_id)
{
    char url[256];
    snprintf(url, sizeof(url), EFA_URL_FMT, stop_id, EFA_LIMIT);

    resp_ctx_t ctx = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .event_handler = http_event,
        .user_data = &ctx,
        /* Verify against ESP-IDF's bundled roots rather than pinning a cert:
         * efa.vgn.de is a third-party endpoint whose certificate will rotate. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx.overflow || !ctx.buf) {
        ESP_LOGW(TAG, "stop %s: %s, HTTP %d%s", stop_id, esp_err_to_name(err),
                 status, ctx.overflow ? " (oversized)" : "");
        free(ctx.buf);
        return NULL;
    }
    return ctx.buf;
}

/* -------------------------------------------------------------- parsing --- */

static int json_int(const cJSON *obj, const char *key)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(j)) {
        return j->valueint;
    }
    /* EFA returns these as strings ("21", "54"), not numbers. */
    if (cJSON_IsString(j) && j->valuestring) {
        return atoi(j->valuestring);
    }
    return -1;
}

/* Minutes between two EFA date/time objects, ignoring the date rollover case
 * beyond a day (a delay never spans that). */
static int minutes_between(const cJSON *from, const cJSON *to)
{
    int fd = json_int(from, "day"), fh = json_int(from, "hour"), fm = json_int(from, "minute");
    int td = json_int(to, "day"), th = json_int(to, "hour"), tm = json_int(to, "minute");
    if (fh < 0 || fm < 0 || th < 0 || tm < 0) {
        return 0;
    }
    int delta = (th * 60 + tm) - (fh * 60 + fm);
    if (td != fd) {
        delta += (td > fd ? 1 : -1) * 24 * 60;
    }
    return delta;
}

static bool parse_first_match(const char *json, const query_t *q, departure_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "line %s: JSON parse failed", q->line);
        return false;
    }

    bool found = false;
    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "departureList");
    cJSON *dep = NULL;
    cJSON_ArrayForEach(dep, list) {
        cJSON *sl = cJSON_GetObjectItemCaseSensitive(dep, "servingLine");
        cJSON *num = cJSON_GetObjectItemCaseSensitive(sl, "number");
        if (!cJSON_IsString(num) || strcmp(num->valuestring, q->line) != 0) {
            continue;
        }
        cJSON *proj = cJSON_GetObjectItemCaseSensitive(sl, "liErgRiProj");
        cJSON *dir = cJSON_GetObjectItemCaseSensitive(proj, "direction");
        if (!cJSON_IsString(dir) || strcmp(dir->valuestring, q->direction) != 0) {
            continue;
        }

        cJSON *planned = cJSON_GetObjectItemCaseSensitive(dep, "dateTime");
        int hh = json_int(planned, "hour"), mm = json_int(planned, "minute");
        if (hh < 0 || mm < 0) {
            continue;
        }

        strlcpy(out->line, q->line, sizeof(out->line));
        strlcpy(out->dir, q->label, sizeof(out->dir));
        snprintf(out->time, sizeof(out->time), "%02d:%02d", hh, mm);

        /* EFA flags a cancelled trip with servingLine.delay == "-9999" (a
         * sentinel, not a real delay -- treating it as a number would render
         * the departure as nearly seven days early). */
        cJSON *jdelay = cJSON_GetObjectItemCaseSensitive(sl, "delay");
        const char *delay_s = cJSON_IsString(jdelay) ? jdelay->valuestring : NULL;
        out->cancelled = (delay_s && strcmp(delay_s, "-9999") == 0) ||
                         (cJSON_IsNumber(jdelay) && jdelay->valueint == -9999);

        cJSON *real = cJSON_GetObjectItemCaseSensitive(dep, "realDateTime");
        out->delay_min = (!out->cancelled && real) ? minutes_between(planned, real) : 0;
        out->valid = true;
        found = true;
        break;
    }

    cJSON_Delete(root);
    if (!found) {
        ESP_LOGW(TAG, "line %s dir %s: no departure in the next %d entries",
                 q->line, q->direction, EFA_LIMIT);
    }
    return found;
}

/* -------------------------------------------------------------- refresh --- */

static void refresh_once(void)
{
    departure_t fresh[DEPARTURES_MAX] = {0};
    char *cached[DEPARTURES_MAX] = {0};
    bool any = false;

    for (int i = 0; i < DEPARTURES_MAX; i++) {
        strlcpy(fresh[i].line, s_queries[i].line, sizeof(fresh[i].line));
        strlcpy(fresh[i].dir, s_queries[i].label, sizeof(fresh[i].dir));
        if (s_queries[i].line[0] == '\0') {
            continue;  /* slot left unconfigured */
        }
        /* Several queries can share a stop (the S1 in both directions), and
         * the monitor response is ~24 kB -- fetch each stop only once. */
        char *json = NULL;
        bool borrowed = false;
        for (int j = 0; j < i; j++) {
            if (cached[j] && strcmp(s_queries[j].stop_id, s_queries[i].stop_id) == 0) {
                json = cached[j];
                borrowed = true;
                break;
            }
        }
        if (!json) {
            json = fetch_json(s_queries[i].stop_id);
            cached[i] = json;
        }
        if (!json) {
            continue;
        }
        if (parse_first_match(json, &s_queries[i], &fresh[i])) {
            any = true;
        }
        (void)borrowed;
    }

    for (int i = 0; i < DEPARTURES_MAX; i++) {
        free(cached[i]);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_cache, fresh, sizeof(s_cache));
    if (any) {
        time(&s_last_ok);
    }
    xSemaphoreGive(s_mutex);

    for (int i = 0; i < DEPARTURES_MAX; i++) {
        if (fresh[i].valid) {
            if (fresh[i].cancelled) {
                ESP_LOGI(TAG, "%s %s AUSFALL", fresh[i].line, fresh[i].time);
            } else {
                ESP_LOGI(TAG, "%s%s%s %s +%d", fresh[i].line,
                         fresh[i].dir[0] ? " " : "", fresh[i].dir,
                         fresh[i].time, fresh[i].delay_min);
            }
        }
    }
}

static void departures_task(void *arg)
{
    (void)arg;
    for (;;) {
        refresh_once();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_DEP_REFRESH_S * 1000));
    }
}

esp_err_t departures_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    /* Network core: this does TLS and blocking HTTP, and must stay off the
     * core driving the panel. Stack is generous because mbedTLS is hungry. */
    BaseType_t ok = xTaskCreatePinnedToCore(departures_task, "departures", 8192,
                                             NULL, tskIDLE_PRIORITY + 2, NULL,
                                             APP_NETWORK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

int departures_get(departure_t *out, int max)
{
    if (!out || max <= 0 || !s_mutex) {
        return 0;
    }
    int n = max < DEPARTURES_MAX ? max : DEPARTURES_MAX;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, s_cache, (size_t)n * sizeof(departure_t));
    xSemaphoreGive(s_mutex);
    return n;
}

int departures_age_s(void)
{
    if (!s_mutex) {
        return -1;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    time_t last = s_last_ok;
    xSemaphoreGive(s_mutex);
    if (last == 0) {
        return -1;
    }
    time_t now = 0;
    time(&now);
    return (int)(now - last);
}
