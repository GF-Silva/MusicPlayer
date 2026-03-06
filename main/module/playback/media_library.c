#include "media_library.h"

#include <dirent.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

#define SAMPLE_RATE_44K1 44100
#define SAMPLE_RATE_48K  48000

int media_skip_id3v2(FILE *f, const char *tag)
{
    uint8_t header[10];

    if (fread(header, 1, 10, f) != 10) {
        fseek(f, 0, SEEK_SET);
        return 0;
    }

    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        uint32_t tag_size = ((header[6] & 0x7F) << 21) |
                            ((header[7] & 0x7F) << 14) |
                            ((header[8] & 0x7F) << 7) |
                            (header[9] & 0x7F);

        ESP_LOGI(tag, "ID3v2 tag: %" PRIu32 " bytes (pulando)", tag_size);

        fseek(f, 10 + tag_size, SEEK_SET);
        return (int)(tag_size + 10);
    }

    fseek(f, 0, SEEK_SET);
    return 0;
}

bool media_drop_trailing_tag_if_present(uint8_t **ptr, int *bytes_left, const char *tag)
{
    if (!ptr || !*ptr || !bytes_left || *bytes_left <= 0) {
        return false;
    }

    if (*bytes_left >= 128 && memcmp(*ptr, "TAG", 3) == 0) {
        ESP_LOGI(tag, "Fim: ID3v1 tail detectado (%d bytes restantes)", *bytes_left);
        *ptr += *bytes_left;
        *bytes_left = 0;
        return true;
    }

    if (*bytes_left >= 32 && memcmp(*ptr, "APETAGEX", 8) == 0) {
        ESP_LOGI(tag, "Fim: APE tag detectado (%d bytes restantes)", *bytes_left);
        *ptr += *bytes_left;
        *bytes_left = 0;
        return true;
    }

    return false;
}

esp_err_t media_count_mp3_files(const char *mount_point, int *out_count, const char *tag)
{
    if (!mount_point || !out_count) return ESP_ERR_INVALID_ARG;

    DIR *dir = opendir(mount_point);
    if (!dir) {
        ESP_LOGE(tag, "Falha abrir diretório");
        return ESP_FAIL;
    }

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcasecmp(entry->d_name + len - 4, ".mp3") == 0) {
                count++;
            }
        }
    }

    closedir(dir);

    if (count == 0) {
        ESP_LOGE(tag, "Nenhum arquivo MP3 encontrado");
        return ESP_ERR_NOT_FOUND;
    }

    *out_count = count;
    ESP_LOGI(tag, "Total: %d arquivos MP3 encontrados", count);
    return ESP_OK;
}

esp_err_t media_get_mp3_path(const char *mount_point, int index, char *path, size_t max_len)
{
    if (!mount_point || !path || max_len == 0 || index < 0) return ESP_ERR_INVALID_ARG;

    DIR *dir = opendir(mount_point);
    if (!dir) return ESP_FAIL;

    int count = 0;
    struct dirent *entry;
    esp_err_t ret = ESP_ERR_NOT_FOUND;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcasecmp(entry->d_name + len - 4, ".mp3") == 0) {
                if (count == index) {
                    int written = snprintf(path, max_len, "%s/%s", mount_point, entry->d_name);
                    if (written < 0 || written >= (int)max_len) {
                        ret = ESP_ERR_NO_MEM;
                    } else {
                        ret = ESP_OK;
                    }
                    break;
                }
                count++;
            }
        }
    }

    closedir(dir);
    return ret;
}

esp_err_t media_analyze_mp3_file(const char *path, mp3_info_t *info, const char *tag)
{
    if (!path || !info) return ESP_ERR_INVALID_ARG;

    memset(info, 0, sizeof(*info));

    struct stat file_stat;
    if (stat(path, &file_stat) != 0) {
        return ESP_FAIL;
    }

    info->file_size = file_stat.st_size;

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }

    media_skip_id3v2(f, tag);

    uint8_t header_buf[4];
    if (fread(header_buf, 1, 4, f) != 4) {
        fclose(f);
        return ESP_FAIL;
    }

    if (header_buf[0] != 0xFF || (header_buf[1] & 0xE0) != 0xE0) {
        fclose(f);
        return ESP_FAIL;
    }

    int bitrate_index = (header_buf[2] >> 4) & 0x0F;
    int samplerate_index = (header_buf[2] >> 2) & 0x03;

    static const int bitrates[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };

    static const int samplerates[4] = {44100, 48000, 32000, 0};

    info->bitrate = (uint32_t)bitrates[bitrate_index] * 1000U;
    info->sample_rate = (uint32_t)samplerates[samplerate_index];
    info->channels = 2;
    info->bits_per_sample = 16;

    if (info->bitrate > 0) {
        info->duration_seconds = (info->file_size * 8U) / info->bitrate;
    }

    info->a2dp_compatible = (info->sample_rate == SAMPLE_RATE_44K1 ||
                             info->sample_rate == SAMPLE_RATE_48K);

    info->is_valid_mp3 = (info->sample_rate > 0 && info->bitrate > 0);

    fclose(f);

    ESP_LOGI(tag, "Analise MP3: %" PRIu32 " Hz | %" PRIu32 " kbps | %u ch | %" PRIu32 " s",
             info->sample_rate,
             info->bitrate / 1000U,
             (unsigned)info->channels,
             info->duration_seconds);

    return ESP_OK;
}
