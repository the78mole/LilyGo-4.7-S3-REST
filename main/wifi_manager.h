#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts Wi-Fi in STA mode and blocks until either an IP address has been
 * obtained or CONFIG_APP_WIFI_MAX_RETRY connection attempts have failed.
 * The underlying Wi-Fi driver task is pinned to CONFIG_APP_TASK_CORE_NETWORK
 * via CONFIG_ESP_WIFI_TASK_CORE_ID (see sdkconfig.defaults).
 */
esp_err_t wifi_manager_start(void);

bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
