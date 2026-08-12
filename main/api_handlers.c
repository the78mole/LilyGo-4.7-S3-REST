#include "api_handlers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "chart.h"
#include "ui_card.h"
#include "widgets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "sd_card.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

static const char *TAG = "api_handlers";

#define UPLOAD_CHUNK_SIZE   4096
/* 96 quarter-hour values plus framing; generous but still bounded. */
#define JSON_BODY_MAX_BYTES 8192
#define SD_LOCK_TIMEOUT_MS  5000
#define LVGL_LOCK_TIMEOUT_MS 5000

static esp_err_t send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    httpd_resp_send_err(req, code, msg);
    return ESP_FAIL;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* Rejects anything but a flat "name.ext" component -- no path separators,
 * no "..", nothing that could escape APP_SD_MOUNT_POINT. */
static bool is_safe_filename(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    if (strstr(name, "..") != NULL) {
        return false;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return false;
    }
    return true;
}

static void build_sd_path(char *out, size_t out_len, const char *filename)
{
    snprintf(out, out_len, "%s/%s", sd_card_mount_point(), filename);
}

/* Reads the full (size-capped) request body into a heap buffer for JSON
 * parsing. Display-command payloads are small by construction, unlike
 * /api/upload which streams instead of buffering. */
static esp_err_t read_json_body(httpd_req_t *req, char **out_buf)
{
    if (req->content_len <= 0 || req->content_len > JSON_BODY_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';
    *out_buf = buf;
    return ESP_OK;
}

esp_err_t api_health_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, wifi_manager_is_connected()
                                 ? "{\"status\":\"ok\",\"wifi_connected\":true}"
                                 : "{\"status\":\"ok\",\"wifi_connected\":false}");
    return ESP_OK;
}

esp_err_t api_upload_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > CONFIG_APP_HTTP_MAX_UPLOAD_BYTES) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing/oversized Content-Length");
    }

    size_t qs_len = httpd_req_get_url_query_len(req) + 1;
    if (qs_len <= 1) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing ?filename=");
    }

    char *query = malloc(qs_len);
    if (!query || httpd_req_get_url_query_str(req, query, qs_len) != ESP_OK) {
        free(query);
        return send_err(req, HTTPD_400_BAD_REQUEST, "bad query string");
    }

    char filename[128];
    esp_err_t qerr = httpd_query_key_value(query, "filename", filename, sizeof(filename));
    free(query);
    if (qerr != ESP_OK || !is_safe_filename(filename)) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing/unsafe filename");
    }

    char path[192];
    build_sd_path(path, sizeof(path), filename);

    if (!sd_card_lock(pdMS_TO_TICKS(SD_LOCK_TIMEOUT_MS))) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd card busy");
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        int err_no = errno;
        sd_card_unlock();
        ESP_LOGE(TAG, "fopen('%s', wb) failed: errno=%d (%s)", path, err_no,
                 strerror(err_no));
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not open file for writing");
    }

    char *chunk = malloc(UPLOAD_CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        sd_card_unlock();
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    int remaining = req->content_len;
    bool io_error = false;
    while (remaining > 0) {
        int to_read = remaining < UPLOAD_CHUNK_SIZE ? remaining : UPLOAD_CHUNK_SIZE;
        int received = httpd_req_recv(req, chunk, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            io_error = true;
            break;
        }
        if (fwrite(chunk, 1, received, f) != (size_t)received) {
            io_error = true;
            break;
        }
        remaining -= received;
    }

    free(chunk);
    fclose(f);
    sd_card_unlock();

    if (io_error) {
        ESP_LOGE(TAG, "upload of '%s' failed mid-stream", filename);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
    }

    ESP_LOGI(TAG, "Wrote %s (%d bytes) to SD", path, req->content_len);
    return send_ok(req);
}

/* Maps an 8-bit grayscale sample onto LVGL's LV_COLOR_DEPTH=8 (RGB332)
 * pixel encoding so that (a) LV_IMG_CF_TRUE_COLOR draws it correctly and
 * (b) our own flush callback's lv_color8_to_gray8() round-trips it back
 * to (approximately) the same gray level. The on-disk format stays plain
 * 8-bit grayscale so client-side tooling doesn't need to know about
 * LVGL's pixel packing. */
static inline uint8_t gray8_to_lv_color8(uint8_t gray)
{
    uint8_t r3 = (uint8_t)((gray * 7 + 127) / 255);
    uint8_t g3 = r3;
    uint8_t b2 = (uint8_t)((gray * 3 + 127) / 255);
    return (uint8_t)((r3 << 5) | (g3 << 2) | b2);
}

esp_err_t api_display_image_handler(httpd_req_t *req)
{
    char *body;
    esp_err_t err = read_json_body(req, &body);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing/oversized JSON body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }

    cJSON *jfilename = cJSON_GetObjectItemCaseSensitive(root, "filename");
    cJSON *jx = cJSON_GetObjectItemCaseSensitive(root, "x");
    cJSON *jy = cJSON_GetObjectItemCaseSensitive(root, "y");

    if (!cJSON_IsString(jfilename) || !cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) ||
        !is_safe_filename(jfilename->valuestring)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400_BAD_REQUEST,
                         "expected {\"filename\":str,\"x\":int,\"y\":int}");
    }

    char filename[128];
    strlcpy(filename, jfilename->valuestring, sizeof(filename));
    int x = jx->valueint;
    int y = jy->valueint;
    cJSON_Delete(root);

    char path[192];
    build_sd_path(path, sizeof(path), filename);

    if (!sd_card_lock(pdMS_TO_TICKS(SD_LOCK_TIMEOUT_MS))) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd card busy");
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        sd_card_unlock();
        return send_err(req, HTTPD_404_NOT_FOUND, "image not found on SD card");
    }

    uint8_t header[APP_IMAGE_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        sd_card_unlock();
        return send_err(req, HTTPD_400_BAD_REQUEST, "truncated image header");
    }

    uint16_t width = (uint16_t)(header[0] | (header[1] << 8));
    uint16_t height = (uint16_t)(header[2] | (header[3] << 8));
    size_t pixel_count = (size_t)width * height;

    if (width == 0 || height == 0 || pixel_count > 4u * 1024 * 1024) {
        fclose(f);
        sd_card_unlock();
        return send_err(req, HTTPD_400_BAD_REQUEST, "invalid image dimensions");
    }

    uint8_t *pixels = heap_caps_malloc(pixel_count, MALLOC_CAP_SPIRAM);
    if (!pixels) {
        fclose(f);
        sd_card_unlock();
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    size_t read_bytes = fread(pixels, 1, pixel_count, f);
    fclose(f);
    sd_card_unlock();

    if (read_bytes != pixel_count) {
        heap_caps_free(pixels);
        return send_err(req, HTTPD_400_BAD_REQUEST, "truncated pixel data");
    }

    for (size_t i = 0; i < pixel_count; i++) {
        pixels[i] = gray8_to_lv_color8(pixels[i]);
    }

    lv_img_dsc_t *dsc = heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM);
    if (!dsc) {
        heap_caps_free(pixels);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    dsc->header.always_zero = 0;
    dsc->header.w = width;
    dsc->header.h = height;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->data_size = pixel_count;
    dsc->data = pixels;

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        heap_caps_free(pixels);
        heap_caps_free(dsc);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display busy");
    }

    /* NOTE: each call creates a new image object; `dsc`/`pixels` are kept
     * alive for the object's lifetime and intentionally not freed here.
     * This template does not track/evict previously placed images -- add
     * that (or a DELETE endpoint) before using this on a long-running
     * device that receives many distinct images. */
    lv_obj_t *img = lv_img_create(lv_scr_act());
    lv_img_set_src(img, dsc);
    lv_obj_set_pos(img, x, y);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Placed image '%s' (%ux%u) at (%d,%d)", filename, width, height, x, y);
    return send_ok(req);
}

esp_err_t api_display_chart_handler(httpd_req_t *req)
{
    char *body;
    if (read_json_body(req, &body) != ESP_OK) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing/oversized JSON body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }

    cJSON *jvalues = cJSON_GetObjectItemCaseSensitive(root, "values");
    if (!cJSON_IsArray(jvalues)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400_BAD_REQUEST, "expected \"values\": [ ... ]");
    }

    /* An empty array is legitimate: it renders the "no data yet" card. */
    int count = cJSON_GetArraySize(jvalues);
    if (count < 0 || count > CHART_MAX_SLOTS) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400_BAD_REQUEST, "values must hold 0..96 numbers");
    }

    float *values = count > 0 ? malloc((size_t)count * sizeof(float)) : NULL;
    if (count > 0 && !values) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(jvalues, i);
        if (!cJSON_IsNumber(item)) {
            free(values);
            cJSON_Delete(root);
            return send_err(req, HTTPD_400_BAD_REQUEST, "values must all be numbers");
        }
        values[i] = (float)item->valuedouble;
    }

    /* Everything except the data has an on-device default; a minimal request
     * is just {"values": [...]}. */
    chart_spec_t spec;
    chart_spec_defaults(&spec);
    spec.values = values;
    spec.count = count;

    /* "slot" picks one of the two dashboard cells and, for tomorrow, disables
     * the now-marker (highlighting "now" on tomorrow's curve is meaningless). */
    char title[64] = "Strompreis heute";
    cJSON *jslot = cJSON_GetObjectItemCaseSensitive(root, "slot");
    bool tomorrow = cJSON_IsString(jslot) && strcmp(jslot->valuestring, "tomorrow") == 0;
    /* Highlighting "now" on tomorrow's curve would be meaningless. */
    spec.highlight_now = !tomorrow;
    strlcpy(title, tomorrow ? "Strompreis morgen" : "Strompreis heute", sizeof(title));
    ui_card_slot_rect(tomorrow ? "bottom-right" : "bottom-left",
                      &spec.x, &spec.y, &spec.w, &spec.h);

    cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (cJSON_IsString(jtitle)) {
        strlcpy(title, jtitle->valuestring, sizeof(title));
    }
    spec.title = title;

    char empty_text[48] = "Noch keine Daten";
    char empty_hint[64] = "";
    cJSON *jet = cJSON_GetObjectItemCaseSensitive(root, "empty_text");
    if (cJSON_IsString(jet)) {
        strlcpy(empty_text, jet->valuestring, sizeof(empty_text));
    }
    cJSON *jeh = cJSON_GetObjectItemCaseSensitive(root, "empty_hint");
    if (cJSON_IsString(jeh)) {
        strlcpy(empty_hint, jeh->valuestring, sizeof(empty_hint));
    }
    spec.empty_text = empty_text;
    spec.empty_hint = empty_hint[0] ? empty_hint : NULL;

    /* Optional overrides. */
    cJSON *j;
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "x"))) spec.x = j->valueint;
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "y"))) spec.y = j->valueint;
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "w"))) spec.w = j->valueint;
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "h"))) spec.h = j->valueint;
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "y_max"))) spec.y_max = (float)j->valuedouble;
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "y_min_neg"))) spec.y_min_neg = (float)j->valuedouble;
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "interval_min"))) spec.interval_min = j->valueint;
    if (cJSON_IsBool(j = cJSON_GetObjectItemCaseSensitive(root, "highlight_now"))) spec.highlight_now = cJSON_IsTrue(j);

    cJSON_Delete(root);

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        free(values);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display busy");
    }
    esp_err_t derr = chart_draw(&spec);
    lvgl_port_unlock();

    /* chart_draw copies what it needs into the canvas bitmap, so the source
     * samples can go away immediately -- unlike the image handler, this leaks
     * only the canvas buffer, not the input data. */
    free(values);

    if (derr != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "chart draw failed");
    }
    return send_ok(req);
}

esp_err_t api_display_list_handler(httpd_req_t *req)
{
    char *body;
    if (read_json_body(req, &body) != ESP_OK) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing/oversized JSON body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }

    cJSON *jitems = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(jitems) || cJSON_GetArraySize(jitems) <= 0) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400_BAD_REQUEST, "expected \"items\": [ ... ]");
    }

    int count = cJSON_GetArraySize(jitems);
    if (count > WIDGET_LIST_MAX_ITEMS) {
        count = WIDGET_LIST_MAX_ITEMS;
    }

    widget_list_item_t *items = calloc(count, sizeof(*items));
    if (!items) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    for (int i = 0; i < count; i++) {
        cJSON *it = cJSON_GetArrayItem(jitems, i);
        cJSON *jt = cJSON_GetObjectItemCaseSensitive(it, "text");
        cJSON *jd = cJSON_GetObjectItemCaseSensitive(it, "done");
        strlcpy(items[i].text, cJSON_IsString(jt) ? jt->valuestring : "?",
                sizeof(items[i].text));
        items[i].done = cJSON_IsTrue(jd);
    }

    widget_list_spec_t spec = {0};
    spec.items = items;
    spec.count = count;

    cJSON *jslot = cJSON_GetObjectItemCaseSensitive(root, "slot");
    ui_card_slot_rect(cJSON_IsString(jslot) ? jslot->valuestring : "top-left",
                      &spec.x, &spec.y, &spec.w, &spec.h);

    char title[64] = "Aufgaben";
    cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (cJSON_IsString(jtitle)) {
        strlcpy(title, jtitle->valuestring, sizeof(title));
    }
    spec.title = title;

    cJSON_Delete(root);

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        free(items);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display busy");
    }
    esp_err_t derr = widget_list_draw(&spec);
    lvgl_port_unlock();
    free(items);

    if (derr != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "list draw failed");
    }
    return send_ok(req);
}

/* Copies a JSON string field into a fixed buffer, tolerating absence. */
static void json_str(cJSON *obj, const char *key, char *dst, size_t dst_sz)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    strlcpy(dst, cJSON_IsString(j) ? j->valuestring : "", dst_sz);
}

esp_err_t api_display_agenda_handler(httpd_req_t *req)
{
    char *body;
    if (read_json_body(req, &body) != ESP_OK) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing/oversized JSON body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }

    widget_agenda_spec_t spec = {0};
    static widget_event_t events[WIDGET_AGENDA_MAX_EVENTS];
    static widget_todo_t todos[WIDGET_AGENDA_MAX_TODOS];
    static widget_waste_t waste[WIDGET_AGENDA_MAX_WASTE];
    memset(events, 0, sizeof(events));
    memset(todos, 0, sizeof(todos));
    memset(waste, 0, sizeof(waste));

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "events");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > WIDGET_AGENDA_MAX_EVENTS) n = WIDGET_AGENDA_MAX_EVENTS;
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(arr, i);
            json_str(e, "when", events[i].when, sizeof(events[i].when));
            json_str(e, "text", events[i].text, sizeof(events[i].text));
            json_str(e, "src",  events[i].src,  sizeof(events[i].src));
        }
        spec.events = events;
        spec.event_count = n;
    }

    arr = cJSON_GetObjectItemCaseSensitive(root, "todos");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > WIDGET_AGENDA_MAX_TODOS) n = WIDGET_AGENDA_MAX_TODOS;
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(arr, i);
            json_str(e, "text", todos[i].text, sizeof(todos[i].text));
            json_str(e, "src",  todos[i].src,  sizeof(todos[i].src));
        }
        spec.todos = todos;
        spec.todo_count = n;
    }

    arr = cJSON_GetObjectItemCaseSensitive(root, "waste");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > WIDGET_AGENDA_MAX_WASTE) n = WIDGET_AGENDA_MAX_WASTE;
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(arr, i);
            json_str(e, "letter", waste[i].letter, sizeof(waste[i].letter));
            json_str(e, "label",  waste[i].label,  sizeof(waste[i].label));
        }
        spec.waste = waste;
        spec.waste_count = n;
    }

    char title[64] = "Termine & Aufgaben";
    cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (cJSON_IsString(jtitle)) {
        strlcpy(title, jtitle->valuestring, sizeof(title));
    }
    spec.title = title;

    cJSON *jslot = cJSON_GetObjectItemCaseSensitive(root, "slot");
    ui_card_slot_rect(cJSON_IsString(jslot) ? jslot->valuestring : "top-left",
                      &spec.x, &spec.y, &spec.w, &spec.h);
    cJSON_Delete(root);

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display busy");
    }
    esp_err_t derr = widget_agenda_draw(&spec);
    lvgl_port_unlock();

    if (derr != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "agenda draw failed");
    }
    return send_ok(req);
}

esp_err_t api_display_weather_handler(httpd_req_t *req)
{
    char *body;
    if (read_json_body(req, &body) != ESP_OK) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing/oversized JSON body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }

    widget_weather_spec_t spec = {0};
    char condition[32] = "cloudy";
    char title[64] = "Wetter";

    cJSON *j;
    if (cJSON_IsString(j = cJSON_GetObjectItemCaseSensitive(root, "condition"))) {
        strlcpy(condition, j->valuestring, sizeof(condition));
    }
    if (cJSON_IsString(j = cJSON_GetObjectItemCaseSensitive(root, "title"))) {
        strlcpy(title, j->valuestring, sizeof(title));
    }
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "temp"))) {
        spec.temp = (float)j->valuedouble;
    }
    cJSON *jmin = cJSON_GetObjectItemCaseSensitive(root, "temp_min");
    cJSON *jmax = cJSON_GetObjectItemCaseSensitive(root, "temp_max");
    if (cJSON_IsNumber(jmin) && cJSON_IsNumber(jmax)) {
        spec.temp_min = (float)jmin->valuedouble;
        spec.temp_max = (float)jmax->valuedouble;
        spec.has_range = true;
    }
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "precip"))) {
        spec.precip = (float)j->valuedouble;
    }
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "wind"))) {
        spec.wind = (float)j->valuedouble;
    }
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "precip_prob"))) {
        spec.precip_prob = (float)j->valuedouble;
        spec.has_precip_prob = true;
    }

    /* Optional 24h hourly series for the two sparklines. */
    float temp_series[WIDGET_SERIES_MAX];
    float pop_series[WIDGET_SERIES_MAX];
    struct { const char *key; float *dst; const float **out; int *out_n; } series[] = {
        { "temp_series", temp_series, &spec.temp_series, &spec.temp_series_count },
        { "pop_series",  pop_series,  &spec.pop_series,  &spec.pop_series_count  },
    };
    for (size_t k = 0; k < sizeof(series) / sizeof(series[0]); k++) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, series[k].key);
        if (!cJSON_IsArray(arr)) {
            continue;
        }
        int n = cJSON_GetArraySize(arr);
        if (n > WIDGET_SERIES_MAX) n = WIDGET_SERIES_MAX;
        int used = 0;
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(arr, i);
            if (cJSON_IsNumber(e)) {
                series[k].dst[used++] = (float)e->valuedouble;
            }
        }
        if (used > 0) {
            *series[k].out = series[k].dst;
            *series[k].out_n = used;
        }
    }

    widget_forecast_t fc[WIDGET_FORECAST_MAX] = {0};
    cJSON *jfc = cJSON_GetObjectItemCaseSensitive(root, "forecast");
    if (cJSON_IsArray(jfc)) {
        int n = cJSON_GetArraySize(jfc);
        if (n > WIDGET_FORECAST_MAX) n = WIDGET_FORECAST_MAX;
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(jfc, i);
            cJSON *jl = cJSON_GetObjectItemCaseSensitive(e, "label");
            cJSON *jt = cJSON_GetObjectItemCaseSensitive(e, "temp");
            strlcpy(fc[i].label, cJSON_IsString(jl) ? jl->valuestring : "?",
                    sizeof(fc[i].label));
            fc[i].temp = cJSON_IsNumber(jt) ? (float)jt->valuedouble : 0.0f;
        }
        spec.forecast = fc;
        spec.forecast_count = n;
    }

    cJSON *jslot = cJSON_GetObjectItemCaseSensitive(root, "slot");
    ui_card_slot_rect(cJSON_IsString(jslot) ? jslot->valuestring : "top-right",
                      &spec.x, &spec.y, &spec.w, &spec.h);
    cJSON_Delete(root);

    spec.condition = condition;
    spec.title = title;

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display busy");
    }
    esp_err_t derr = widget_weather_draw(&spec);
    lvgl_port_unlock();

    if (derr != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "weather draw failed");
    }
    return send_ok(req);
}

static const lv_font_t *font_for_size(int size)
{
    if (size <= 14) return &lv_font_montserrat_14;
    if (size <= 18) return &lv_font_montserrat_18;
    if (size <= 24) return &lv_font_montserrat_24;
    if (size <= 28) return &lv_font_montserrat_28;
    if (size <= 32) return &lv_font_montserrat_32;
    return &lv_font_montserrat_40;
}

esp_err_t api_display_text_handler(httpd_req_t *req)
{
    char *body;
    esp_err_t err = read_json_body(req, &body);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing/oversized JSON body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }

    cJSON *jtext = cJSON_GetObjectItemCaseSensitive(root, "text");
    cJSON *jx = cJSON_GetObjectItemCaseSensitive(root, "x");
    cJSON *jy = cJSON_GetObjectItemCaseSensitive(root, "y");
    cJSON *jsize = cJSON_GetObjectItemCaseSensitive(root, "size");

    if (!cJSON_IsString(jtext) || !cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400_BAD_REQUEST,
                         "expected {\"text\":str,\"x\":int,\"y\":int,\"size\":int}");
    }

    char text[256];
    strlcpy(text, jtext->valuestring, sizeof(text));
    int x = jx->valueint;
    int y = jy->valueint;
    int size = cJSON_IsNumber(jsize) ? jsize->valueint : 18;
    cJSON_Delete(root);

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display busy");
    }

    /* Each call adds a new label rather than replacing prior text, so the
     * screen behaves like a persistent canvas. Same lifecycle caveat as
     * api_display_image_handler() applies. */
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label, font_for_size(size), 0);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Placed text '%s' at (%d,%d) size=%d", text, x, y, size);
    return send_ok(req);
}
