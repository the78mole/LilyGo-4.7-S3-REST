#include "ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

/* Big enough that the write loop is not syscall-bound, small enough to sit in
 * internal RAM without competing with the panel framebuffers. */
#define OTA_CHUNK_SIZE 4096

/* Delay before restarting, so the HTTP response reaches the client before the
 * socket dies with the rest of the system. */
#define OTA_REBOOT_DELAY_MS 1500

/* Serialises updates: two concurrent uploads would interleave writes into the
 * same slot and produce a corrupt image that still passes a length check. */
static volatile bool s_update_running;

static esp_err_t send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    ESP_LOGE(TAG, "%s", msg);
    httpd_resp_send_err(req, code, msg);
    return ESP_FAIL;
}

void ota_report_boot_state(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "running '%s' (%s) -- PENDING VERIFY, will roll back "
                      "unless start-up completes",
                 running->label, desc->version);
    } else {
        ESP_LOGI(TAG, "running '%s' (%s, built %s %s)", running->label,
                 desc->version, desc->date, desc->time);
    }
}

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;  /* not on probation -- nothing to confirm */
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "image on '%s' confirmed, rollback cancelled", running->label);
    } else {
        /* Leaving it pending is the safe outcome: the next reset returns to
         * the previous image rather than keeping an unconfirmed one. */
        ESP_LOGE(TAG, "could not confirm image: %s", esp_err_to_name(err));
    }
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    ESP_LOGW(TAG, "restarting into the updated image");
    esp_restart();
}

esp_err_t api_ota_handler(httpd_req_t *req)
{
    if (s_update_running) {
        /* httpd_err_code_t has no 409, so set the status directly -- the
         * distinction matters to a client deciding whether to retry. */
        ESP_LOGE(TAG, "rejected: an update is already in progress");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"an update is already in progress\"}");
        return ESP_FAIL;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        /* Happens on a single-app partition table -- i.e. firmware flashed
         * before the A/B layout was introduced. */
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no OTA slot available (partition table has no ota_0/ota_1)");
    }

    if (req->content_len <= 0) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "missing Content-Length");
    }
    if ((size_t)req->content_len > target->size) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "image larger than the OTA slot");
    }

    char *chunk = malloc(OTA_CHUNK_SIZE);
    if (!chunk) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    s_update_running = true;
    ESP_LOGI(TAG, "update starting: %d bytes -> '%s'", req->content_len, target->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        free(chunk);
        s_update_running = false;
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    int remaining = req->content_len;
    bool failed = false;
    while (remaining > 0) {
        int to_read = remaining < OTA_CHUNK_SIZE ? remaining : OTA_CHUNK_SIZE;
        int received = httpd_req_recv(req, chunk, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "transfer aborted with %d bytes to go", remaining);
            failed = true;
            break;
        }
        err = esp_ota_write(handle, chunk, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }
        remaining -= received;
    }

    free(chunk);

    if (failed) {
        esp_ota_abort(handle);
        s_update_running = false;
        /* The running slot was never touched, so the device stays usable and
         * the client can simply retry. */
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "transfer failed; running firmware untouched");
    }

    /* Verifies the image header and checksum -- a truncated or corrupted
     * upload is rejected here rather than at the next boot. */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        s_update_running = false;
        return send_err(req, HTTPD_400_BAD_REQUEST,
                        err == ESP_ERR_OTA_VALIDATE_FAILED
                            ? "image failed validation; running firmware untouched"
                            : esp_err_to_name(err));
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        s_update_running = false;
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "update written to '%s', rebooting in %d ms", target->label,
             OTA_REBOOT_DELAY_MS);

    httpd_resp_set_type(req, "application/json");
    char body[160];
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"slot\":\"%s\",\"bytes\":%d,\"reboot_in_ms\":%d}",
             target->label, req->content_len, OTA_REBOOT_DELAY_MS);
    httpd_resp_sendstr(req, body);

    /* s_update_running stays set: the device is on its way down and must not
     * accept another upload in the meantime. */
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}

esp_err_t api_ota_status_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *desc = esp_app_get_description();

    esp_ota_img_states_t state;
    bool pending = esp_ota_get_state_partition(running, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;

    char body[320];
    snprintf(body, sizeof(body),
             "{\"running\":\"%s\",\"next\":\"%s\",\"version\":\"%s\","
             "\"built\":\"%s %s\",\"idf\":\"%s\",\"pending_verify\":%s,"
             "\"slot_size\":%" PRIu32 "}",
             running->label, next ? next->label : "none", desc->version,
             desc->date, desc->time, desc->idf_ver, pending ? "true" : "false",
             next ? next->size : 0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}
