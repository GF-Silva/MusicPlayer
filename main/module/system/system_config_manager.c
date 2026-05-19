#include "system_config_manager.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"

#define SYSTEM_CONFIG_MAX_JSON_SIZE 4096
#define SYSTEM_CONFIG_PATH_LEN 192

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static esp_err_t build_system_dir_path(const char *sd_mount_point, char *path, size_t path_len)
{
    if (!sd_mount_point || sd_mount_point[0] == '\0' || !path || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(path, path_len, "%s/%s", sd_mount_point, SYSTEM_CONFIG_DIR_NAME);
    if (written < 0 || written >= (int)path_len) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t system_config_build_path(const char *sd_mount_point,
                                   const char *file_name,
                                   char *path,
                                   size_t path_len)
{
    if (!file_name || file_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(path, path_len, "%s/%s/%s", sd_mount_point, SYSTEM_CONFIG_DIR_NAME, file_name);
    if (written < 0 || written >= (int)path_len) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t ensure_system_dir(const char *sd_mount_point, const char *tag)
{
    char path[SYSTEM_CONFIG_PATH_LEN];
    esp_err_t ret = build_system_dir_path(sd_mount_point, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(tag, "Falha criando %s: errno=%d", path, errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static cJSON *build_default_config_json(const system_config_defaults_t *defaults)
{
    char mac[18];

    cJSON *root = cJSON_CreateObject();
    cJSON *bt = cJSON_CreateObject();
    cJSON *storage = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *runtime = cJSON_CreateObject();

    if (!root || !bt || !storage || !wifi || !audio || !runtime) {
        cJSON_Delete(root);
        cJSON_Delete(bt);
        cJSON_Delete(storage);
        cJSON_Delete(wifi);
        cJSON_Delete(audio);
        cJSON_Delete(runtime);
        return NULL;
    }

    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             defaults->target_mac[0],
             defaults->target_mac[1],
             defaults->target_mac[2],
             defaults->target_mac[3],
             defaults->target_mac[4],
             defaults->target_mac[5]);

    cJSON_AddStringToObject(root, "schema", "musicplayer.config.v1");
    cJSON_AddItemToObject(root, "bluetooth", bt);
    cJSON_AddStringToObject(bt, "device", defaults->bt_device);
    cJSON_AddStringToObject(bt, "target_mac", mac);
    cJSON_AddNumberToObject(bt, "discovery_timeout_sec", defaults->discovery_timeout_sec);
    cJSON_AddNumberToObject(bt, "connecting_stuck_ms", defaults->bt_connecting_stuck_ms);

    cJSON_AddItemToObject(root, "storage", storage);
    cJSON_AddStringToObject(storage, "sd_mount_point", defaults->sd_mount_point);
    cJSON_AddStringToObject(storage, "mount_point", defaults->music_mount_point);
    cJSON_AddStringToObject(storage, "system_dir", SYSTEM_CONFIG_DIR_NAME);

    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON_AddStringToObject(wifi, "ssid", defaults->wifi_ssid);
    cJSON_AddStringToObject(wifi, "password", defaults->wifi_password);
    cJSON_AddNumberToObject(wifi, "channel", defaults->wifi_channel);
    cJSON_AddNumberToObject(wifi, "max_connections", defaults->wifi_max_connections);

    cJSON_AddItemToObject(root, "audio", audio);
    cJSON_AddNumberToObject(audio, "default_volume", defaults->default_volume);
    cJSON_AddNumberToObject(audio, "volume_step", defaults->volume_step);
    cJSON_AddNumberToObject(audio, "stream_buffer_size", defaults->stream_buffer_size);
    cJSON_AddNumberToObject(audio, "stream_low_watermark_pct", defaults->stream_low_watermark_pct);
    cJSON_AddNumberToObject(audio, "stream_high_watermark_pct", defaults->stream_high_watermark_pct);
    cJSON_AddNumberToObject(audio, "mp3_read_min", defaults->mp3_read_min);
    cJSON_AddNumberToObject(audio, "mp3_read_max", defaults->mp3_read_max);

    cJSON_AddItemToObject(root, "runtime", runtime);
    cJSON_AddNumberToObject(runtime, "auto_sleep_idle_ms", defaults->auto_sleep_idle_ms);
    cJSON_AddNumberToObject(runtime, "decode_stall_recovery_ms", defaults->decode_stall_recovery_ms);

    return root;
}

static esp_err_t write_json_file_atomic(const char *path, const char *json, const char *tag)
{
    char temp_path[SYSTEM_CONFIG_PATH_LEN + 8];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (written < 0 || written >= (int)sizeof(temp_path)) {
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        ESP_LOGE(tag, "Falha abrindo %s: errno=%d", temp_path, errno);
        return ESP_FAIL;
    }

    size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, file) == len && fwrite("\n", 1, 1, file) == 1;
    if (fclose(file) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(temp_path);
        ESP_LOGE(tag, "Falha gravando %s", temp_path);
        return ESP_FAIL;
    }

    remove(path);
    if (rename(temp_path, path) != 0) {
        ESP_LOGE(tag, "Falha renomeando %s para %s: errno=%d", temp_path, path, errno);
        remove(temp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t write_default_config(const char *sd_mount_point,
                                      const system_config_defaults_t *defaults,
                                      const char *tag)
{
    char path[SYSTEM_CONFIG_PATH_LEN];
    esp_err_t ret = system_config_build_path(sd_mount_point, SYSTEM_CONFIG_FILE_NAME, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *root = build_default_config_json(defaults);
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    ret = write_json_file_atomic(path, json, tag);
    cJSON_free(json);
    return ret;
}

esp_err_t system_config_replace_json(const char *sd_mount_point,
                                     const char *json,
                                     size_t json_len,
                                     const char *tag)
{
    if (!json || json_len == 0 || json_len > SYSTEM_CONFIG_MAX_JSON_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char *normalized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!normalized) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ensure_system_dir(sd_mount_point, tag);
    if (ret == ESP_OK) {
        char path[SYSTEM_CONFIG_PATH_LEN];
        ret = system_config_build_path(sd_mount_point, SYSTEM_CONFIG_FILE_NAME, path, sizeof(path));
        if (ret == ESP_OK) {
            ret = write_json_file_atomic(path, normalized, tag);
        }
    }

    cJSON_free(normalized);
    return ret;
}

esp_err_t system_config_append_error(const char *sd_mount_point,
                                     const char *tag,
                                     const char *message)
{
    if (!message || message[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_system_dir(sd_mount_point, tag);
    if (ret != ESP_OK) {
        return ret;
    }

    char path[SYSTEM_CONFIG_PATH_LEN];
    ret = system_config_build_path(sd_mount_point, SYSTEM_ERROR_LOG_FILE_NAME, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *file = fopen(path, "ab");
    if (!file) {
        ESP_LOGE(tag, "Falha abrindo log %s: errno=%d", path, errno);
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    fprintf(file, "{\"ts\":%ld,\"tag\":\"%s\",\"erro\":\"%s\"}\n", (long)now, tag ? tag : "", message);
    fclose(file);
    return ESP_OK;
}

esp_err_t system_config_ensure(const char *sd_mount_point,
                               const system_config_defaults_t *defaults,
                               const char *tag)
{
    if (!defaults) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_system_dir(sd_mount_point, tag);
    if (ret != ESP_OK) {
        return ret;
    }

    char config_path[SYSTEM_CONFIG_PATH_LEN];
    ret = system_config_build_path(sd_mount_point, SYSTEM_CONFIG_FILE_NAME, config_path, sizeof(config_path));
    if (ret != ESP_OK) {
        return ret;
    }

    if (!file_exists(config_path)) {
        ESP_LOGI(tag, "Criando config default em %s", config_path);
        ret = write_default_config(sd_mount_point, defaults, tag);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    char log_path[SYSTEM_CONFIG_PATH_LEN];
    ret = system_config_build_path(sd_mount_point, SYSTEM_ERROR_LOG_FILE_NAME, log_path, sizeof(log_path));
    if (ret != ESP_OK) {
        return ret;
    }

    if (!file_exists(log_path)) {
        FILE *file = fopen(log_path, "ab");
        if (!file) {
            ESP_LOGE(tag, "Falha criando %s: errno=%d", log_path, errno);
            return ESP_FAIL;
        }
        fclose(file);
    }

    return ESP_OK;
}
