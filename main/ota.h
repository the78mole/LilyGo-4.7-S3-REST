#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A/B firmware updates over the REST API.
 *
 * The image is streamed straight into whichever of ota_0 / ota_1 is not
 * currently running, so the running firmware stays intact for the whole
 * transfer and a failed or interrupted upload changes nothing. Only once the
 * complete image has been written and validated is the boot slot switched.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image boots as
 * PENDING_VERIFY: if it does not call ota_mark_valid() before the next reset,
 * the bootloader reverts to the slot that was running before. That is what
 * makes a bad update survivable without a cable.
 */

/*
 * Reports what the running image is and whether it is still on probation.
 * Call early in start-up, before the network is up, so the log records which
 * slot booted even if the rest of start-up fails.
 */
void ota_report_boot_state(void);

/*
 * Confirms the running image, cancelling any pending rollback.
 *
 * Call this only once the device can actually receive another update -- Wi-Fi
 * associated and the HTTP server accepting requests. That is the property
 * worth verifying: a firmware that reaches this point can always be replaced
 * over the air, and one that does not gets rolled back automatically.
 */
void ota_mark_valid(void);

/* POST /api/ota -- body is the raw .bin, streamed into the inactive slot.
 * Reboots into the new image shortly after answering. */
esp_err_t api_ota_handler(httpd_req_t *req);

/* GET /api/ota/status -- running slot, version, and rollback state. */
esp_err_t api_ota_status_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
