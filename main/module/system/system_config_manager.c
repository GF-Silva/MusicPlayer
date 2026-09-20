#include "system_config_manager.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"

#define SYSTEM_CONFIG_MAX_JSON_SIZE 4096
#define SYSTEM_CONFIG_PATH_LEN 192

static esp_err_t regular_file_exists(const char *path, bool *exists, const char *tag)
{
    struct stat st;

    if (!path || !exists) {
        return ESP_ERR_INVALID_ARG;
    }

    *exists = false;

    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            ESP_LOGE(tag, "Caminho existe, mas nao e arquivo regular: %s", path);
            return ESP_FAIL;
        }
        *exists = true;
        return ESP_OK;
    }

    int stat_errno = errno;
    if (stat_errno == ENOENT) {
        return ESP_OK;
    }

    ESP_LOGE(tag, "Falha acessando %s: errno=%d", path, stat_errno);
    return ESP_FAIL;
}

static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_len, "%s", src);
}

static void config_from_defaults(const system_config_defaults_t *defaults, system_config_t *config)
{
    memset(config, 0, sizeof(*config));

    copy_string(config->bt_device, sizeof(config->bt_device), defaults->bt_device);
    copy_string(config->sd_mount_point, sizeof(config->sd_mount_point), defaults->sd_mount_point);
    copy_string(config->music_mount_point, sizeof(config->music_mount_point), defaults->music_mount_point);
    copy_string(config->wifi_ssid, sizeof(config->wifi_ssid), defaults->wifi_ssid);
    copy_string(config->wifi_password, sizeof(config->wifi_password), defaults->wifi_password);
    config->wifi_channel = defaults->wifi_channel;
    config->wifi_max_connections = defaults->wifi_max_connections;
    config->default_volume = defaults->default_volume;
    config->volume_step = defaults->volume_step;
    config->auto_sleep_idle_ms = defaults->auto_sleep_idle_ms;
    config->discovery_timeout_sec = defaults->discovery_timeout_sec;
    config->bt_connecting_stuck_ms = defaults->bt_connecting_stuck_ms;
    config->decode_stall_recovery_ms = defaults->decode_stall_recovery_ms;
    config->stream_buffer_size = defaults->stream_buffer_size;
    config->stream_low_watermark_pct = defaults->stream_low_watermark_pct;
    config->stream_high_watermark_pct = defaults->stream_high_watermark_pct;
    config->mp3_read_min = defaults->mp3_read_min;
    config->mp3_read_max = defaults->mp3_read_max;
    memcpy(config->target_mac, defaults->target_mac, sizeof(config->target_mac));
}

static bool path_has_traversal(const char *value)
{
    return value && (strstr(value, "../") || strstr(value, "/..") || strcmp(value, "..") == 0);
}

static bool valid_sd_mount_point(const char *value)
{
    return value && value[0] == '/' && strlen(value) < SYSTEM_CONFIG_MOUNT_POINT_LEN &&
           !path_has_traversal(value) && !strchr(value, '\\');
}

static bool valid_music_mount_point(const char *value)
{
    return value && value[0] != '\0' && strlen(value) < SYSTEM_CONFIG_MOUNT_POINT_LEN &&
           !path_has_traversal(value) && !strchr(value, '\\');
}

static bool valid_wifi_password(const char *value)
{
    size_t len = value ? strlen(value) : 0;
    return len == 0 || (len >= 8 && len < SYSTEM_CONFIG_WIFI_PASSWORD_LEN);
}

static bool read_file_to_string(const char *path, char **out_json, size_t *out_len, const char *tag)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(tag, "Falha abrindo config %s: errno=%d", path, errno);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    long file_len = ftell(file);
    if (file_len <= 0 || file_len > SYSTEM_CONFIG_MAX_JSON_SIZE || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    char *json = calloc(1, (size_t)file_len + 1);
    if (!json) {
        fclose(file);
        return false;
    }

    size_t read_len = fread(json, 1, (size_t)file_len, file);
    fclose(file);
    if (read_len != (size_t)file_len) {
        free(json);
        return false;
    }

    *out_json = json;
    *out_len = read_len;
    return true;
}

static void apply_string_field(const cJSON *parent,
                               const char *key,
                               char *dst,
                               size_t dst_len,
                               bool allow_empty,
                               bool (*validator)(const char *))
{
    const cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, key) : NULL;
    if (!cJSON_IsString(item) || !item->valuestring) {
        return;
    }

    size_t len = strlen(item->valuestring);
    if (len >= dst_len || (!allow_empty && len == 0)) {
        return;
    }

    if (validator && !validator(item->valuestring)) {
        return;
    }

    copy_string(dst, dst_len, item->valuestring);
}

static void apply_u32_field(const cJSON *parent,
                            const char *key,
                            uint32_t min_value,
                            uint32_t max_value,
                            uint32_t *dst)
{
    const cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, key) : NULL;
    if (!cJSON_IsNumber(item)) {
        return;
    }

    double value = item->valuedouble;
    if (value < (double)min_value || value > (double)max_value) {
        return;
    }

    *dst = (uint32_t)value;
}

static void apply_u8_field(const cJSON *parent,
                           const char *key,
                           uint8_t min_value,
                           uint8_t max_value,
                           uint8_t *dst)
{
    uint32_t value = *dst;
    apply_u32_field(parent, key, min_value, max_value, &value);
    *dst = (uint8_t)value;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_mac_string(const char *value, uint8_t out_mac[6])
{
    if (!value || strlen(value) != 17) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        int pos = i * 3;
        int high = hex_nibble(value[pos]);
        int low = hex_nibble(value[pos + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        if (i < 5 && value[pos + 2] != ':') {
            return false;
        }
        out_mac[i] = (uint8_t)((high << 4) | low);
    }

    return true;
}

static void apply_mac_field(const cJSON *parent, const char *key, uint8_t dst[6])
{
    const cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, key) : NULL;
    uint8_t parsed[6];
    if (cJSON_IsString(item) && parse_mac_string(item->valuestring, parsed)) {
        memcpy(dst, parsed, sizeof(parsed));
    }
}

static bool schema_is_valid_or_absent(const cJSON *root)
{
    const cJSON *schema = root ? cJSON_GetObjectItemCaseSensitive(root, "schema") : NULL;
    return !schema || (cJSON_IsString(schema) && strcmp(schema->valuestring, SYSTEM_CONFIG_SCHEMA) == 0);
}

static void set_validation_error(char *error, size_t error_len, const char *format, ...)
{
    if (!error || error_len == 0) {
        return;
    }

    va_list args;
    va_start(args, format);
    vsnprintf(error, error_len, format, args);
    va_end(args);
}

static void format_field_path(char *dst, size_t dst_len, const char *scope, const char *key)
{
    if (!dst || dst_len == 0) {
        return;
    }

    if (scope && scope[0] != '\0') {
        snprintf(dst, dst_len, "%s.%s", scope, key ? key : "");
    } else {
        snprintf(dst, dst_len, "%s", key ? key : "");
    }
}

static bool key_is_allowed(const char *key, const char *const *allowed)
{
    if (!key || !allowed) {
        return false;
    }

    for (size_t i = 0; allowed[i]; i++) {
        if (strcmp(key, allowed[i]) == 0) {
            return true;
        }
    }

    return false;
}

static bool validate_allowed_keys(const cJSON *object,
                                  const char *scope,
                                  const char *const *allowed,
                                  char *error,
                                  size_t error_len)
{
    for (const cJSON *child = object ? object->child : NULL; child; child = child->next) {
        if (!key_is_allowed(child->string, allowed)) {
            char field[96];
            format_field_path(field, sizeof(field), scope, child->string ? child->string : "?");
            set_validation_error(error, error_len, "campo %s nao e suportado", field);
            return false;
        }
    }

    return true;
}

static const cJSON *get_required_field(const cJSON *parent,
                                       const char *scope,
                                       const char *key,
                                       char *error,
                                       size_t error_len)
{
    const cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, key) : NULL;
    if (!item) {
        char field[96];
        format_field_path(field, sizeof(field), scope, key);
        set_validation_error(error, error_len, "config faltando: %s", field);
    }
    return item;
}

static bool validate_required_object(const cJSON *parent,
                                     const char *scope,
                                     const char *key,
                                     const cJSON **out,
                                     char *error,
                                     size_t error_len)
{
    const cJSON *item = get_required_field(parent, scope, key, error, error_len);
    if (!item) {
        return false;
    }

    if (!cJSON_IsObject(item)) {
        char field[96];
        format_field_path(field, sizeof(field), scope, key);
        set_validation_error(error, error_len, "campo %s deve ser objeto", field);
        return false;
    }

    if (out) {
        *out = item;
    }
    return true;
}

static bool validate_required_string(const cJSON *parent,
                                     const char *scope,
                                     const char *key,
                                     size_t max_len,
                                     bool allow_empty,
                                     bool (*validator)(const char *),
                                     char *error,
                                     size_t error_len)
{
    const cJSON *item = get_required_field(parent, scope, key, error, error_len);
    if (!item) {
        return false;
    }

    if (!cJSON_IsString(item) || !item->valuestring) {
        char field[96];
        format_field_path(field, sizeof(field), scope, key);
        set_validation_error(error, error_len, "campo %s deve ser string", field);
        return false;
    }

    size_t len = strlen(item->valuestring);
    if (!allow_empty && len == 0) {
        char field[96];
        format_field_path(field, sizeof(field), scope, key);
        set_validation_error(error, error_len, "campo %s nao pode ser vazio", field);
        return false;
    }

    if (len >= max_len) {
        char field[96];
        format_field_path(field, sizeof(field), scope, key);
        set_validation_error(error, error_len, "campo %s muito longo", field);
        return false;
    }

    if (validator && !validator(item->valuestring)) {
        char field[96];
        format_field_path(field, sizeof(field), scope, key);
        set_validation_error(error, error_len, "campo %s contem valor invalido", field);
        return false;
    }

    return true;
}

static bool validate_required_u32(const cJSON *parent,
                                  const char *scope,
                                  const char *key,
                                  uint32_t min_value,
                                  uint32_t max_value,
                                  uint32_t *out,
                                  char *error,
                                  size_t error_len)
{
    const cJSON *item = get_required_field(parent, scope, key, error, error_len);
    if (!item) {
        return false;
    }

    char field[96];
    format_field_path(field, sizeof(field), scope, key);
    if (!cJSON_IsNumber(item)) {
        set_validation_error(error, error_len, "campo %s deve ser numero", field);
        return false;
    }

    double value = item->valuedouble;
    if (!(value >= (double)min_value && value <= (double)max_value)) {
        set_validation_error(error,
                             error_len,
                             "campo %s fora do intervalo %lu-%lu",
                             field,
                             (unsigned long)min_value,
                             (unsigned long)max_value);
        return false;
    }

    uint32_t int_value = (uint32_t)value;
    if (value != (double)int_value) {
        set_validation_error(error, error_len, "campo %s deve ser inteiro", field);
        return false;
    }

    if (out) {
        *out = int_value;
    }
    return true;
}

static bool valid_mac_string(const char *value)
{
    uint8_t parsed[6];
    return parse_mac_string(value, parsed);
}

static bool validate_config_tree(const cJSON *root, char *error, size_t error_len)
{
    static const char *const root_keys[] = {
        "schema",
        "bluetooth",
        "storage",
        "wifi",
        "audio",
        "runtime",
        NULL,
    };
    static const char *const bluetooth_keys[] = {
        "device",
        "target_mac",
        "discovery_timeout_sec",
        "connecting_stuck_ms",
        NULL,
    };
    static const char *const storage_keys[] = {
        "sd_mount_point",
        "mount_point",
        NULL,
    };
    static const char *const wifi_keys[] = {
        "ssid",
        "password",
        "channel",
        "max_connections",
        NULL,
    };
    static const char *const audio_keys[] = {
        "default_volume",
        "volume_step",
        "stream_buffer_size",
        "stream_low_watermark_pct",
        "stream_high_watermark_pct",
        "mp3_read_min",
        "mp3_read_max",
        NULL,
    };
    static const char *const runtime_keys[] = {
        "auto_sleep_idle_ms",
        "decode_stall_recovery_ms",
        NULL,
    };

    if (!validate_allowed_keys(root, "", root_keys, error, error_len)) {
        return false;
    }

    const cJSON *schema = get_required_field(root, "", "schema", error, error_len);
    if (!schema) {
        return false;
    }
    if (!cJSON_IsString(schema) || !schema->valuestring ||
        strcmp(schema->valuestring, SYSTEM_CONFIG_SCHEMA) != 0) {
        set_validation_error(error,
                             error_len,
                             "campo schema deve ser %s",
                             SYSTEM_CONFIG_SCHEMA);
        return false;
    }

    const cJSON *bt = NULL;
    const cJSON *storage = NULL;
    const cJSON *wifi = NULL;
    const cJSON *audio = NULL;
    const cJSON *runtime = NULL;

    if (!validate_required_object(root, "", "bluetooth", &bt, error, error_len) ||
        !validate_required_object(root, "", "storage", &storage, error, error_len) ||
        !validate_required_object(root, "", "wifi", &wifi, error, error_len) ||
        !validate_required_object(root, "", "audio", &audio, error, error_len) ||
        !validate_required_object(root, "", "runtime", &runtime, error, error_len)) {
        return false;
    }

    if (!validate_allowed_keys(bt, "bluetooth", bluetooth_keys, error, error_len) ||
        !validate_required_string(bt, "bluetooth", "device", SYSTEM_CONFIG_BT_DEVICE_LEN, false, NULL, error, error_len) ||
        !validate_required_string(bt, "bluetooth", "target_mac", 18, false, valid_mac_string, error, error_len) ||
        !validate_required_u32(bt, "bluetooth", "discovery_timeout_sec", 1, 120, NULL, error, error_len) ||
        !validate_required_u32(bt, "bluetooth", "connecting_stuck_ms", 1000, 300000, NULL, error, error_len)) {
        return false;
    }

    if (!validate_allowed_keys(storage, "storage", storage_keys, error, error_len) ||
        !validate_required_string(storage,
                                  "storage",
                                  "sd_mount_point",
                                  SYSTEM_CONFIG_MOUNT_POINT_LEN,
                                  false,
                                  valid_sd_mount_point,
                                  error,
                                  error_len) ||
        !validate_required_string(storage,
                                  "storage",
                                  "mount_point",
                                  SYSTEM_CONFIG_MOUNT_POINT_LEN,
                                  false,
                                  valid_music_mount_point,
                                  error,
                                  error_len)) {
        return false;
    }

    if (!validate_allowed_keys(wifi, "wifi", wifi_keys, error, error_len) ||
        !validate_required_string(wifi, "wifi", "ssid", SYSTEM_CONFIG_WIFI_SSID_LEN, false, NULL, error, error_len) ||
        !validate_required_string(wifi,
                                  "wifi",
                                  "password",
                                  SYSTEM_CONFIG_WIFI_PASSWORD_LEN,
                                  true,
                                  valid_wifi_password,
                                  error,
                                  error_len) ||
        !validate_required_u32(wifi, "wifi", "channel", 1, 13, NULL, error, error_len) ||
        !validate_required_u32(wifi, "wifi", "max_connections", 1, 4, NULL, error, error_len)) {
        return false;
    }

    uint32_t low_watermark = 0;
    uint32_t high_watermark = 0;
    uint32_t mp3_read_min = 0;
    uint32_t mp3_read_max = 0;
    if (!validate_allowed_keys(audio, "audio", audio_keys, error, error_len) ||
        !validate_required_u32(audio, "audio", "default_volume", 0, 100, NULL, error, error_len) ||
        !validate_required_u32(audio, "audio", "volume_step", 1, 100, NULL, error, error_len) ||
        !validate_required_u32(audio, "audio", "stream_buffer_size", 4096, 131072, NULL, error, error_len) ||
        !validate_required_u32(audio, "audio", "stream_low_watermark_pct", 1, 99, &low_watermark, error, error_len) ||
        !validate_required_u32(audio, "audio", "stream_high_watermark_pct", 1, 99, &high_watermark, error, error_len) ||
        !validate_required_u32(audio, "audio", "mp3_read_min", 64, 65536, &mp3_read_min, error, error_len) ||
        !validate_required_u32(audio, "audio", "mp3_read_max", 64, 65536, &mp3_read_max, error, error_len)) {
        return false;
    }

    if (high_watermark <= low_watermark) {
        set_validation_error(error,
                             error_len,
                             "campo audio.stream_high_watermark_pct deve ser maior que audio.stream_low_watermark_pct");
        return false;
    }

    if (mp3_read_max < mp3_read_min) {
        set_validation_error(error, error_len, "campo audio.mp3_read_max deve ser maior ou igual a audio.mp3_read_min");
        return false;
    }

    if (!validate_allowed_keys(runtime, "runtime", runtime_keys, error, error_len) ||
        !validate_required_u32(runtime, "runtime", "auto_sleep_idle_ms", 1000, 3600000, NULL, error, error_len) ||
        !validate_required_u32(runtime,
                               "runtime",
                               "decode_stall_recovery_ms",
                               1000,
                               120000,
                               NULL,
                               error,
                               error_len)) {
        return false;
    }

    return true;
}

system_config_json_status_t system_config_validate_json(const char *json,
                                                        size_t json_len,
                                                        char *error,
                                                        size_t error_len)
{
    if (error && error_len > 0) {
        error[0] = '\0';
    }

    if (!json || json_len == 0 || json_len > SYSTEM_CONFIG_MAX_JSON_SIZE) {
        set_validation_error(error, error_len, "json de config ausente ou muito grande");
        return SYSTEM_CONFIG_JSON_MALFORMED;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &parse_end, false);
    if (!root) {
        set_validation_error(error, error_len, "formatacao JSON invalida");
        return SYSTEM_CONFIG_JSON_MALFORMED;
    }

    while (parse_end && parse_end < json + json_len && isspace((unsigned char)*parse_end)) {
        parse_end++;
    }

    if (!parse_end || parse_end != json + json_len) {
        cJSON_Delete(root);
        set_validation_error(error, error_len, "formatacao JSON invalida");
        return SYSTEM_CONFIG_JSON_MALFORMED;
    }

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_validation_error(error, error_len, "estrutura invalida: raiz deve ser objeto");
        return SYSTEM_CONFIG_JSON_INVALID;
    }

    if (!validate_config_tree(root, error, error_len)) {
        cJSON_Delete(root);
        return SYSTEM_CONFIG_JSON_INVALID;
    }

    cJSON_Delete(root);
    return SYSTEM_CONFIG_JSON_OK;
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
    if (!sd_mount_point || sd_mount_point[0] == '\0' || !file_name || file_name[0] == '\0' ||
        !path || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(path, path_len, "%s/%s/%s", sd_mount_point, SYSTEM_CONFIG_DIR_NAME, file_name);
    if (written < 0 || written >= (int)path_len) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t system_config_load(const char *sd_mount_point,
                             const system_config_defaults_t *defaults,
                             system_config_t *out_config,
                             const char *tag)
{
    if (!sd_mount_point || !defaults || !out_config) {
        return ESP_ERR_INVALID_ARG;
    }

    config_from_defaults(defaults, out_config);

    char path[SYSTEM_CONFIG_PATH_LEN];
    esp_err_t ret = system_config_build_path(sd_mount_point, SYSTEM_CONFIG_FILE_NAME, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    char *json = NULL;
    size_t json_len = 0;
    if (!read_file_to_string(path, &json, &json_len, tag)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    free(json);
    if (!root || !cJSON_IsObject(root) || !schema_is_valid_or_absent(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *bt = cJSON_GetObjectItemCaseSensitive(root, "bluetooth");
    const cJSON *storage = cJSON_GetObjectItemCaseSensitive(root, "storage");
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    const cJSON *runtime = cJSON_GetObjectItemCaseSensitive(root, "runtime");

    if (cJSON_IsObject(bt)) {
        apply_string_field(bt, "device", out_config->bt_device, sizeof(out_config->bt_device), false, NULL);
        apply_mac_field(bt, "target_mac", out_config->target_mac);
        apply_u32_field(bt, "discovery_timeout_sec", 1, 120, &out_config->discovery_timeout_sec);
        apply_u32_field(bt, "connecting_stuck_ms", 1000, 300000, &out_config->bt_connecting_stuck_ms);
    }

    if (cJSON_IsObject(storage)) {
        apply_string_field(storage,
                           "sd_mount_point",
                           out_config->sd_mount_point,
                           sizeof(out_config->sd_mount_point),
                           false,
                           valid_sd_mount_point);
        apply_string_field(storage,
                           "mount_point",
                           out_config->music_mount_point,
                           sizeof(out_config->music_mount_point),
                           false,
                           valid_music_mount_point);
    }

    if (cJSON_IsObject(wifi)) {
        apply_string_field(wifi, "ssid", out_config->wifi_ssid, sizeof(out_config->wifi_ssid), false, NULL);
        apply_string_field(wifi,
                           "password",
                           out_config->wifi_password,
                           sizeof(out_config->wifi_password),
                           true,
                           valid_wifi_password);
        apply_u8_field(wifi, "channel", 1, 13, &out_config->wifi_channel);
        apply_u8_field(wifi, "max_connections", 1, 4, &out_config->wifi_max_connections);
    }

    if (cJSON_IsObject(audio)) {
        apply_u8_field(audio, "default_volume", 0, 100, &out_config->default_volume);
        apply_u8_field(audio, "volume_step", 1, 100, &out_config->volume_step);
        apply_u32_field(audio, "stream_buffer_size", 4096, 131072, &out_config->stream_buffer_size);
        apply_u32_field(audio, "stream_low_watermark_pct", 1, 99, &out_config->stream_low_watermark_pct);
        apply_u32_field(audio, "stream_high_watermark_pct", 1, 99, &out_config->stream_high_watermark_pct);
        apply_u32_field(audio, "mp3_read_min", 64, 65536, &out_config->mp3_read_min);
        apply_u32_field(audio, "mp3_read_max", 64, 65536, &out_config->mp3_read_max);
    }

    if (cJSON_IsObject(runtime)) {
        apply_u32_field(runtime, "auto_sleep_idle_ms", 1000, 3600000, &out_config->auto_sleep_idle_ms);
        apply_u32_field(runtime, "decode_stall_recovery_ms", 1000, 120000, &out_config->decode_stall_recovery_ms);
    }

    cJSON_Delete(root);

    if (out_config->stream_high_watermark_pct <= out_config->stream_low_watermark_pct) {
        out_config->stream_low_watermark_pct = defaults->stream_low_watermark_pct;
        out_config->stream_high_watermark_pct = defaults->stream_high_watermark_pct;
    }

    if (out_config->mp3_read_min > out_config->mp3_read_max) {
        out_config->mp3_read_min = defaults->mp3_read_min;
        out_config->mp3_read_max = defaults->mp3_read_max;
    }

    ESP_LOGI(tag,
             "Config carregada: bt=%s sd=%s music=%s wifi=%s vol=%u%% stream=%lu",
             out_config->bt_device,
             out_config->sd_mount_point,
             out_config->music_mount_point,
             out_config->wifi_ssid,
             (unsigned)out_config->default_volume,
             (unsigned long)out_config->stream_buffer_size);

    return ESP_OK;
}

static esp_err_t ensure_system_dir(const char *sd_mount_point, const char *tag)
{
    char path[SYSTEM_CONFIG_PATH_LEN];
    esp_err_t ret = build_system_dir_path(sd_mount_point, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        ESP_LOGE(tag, "Caminho existe, mas nao e diretorio: %s", path);
        return ESP_FAIL;
    }

    int stat_errno = errno;
    if (stat_errno != ENOENT) {
        ESP_LOGE(tag, "Falha acessando %s: errno=%d", path, stat_errno);
        return ESP_FAIL;
    }

    if (mkdir(path, 0775) != 0) {
        int mkdir_errno = errno;
        if (mkdir_errno == EEXIST) {
            return ESP_OK;
        }
        ESP_LOGE(tag, "Falha criando %s: errno=%d", path, mkdir_errno);
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

    cJSON_AddStringToObject(root, "schema", SYSTEM_CONFIG_SCHEMA);
    cJSON_AddItemToObject(root, "bluetooth", bt);
    cJSON_AddStringToObject(bt, "device", defaults->bt_device);
    cJSON_AddStringToObject(bt, "target_mac", mac);
    cJSON_AddNumberToObject(bt, "discovery_timeout_sec", defaults->discovery_timeout_sec);
    cJSON_AddNumberToObject(bt, "connecting_stuck_ms", defaults->bt_connecting_stuck_ms);

    cJSON_AddItemToObject(root, "storage", storage);
    cJSON_AddStringToObject(storage, "sd_mount_point", defaults->sd_mount_point);
    cJSON_AddStringToObject(storage, "mount_point", defaults->music_mount_point);

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
    char validation_error[160];
    system_config_json_status_t validation = system_config_validate_json(json,
                                                                         json_len,
                                                                         validation_error,
                                                                         sizeof(validation_error));
    if (validation == SYSTEM_CONFIG_JSON_NO_MEM) {
        return ESP_ERR_NO_MEM;
    }
    if (validation != SYSTEM_CONFIG_JSON_OK) {
        ESP_LOGW(tag, "Config recusada: %s", validation_error);
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
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "ts", (double)now);
    cJSON_AddStringToObject(root, "tag", tag ? tag : "");
    cJSON_AddStringToObject(root, "erro", message);

    char *line = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!line) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    bool ok = fprintf(file, "%s\n", line) >= 0;
    cJSON_free(line);
    fclose(file);
    return ok ? ESP_OK : ESP_FAIL;
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

    bool config_exists = false;
    ret = regular_file_exists(config_path, &config_exists, tag);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!config_exists) {
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

    bool log_exists = false;
    ret = regular_file_exists(log_path, &log_exists, tag);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!log_exists) {
        FILE *file = fopen(log_path, "ab");
        if (!file) {
            ESP_LOGE(tag, "Falha criando %s: errno=%d", log_path, errno);
            return ESP_FAIL;
        }
        fclose(file);
    }

    return ESP_OK;
}
