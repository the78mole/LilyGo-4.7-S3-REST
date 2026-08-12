#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* POST /api/upload?filename=<name>
 * Body: raw bytes, streamed straight to <mount>/<name> without buffering
 * the whole payload in RAM. */
esp_err_t api_upload_handler(httpd_req_t *req);

/* POST /api/display/image
 * Body: {"filename": "image.bin", "x": <int>, "y": <int>}
 * Loads <mount>/<filename> (see app_config.h for the raw format) and draws
 * it unscaled at (x, y). */
esp_err_t api_display_image_handler(httpd_req_t *req);

/* POST /api/display/text
 * Body: {"text": "<string>", "x": <int>, "y": <int>, "size": <int>} */
esp_err_t api_display_text_handler(httpd_req_t *req);

/* POST /api/display/chart
 * Body (minimal): {"values": [<1..96 numbers>]}
 * Optional: "slot": "today"|"tomorrow", "title", "x","y","w","h",
 *           "y_max", "interval_min", "highlight_now".
 * Axis range, slot resolution and cell geometry default to the firmware's
 * Kconfig values, so a client normally only sends the data points. The
 * highlighted slot is derived from the device's own NTP-synced clock. */
esp_err_t api_display_chart_handler(httpd_req_t *req);

/* POST /api/display/list
 * Body: {"items": [{"text": str, "done": bool}, ...], "slot": "...", "title": str} */
esp_err_t api_display_list_handler(httpd_req_t *req);

/* POST /api/display/weather
 * Body: {"condition": <HA slug>, "temp": num, "temp_min"/"temp_max": num,
 *        "precip": num, "wind": num, "forecast": [{"label": str, "temp": num}],
 *        "slot": "...", "title": str} */
esp_err_t api_display_weather_handler(httpd_req_t *req);

/* GET /api/health */
esp_err_t api_health_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
