#ifndef SYSTEM_CONFIG_MANAGER_H
#define SYSTEM_CONFIG_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SYSTEM_CONFIG_DIR_NAME ".system"
#define SYSTEM_CONFIG_FILE_NAME "config.json"
#define SYSTEM_ERROR_LOG_FILE_NAME "errors.log"

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

esp_err_t system_config_ensure(const char *sd_mount_point,
                               const system_config_defaults_t *defaults,
                               const char *tag);
esp_err_t system_config_build_path(const char *sd_mount_point,
                                   const char *file_name,
                                   char *path,
                                   size_t path_len);
esp_err_t system_config_replace_json(const char *sd_mount_point,
                                     const char *json,
                                     size_t json_len,
                                     const char *tag);
esp_err_t system_config_append_error(const char *sd_mount_point,
                                     const char *tag,
                                     const char *message);

#endif
