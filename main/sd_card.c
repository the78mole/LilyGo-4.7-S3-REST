#include "sd_card.h"

#include "app_config.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";

static SemaphoreHandle_t s_sd_mutex;
static sdmmc_card_t *s_card;

esp_err_t sd_card_init(void)
{
    s_sd_mutex = xSemaphoreCreateMutex();
    if (!s_sd_mutex) {
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, /* never auto-format a user's card */
        .max_files = APP_SD_MAX_FILES_OPEN,
        .allocation_unit_size = 16 * 1024,
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_APP_SD_PIN_MOSI,
        .miso_io_num = CONFIG_APP_SD_PIN_MISO,
        .sclk_io_num = CONFIG_APP_SD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t err = spi_bus_initialize((spi_host_device_t)CONFIG_APP_SD_SPI_HOST,
                                        &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = CONFIG_APP_SD_SPI_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_APP_SD_PIN_CS;
    slot_config.host_id = (spi_host_device_t)CONFIG_APP_SD_SPI_HOST;

    err = esp_vfs_fat_sdspi_mount(APP_SD_MOUNT_POINT, &host, &slot_config,
                                   &mount_config, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. Card present but "
                          "unformatted/corrupt -- refusing to auto-format.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s (check wiring / "
                          "APP_SD_PIN_* Kconfig values)", esp_err_to_name(err));
        }
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool sd_card_lock(TickType_t timeout_ticks)
{
    return xSemaphoreTake(s_sd_mutex, timeout_ticks) == pdTRUE;
}

void sd_card_unlock(void)
{
    xSemaphoreGive(s_sd_mutex);
}

const char *sd_card_mount_point(void)
{
    return APP_SD_MOUNT_POINT;
}
