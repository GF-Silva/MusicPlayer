#ifndef APP_BOOTSTRAP_H
#define APP_BOOTSTRAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_bt_defs.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "a2dp_stream.h"
#include "app_facade.h"
#include "audio_pipeline.h"
#include "bt_callbacks.h"
#include "bt_manager.h"
#include "mp3dec.h"
#include "playback_engine.h"
#include "player_tasks.h"
#include "ring_buffer.h"
#include "sleep_manager.h"

typedef struct {
    const char *tag;

    EventGroupHandle_t *player_event_group;
    TimerHandle_t *connection_timer;
    TimerHandle_t *discovery_timer;
    TimerHandle_t *buffer_monitor_timer;

    uint32_t discovery_timeout_sec;
    uint32_t pwr_release_wait_ms;
    uint32_t bt_connecting_stuck_ms;
    uint32_t auto_sleep_idle_ms;
    uint32_t a2dp_open_fail_rediscovery_threshold;

    int control_queue_len;

    bool *sd_mounted;
    bool *bt_initialized;
    bool *bt_connected;
    bool *bt_connecting;
    bool *audio_playing;
    bool *codec_configured;
    bool *streaming_active;

    int *connection_retries;
    int *file_errors;

    uint8_t *current_volume;
    float *volume_scale;
    int *current_track;
    int *mp3_count;

    bool *device_found;
    bool *discovery_active;
    bool *connect_after_discovery_stop;
    bool *discovery_stop_pending;
    volatile bool *restart_discovery_in_progress;

    TickType_t *bt_connecting_since;
    uint32_t *a2dp_open_fail_streak;

    esp_bd_addr_t *target_device_addr;
    esp_bd_addr_t *target_mac_addr;

    FILE **current_file;
    mp3_info_t *current_mp3_info;
    TaskHandle_t *file_reader_task_handle;
    volatile bool *file_reader_task_running;

    HMP3Decoder *mp3_decoder;
    uint8_t **mp3_input_buffer;
    int16_t **pcm_output_buffer;
    int *bytes_left_in_mp3;
    uint8_t **read_ptr;
    ring_buffer_t **stream_buffer;

    uint32_t *total_bytes_streamed;
    uint32_t *underrun_count;
    uint32_t *callback_count;
    uint32_t *cb_lock_fail_count;
    uint32_t *cb_empty_count;
    uint32_t *cb_partial_count;
    uint32_t *producer_frame_count;
    uint32_t *producer_drop_count;
    bool *stream_stabilized;
    TickType_t *last_producer_tick;
    uint32_t *buffer_low_events;
    uint32_t *buffer_high_events;
    bool *playback_paused;

    uint32_t stream_buffer_size;
    uint32_t stream_low_bytes;
    uint32_t stream_high_bytes;
    uint32_t mp3_input_buffer_size;
    uint32_t pcm_output_buffer_size;
    uint32_t prebuffer_frames;
    uint32_t mp3_critical_bytes;
    uint32_t mp3_read_min;
    uint32_t mp3_read_max;
    uint32_t mp3_no_sync_drop_bytes;

    uint32_t power_pin;
    uint32_t board_led_gpio;
    bool board_led_active_high;

    const char *mount_point;
    uint32_t max_path_len;

    EventBits_t bt_connected_bit;
    EventBits_t track_finished_bit;
    EventBits_t stream_ready_bit;
    EventBits_t codec_ready_bit;

    sleep_manager_ctx_t *sleep_manager;
    app_facade_ctx_t *app_facade;
    bt_manager_t *bt_manager;
    audio_pipeline_ctx_t *audio_pipeline;
    bt_callbacks_ctx_t *bt_callbacks;
    playback_engine_ctx_t *playback_engine;
    a2dp_stream_ctx_t *a2dp_stream;
    player_tasks_ctx_t *player_tasks;
} app_bootstrap_ctx_t;

esp_err_t app_bootstrap_init(app_bootstrap_ctx_t *ctx);

#endif
