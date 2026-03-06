#ifndef BT_CALLBACKS_H
#define BT_CALLBACKS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt_defs.h"
#include "esp_gap_bt_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

typedef struct {
    const char *tag;

    bool *bt_connected;
    bool *bt_connecting;
    bool *audio_playing;
    bool *streaming_active;
    bool *playback_paused;

    bool *device_found;
    bool *discovery_active;
    bool *connect_after_discovery_stop;
    bool *discovery_stop_pending;

    esp_bd_addr_t *target_device_addr;
    const esp_bd_addr_t *target_mac_addr;

    EventGroupHandle_t *player_event_group;
    EventBits_t bt_connected_bit;
    EventBits_t stream_ready_bit;

    TimerHandle_t *connection_timer;
    TimerHandle_t *discovery_timer;

    uint32_t *a2dp_open_fail_streak;
    uint32_t a2dp_open_fail_rediscovery_threshold;
    uint32_t *total_bytes_streamed;
    uint32_t *callback_count;

    volatile bool *file_reader_task_running;
    FILE **current_file;

    void (*set_bt_connecting)(bool connecting);
    void (*log_bt_state)(const char *reason);
    void (*volume_up)(void);
    void (*volume_down)(void);
} bt_callbacks_ctx_t;

void bt_callbacks_bind(bt_callbacks_ctx_t *ctx);

void bt_callbacks_connection_timeout_cb(TimerHandle_t xTimer);
void bt_callbacks_discovery_timeout_cb(TimerHandle_t xTimer);
void bt_callbacks_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
void bt_callbacks_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
void bt_callbacks_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
void bt_callbacks_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

#endif
