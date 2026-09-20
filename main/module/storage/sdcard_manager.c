#include "sdcard_manager.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"

static sdmmc_card_t *s_card;
static sdmmc_host_t s_host;
static bool s_bus_initialized;
static char s_mount_point[96];

static void sdcard_manager_log_info(const char *tag)
{
    if (!s_card) {
        return;
    }

    uint16_t ccc = s_card->csd.card_command_class;
    char ccc_str[64] = {0};
    size_t ccc_pos = 0;
    static const struct { int bit; const char *name; } s_ccc_names[] = {
        {0, "basic"}, {2, "block-read"}, {4, "block-write"},
        {5, "erase"}, {6, "write-prot"}, {7, "lock"},
        {8, "app-spec"}, {10, "high-speed"},
    };
    for (size_t i = 0; i < sizeof(s_ccc_names) / sizeof(s_ccc_names[0]); i++) {
        if (ccc & (1 << s_ccc_names[i].bit)) {
            int written = snprintf(ccc_str + ccc_pos, sizeof(ccc_str) - ccc_pos,
                                    "%s%s", ccc_pos ? "," : "", s_ccc_names[i].name);
            if (written > 0) {
                ccc_pos += (size_t)written;
            }
        }
    }

    const char *card_type = "SDSC";
    if (s_card->ocr & (1 << 30)) {
        card_type = (s_card->csd.capacity > (32ULL * 1024 * 1024 * 1024 / 512)) ? "SDXC" : "SDHC";
    }

    double capacity_gb = (s_card->csd.capacity * 512.0) / (1024.0 * 1024.0 * 1024.0);

    // Monta cada valor como string primeiro, com unidade ja embutida,
    // depois aplica UM padding consistente sobre o resultado final.
    char line[96];
    const int width = 43; // largura interna da tabela, entre as bordas "| " e " |"

    ESP_LOGI(tag, "+-------------------------------------------+");
    ESP_LOGI(tag, "| SD Card Info                               |");
    ESP_LOGI(tag, "+-------------------------------------------+");

    snprintf(line, sizeof(line), "nome        : %s", s_card->cid.name);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "tipo        : %s", card_type);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "capacidade  : %.2f GB", capacity_gb);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "setor       : %u bytes", (unsigned)s_card->csd.sector_size);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "freq pedida : %u kHz", (unsigned)s_host.max_freq_khz);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "freq real   : %u kHz", (unsigned)s_card->real_freq_khz);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "freq max    : %u kHz", (unsigned)s_card->max_freq_khz);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "fabricante  : 0x%x", s_card->cid.mfg_id);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "oem id      : 0x%x", s_card->cid.oem_id);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "serial      : 0x%lx", (unsigned long)s_card->cid.serial);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "fabricacao  : %02u/%u",
             (unsigned)(s_card->cid.date & 0x0F), (unsigned)(2000 + (s_card->cid.date >> 4)));
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "revisao     : %u.%u",
             s_card->cid.revision >> 4, s_card->cid.revision & 0x0F);
    ESP_LOGI(tag, "| %-*s |", width, line);

    snprintf(line, sizeof(line), "ccc classes : %s", ccc_str);
    ESP_LOGI(tag, "| %-*s |", width, line);

    ESP_LOGI(tag, "+-------------------------------------------+");
}

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

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = pin_mosi,
        .miso_io_num = pin_miso,
        .sclk_io_num = pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8192, // acompanha o MUSIC_UPLOAD_CHUNK_SIZE atual (4096) com folga
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

    // Tenta do clock mais alto pro mais baixo; muitos cartoes/wiring
    // genericos falham em 40MHz mas funcionam bem em 26 ou 20.
    static const uint32_t s_freq_candidates_khz[] = { 26000, 24000, 22000, 20000, 16000, 10000 };
    bool mounted = false;

    for (size_t i = 0; i < sizeof(s_freq_candidates_khz) / sizeof(s_freq_candidates_khz[0]); i++) {
        s_host.max_freq_khz = s_freq_candidates_khz[i];

        ret = esp_vfs_fat_sdspi_mount(mount_point, &s_host, &slot_config, &mount_config, &s_card);
        if (ret == ESP_OK) {
            ESP_LOGI(tag, "SD montado com sucesso em %lu kHz", (unsigned long)s_freq_candidates_khz[i]);
            mounted = true;
            break;
        }

        ESP_LOGW(tag, "Falha montando SD em %lu kHz: %s — tentando frequencia menor",
                 (unsigned long)s_freq_candidates_khz[i], esp_err_to_name(ret));
    }

    if (!mounted) {
        ESP_LOGE(tag, "Falha ao montar SD em todas as frequencias testadas");
        spi_bus_free(s_host.slot);
        s_bus_initialized = false;
        return ret;
    }

    snprintf(s_mount_point, sizeof(s_mount_point), "%s", mount_point);

    if (sd_mounted) {
        *sd_mounted = true;
    }

    sdcard_manager_log_info(tag);

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
