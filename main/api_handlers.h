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

/* GET /api/health */
esp_err_t api_health_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
