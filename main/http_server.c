#include "http_server.h"

#include "api_handlers.h"
#include "app_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ota.h"
#include "sdkconfig.h"

static const char *TAG = "http_server";

esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_APP_HTTP_PORT;
    config.core_id = APP_NETWORK_CORE;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* Defaults to 8, which the route table below has already outgrown --
     * registration then fails with ESP_ERR_HTTPD_HANDLERS_FULL and the
     * ESP_ERROR_CHECK turns that into a boot loop. Keep this comfortably
     * above the number of routes. */
    config.max_uri_handlers = 16;
    /* Body sizes are validated per-handler against req->content_len /
     * CONFIG_APP_HTTP_MAX_UPLOAD_BYTES; httpd itself just needs enough
     * head-room for the largest declared Content-Length it will parse. */
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        {
            .uri = "/api/health",
            .method = HTTP_GET,
            .handler = api_health_handler,
        },
        {
            .uri = "/api/upload",
            .method = HTTP_POST,
            .handler = api_upload_handler,
        },
        {
            .uri = "/api/display/image",
            .method = HTTP_POST,
            .handler = api_display_image_handler,
        },
        {
            .uri = "/api/display/text",
            .method = HTTP_POST,
            .handler = api_display_text_handler,
        },
        {
            .uri = "/api/display/list",
            .method = HTTP_POST,
            .handler = api_display_list_handler,
        },
        {
            .uri = "/api/display/agenda",
            .method = HTTP_POST,
            .handler = api_display_agenda_handler,
        },
        {
            .uri = "/api/display/weather",
            .method = HTTP_POST,
            .handler = api_display_weather_handler,
        },
        {
            .uri = "/api/display/chart",
            .method = HTTP_POST,
            .handler = api_display_chart_handler,
        },
        {
            .uri = "/api/ota",
            .method = HTTP_POST,
            .handler = api_ota_handler,
        },
        {
            .uri = "/api/ota/status",
            .method = HTTP_GET,
            .handler = api_ota_status_handler,
        },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "REST API listening on port %d (core %d)", CONFIG_APP_HTTP_PORT,
             APP_NETWORK_CORE);
    return ESP_OK;
}
