#include "time_sync.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

static const char *TAG = "time_sync";

/* Anything before 2023-01-01 means SNTP has not landed yet (the RTC starts at
 * the epoch after a cold boot). */
#define PLAUSIBLE_EPOCH 1672531200L

esp_err_t time_sync_start(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_APP_NTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* POSIX TZ string -- note the sign convention is inverted relative to UTC
     * offsets (CET-1 means UTC+1). Default covers Europe/Berlin including DST. */
    setenv("TZ", CONFIG_APP_TIMEZONE, 1);
    tzset();

    ESP_LOGI(TAG, "SNTP started (server=%s, TZ=%s)",
             CONFIG_APP_NTP_SERVER, CONFIG_APP_TIMEZONE);
    return ESP_OK;
}

bool time_sync_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return now >= PLAUSIBLE_EPOCH;
}

int time_sync_current_slot(int interval_min)
{
    if (interval_min <= 0 || !time_sync_is_valid()) {
        return -1;
    }

    time_t now = 0;
    time(&now);
    struct tm lt;
    localtime_r(&now, &lt);

    int minutes_into_day = lt.tm_hour * 60 + lt.tm_min;
    int slot = minutes_into_day / interval_min;

    int slots_per_day = (24 * 60) / interval_min;
    if (slot >= slots_per_day) {
        slot = slots_per_day - 1;
    }
    return slot;
}

void time_sync_format_hhmm(char *out, size_t out_len)
{
    if (out_len < 6) {
        return;
    }
    if (!time_sync_is_valid()) {
        strlcpy(out, "--:--", out_len);
        return;
    }
    time_t now = 0;
    time(&now);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(out, out_len, "%02d:%02d", lt.tm_hour, lt.tm_min);
}
