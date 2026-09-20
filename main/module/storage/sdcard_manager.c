#include "sdcard_manager.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static sdmmc_card_t *s_card;
static sdmmc_host_t s_host;
static bool s_bus_initialized;
static char s_mount_point[96];

esp_err_t sdcard_manager_mount_sdspi(const char *tag,
                                     int pin_mosi,
                                     int pin_miso,
                                     int pin_clk,
                                     int pin_cs,
                                     const char *mount_point,
                                     bool *sd_mounted)
{
    if (!tag || !mount_point) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_card) {
        if (strcmp(s_mount_point, mount_point) == 0) {
            if (sd_mounted) {
                *sd_mounted = true;
            }
            return ESP_OK;
        }
        ESP_LOGW(tag, "SD já montado em %s", s_mount_point);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(tag, "Inicializando SD Card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 32 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    s_host = host;
    s_host.max_freq_khz = 19000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = pin_mosi,
        .miso_io_num = pin_miso,
        .sclk_io_num = pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(s_host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Falha SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    s_bus_initialized = true;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = pin_cs;
    slot_config.host_id = s_host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &s_host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Falha montar SD: %s", esp_err_to_name(ret));
        spi_bus_free(s_host.slot);
        s_bus_initialized = false;
        return ret;
    }

    snprintf(s_mount_point, sizeof(s_mount_point), "%s", mount_point);

    if (sd_mounted) {
        *sd_mounted = true;
    }

    ESP_LOGI(tag, "SD Card montado em %s: %s", s_mount_point, s_card->cid.name);
    ESP_LOGI(tag, "Tamanho: %.2f GB", (s_card->csd.capacity * 512.0) / (1024 * 1024 * 1024));

    return ESP_OK;
}

esp_err_t sdcard_manager_unmount(const char *tag, bool *sd_mounted)
{
    if (!s_card) {
        if (sd_mounted) {
            *sd_mounted = false;
        }
        return ESP_OK;
    }

    esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    s_card = NULL;

    if (s_bus_initialized) {
        spi_bus_free(s_host.slot);
        s_bus_initialized = false;
    }

    ESP_LOGI(tag ? tag : "sdcard", "SD Card desmontado de %s", s_mount_point);
    s_mount_point[0] = '\0';

    if (sd_mounted) {
        *sd_mounted = false;
    }

    return ESP_OK;
}
