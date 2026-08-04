#ifndef SYSTEM_CONFIG_MANAGER_H
#define SYSTEM_CONFIG_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SYSTEM_CONFIG_DIR_NAME ".system"
#define SYSTEM_CONFIG_FILE_NAME "config.json"
#define SYSTEM_ERROR_LOG_FILE_NAME "errors.log"
#define SYSTEM_CONFIG_SCHEMA "musicplayer.config.v1"

#define SYSTEM_CONFIG_BT_DEVICE_LEN 64
#define SYSTEM_CONFIG_MOUNT_POINT_LEN 96
#define SYSTEM_CONFIG_WIFI_SSID_LEN 33
#define SYSTEM_CONFIG_WIFI_PASSWORD_LEN 65

typedef struct {
    const char *bt_device;
    const char *sd_mount_point;
    const char *music_mount_point;
    const char *wifi_ssid;
    const char *wifi_password;
    uint8_t wifi_channel;
    uint8_t wifi_max_connections;
    uint8_t default_volume;
    uint8_t volume_step;
    uint32_t auto_sleep_idle_ms;
    uint32_t discovery_timeout_sec;
    uint32_t bt_connecting_stuck_ms;
    uint32_t decode_stall_recovery_ms;
    uint32_t stream_buffer_size;
    uint32_t stream_low_watermark_pct;
    uint32_t stream_high_watermark_pct;
    uint32_t mp3_read_min;
    uint32_t mp3_read_max;
    uint8_t target_mac[6];
} system_config_defaults_t;

typedef struct {
    char bt_device[SYSTEM_CONFIG_BT_DEVICE_LEN];
    char sd_mount_point[SYSTEM_CONFIG_MOUNT_POINT_LEN];
    char music_mount_point[SYSTEM_CONFIG_MOUNT_POINT_LEN];
    char wifi_ssid[SYSTEM_CONFIG_WIFI_SSID_LEN];
    char wifi_password[SYSTEM_CONFIG_WIFI_PASSWORD_LEN];
    uint8_t wifi_channel;
    uint8_t wifi_max_connections;
    uint8_t default_volume;
    uint8_t volume_step;
    uint32_t auto_sleep_idle_ms;
    uint32_t discovery_timeout_sec;
    uint32_t bt_connecting_stuck_ms;
    uint32_t decode_stall_recovery_ms;
    uint32_t stream_buffer_size;
    uint32_t stream_low_watermark_pct;
    uint32_t stream_high_watermark_pct;
    uint32_t mp3_read_min;
    uint32_t mp3_read_max;
    uint8_t target_mac[6];
} system_config_t;

typedef enum {
    SYSTEM_CONFIG_JSON_OK = 0,
    SYSTEM_CONFIG_JSON_MALFORMED,
    SYSTEM_CONFIG_JSON_INVALID,
    SYSTEM_CONFIG_JSON_NO_MEM,
} system_config_json_status_t;

esp_err_t system_config_ensure(const char *sd_mount_point,
                               const system_config_defaults_t *defaults,
                               const char *tag);
esp_err_t system_config_load(const char *sd_mount_point,
                             const system_config_defaults_t *defaults,
                             system_config_t *out_config,
                             const char *tag);
esp_err_t system_config_build_path(const char *sd_mount_point,
                                   const char *file_name,
                                   char *path,
                                   size_t path_len);
system_config_json_status_t system_config_validate_json(const char *json,
                                                        size_t json_len,
                                                        char *error,
                                                        size_t error_len);
esp_err_t system_config_replace_json(const char *sd_mount_point,
                                     const char *json,
                                     size_t json_len,
                                     const char *tag);
esp_err_t system_config_append_error(const char *sd_mount_point,
                                     const char *tag,
                                     const char *message);

#endif
