#ifndef PLAYER_TASKS_H
#define PLAYER_TASKS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_bt_defs.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

typedef struct {
    const char *tag;

    EventGroupHandle_t *player_event_group;
    EventBits_t bt_connected_bit;
    EventBits_t track_finished_bit;
    EventBits_t stream_ready_bit;

    bool *bt_connected;
    bool *bt_connecting;
    bool *audio_playing;
    bool *streaming_active;
    bool *playback_paused;

    volatile bool *restart_discovery_in_progress;
    bool *discovery_active;
    bool *discovery_stop_pending;
    bool *device_found;
    bool *connect_after_discovery_stop;

    int *current_track;
    int *mp3_count;
    int *connection_retries;
    int *file_errors;

    uint32_t *a2dp_open_fail_streak;

    TickType_t *bt_connecting_since;
    TickType_t *last_producer_tick;

    esp_bd_addr_t *target_device_addr;
    const esp_bd_addr_t *target_mac_addr;

    TimerHandle_t *connection_timer;

    uint32_t bt_connecting_stuck_ms;
    uint32_t auto_sleep_idle_ms;
    uint32_t a2dp_fail_rediscovery_threshold;
    uint32_t decode_stall_recovery_ms;

    bool (*bt_ready_for_playback)(bool bt_connected, bool streaming_active);
    int (*get_random_track)(int current_track, int total_tracks);
    int (*get_previous_track)(int current_track, int total_tracks);

    void (*start_current_track_playback)(void);
    void (*stop_playback_and_reset)(bool wait_task, const char *reason);
    esp_err_t (*bluetooth_search_and_connect)(void);
    void (*log_system_status)(void);
    void (*set_bt_connecting)(bool connecting);
    void (*enter_deep_sleep)(bool from_power_button);
    void (*log_bt_state)(const char *reason);
} player_tasks_ctx_t;

void player_tasks_bind(player_tasks_ctx_t *ctx);
void player_tasks_control_task(void *pvParameter);
void player_tasks_main_task(void *pvParameter);

#endif
