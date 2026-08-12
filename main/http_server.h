#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts esp_http_server with its task pinned to CONFIG_APP_TASK_CORE_NETWORK
 * and registers the /api endpoint handlers from api_handlers.h. */
esp_err_t http_server_start(void);

#ifdef __cplusplus
}
#endif
