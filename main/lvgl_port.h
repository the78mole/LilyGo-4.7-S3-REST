#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Brings up LVGL (draw buffers + display driver bound to display_manager's
 * flush path), starts the tick timer and the core-1 lv_timer_handler()
 * task. display_manager_init() must have already succeeded. LVGL's own
 * small internal allocations (objects, styles) go through CONFIG_LV_MEM_CUSTOM
 * -> plain malloc/free, which CONFIG_SPIRAM_USE_MALLOC in sdkconfig.defaults
 * spills into PSRAM above a size threshold; the large, deliberately-PSRAM
 * buffers (draw buffers, grayscale scratch buffer) are allocated explicitly
 * here via heap_caps_malloc(..., MALLOC_CAP_SPIRAM).
 */
esp_err_t lvgl_port_init(void);

/*
 * LVGL is not thread-safe: every call into lv_* from outside the LVGL task
 * itself (i.e. from the HTTP server handlers) must be bracketed by these.
 */
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif
