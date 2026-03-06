#include "sdcard_manager.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

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

    ESP_LOGI(tag, "Inicializando SD Card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 19000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = pin_mosi,
        .miso_io_num = pin_miso,
        .sclk_io_num = pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Falha SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = pin_cs;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Falha montar SD: %s", esp_err_to_name(ret));
        return ret;
    }

    if (sd_mounted) {
        *sd_mounted = true;
    }

    ESP_LOGI(tag, "SD Card montado: %s", card->cid.name);
    ESP_LOGI(tag, "Tamanho: %.2f GB", (card->csd.capacity * 512.0) / (1024 * 1024 * 1024));

    return ESP_OK;
}
