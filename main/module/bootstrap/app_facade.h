#ifndef APP_FACADE_H
#define APP_FACADE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_bt_defs.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "audio_pipeline.h"
#include "bt_manager.h"
#include "sleep_manager.h"

typedef struct {
    const char *tag;

    bool *sd_mounted;
    bool *bt_initialized;
    bool *bt_connected;
    bool *bt_connecting;
    bool *audio_playing;
    bool *codec_configured;
    bool *streaming_active;

    bool *device_found;
    bool *discovery_active;
    bool *connect_after_discovery_stop;
    bool *discovery_stop_pending;

    int *mp3_count;
    int *current_track;
    uint8_t *current_volume;

    esp_bd_addr_t *target_device_addr;
    esp_bd_addr_t *target_mac_addr;

    TickType_t *bt_connecting_since;

    uint32_t *total_bytes_streamed;
    uint32_t *underrun_count;
    uint32_t *callback_count;
    uint32_t *cb_lock_fail_count;
    uint32_t *cb_empty_count;
    uint32_t *cb_partial_count;
    uint32_t *buffer_low_events;
    uint32_t *buffer_high_events;

    EventGroupHandle_t *player_event_group;

    TimerHandle_t discovery_timer;

    bt_manager_t *bt_manager;
    sleep_manager_ctx_t *sleep_manager;
    audio_pipeline_ctx_t *audio_pipeline;

    esp_bt_gap_cb_t gap_callback;
    esp_a2d_cb_t a2dp_callback;
    esp_avrc_ct_cb_t avrc_ct_callback;
    esp_avrc_tg_cb_t avrc_tg_callback;
    esp_a2d_source_data_cb_t source_data_callback;

    size_t stream_buffer_size;
    uint32_t stream_low_bytes;
    uint32_t stream_high_bytes;

} app_facade_ctx_t;

void app_facade_bind(app_facade_ctx_t *ctx);

void app_facade_set_bt_connecting(bool connecting);
void app_facade_log_bt_state(const char *reason);
bool app_facade_ringbuf_decode_continue(void *ignored);

esp_err_t app_facade_init_audio_buffers(size_t mp3_input_buffer_size,
                                        size_t pcm_output_buffer_size,
                                        size_t stream_buffer_size);
void app_facade_cleanup_audio_buffers(void);
esp_err_t app_facade_init_audio_decoder(void);
void app_facade_cleanup_audio_decoder(void);

void app_facade_free_playlist_cache(void);
void app_facade_log_system_status(void);
void app_facade_stop_playback_and_reset(bool wait_task, const char *reason);
void app_facade_buffer_monitor_callback(TimerHandle_t xTimer);
void app_facade_input_on_power_hold(bool from_power_button);

esp_err_t app_facade_bluetooth_init(void);
esp_err_t app_facade_bluetooth_search_and_connect(void);

#endif
