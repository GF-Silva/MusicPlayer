#ifndef PLAYBACK_ENGINE_H
#define PLAYBACK_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "media_library.h"
#include "mp3dec.h"
#include "ring_buffer.h"

typedef struct {
    const char *tag;
    const char *mount_point;

    int max_path_len;
    int prebuffer_frames;

    int mp3_input_buffer_size;
    int stream_buffer_size;
    int mp3_critical_bytes;
    int mp3_read_min;
    int mp3_read_max;
    int mp3_no_sync_drop_bytes;

    int *current_track;
    int *mp3_count;
    FILE **current_file;
    mp3_info_t *current_mp3_info;

    TaskHandle_t *file_reader_task_handle;
    volatile bool *file_reader_task_running;

    HMP3Decoder *mp3_decoder;
    uint8_t **mp3_input_buffer;
    int16_t **pcm_output_buffer;

    int *bytes_left_in_mp3;
    uint8_t **read_ptr;

    float *volume_scale;

    bool *playback_paused;
    bool *bt_connected;
    bool *streaming_active;
    bool *audio_playing;

    int *file_errors;

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

    EventGroupHandle_t *player_event_group;
    EventBits_t stream_ready_bit;
    EventBits_t track_finished_bit;

    ring_buffer_t **stream_buffer;

    bool (*ringbuf_decode_continue)(void *ctx);

    esp_err_t (*media_get_mp3_path)(const char *mount_point, int index, char *path, size_t max_len);
    esp_err_t (*media_analyze_mp3_file)(const char *path, mp3_info_t *info, const char *tag);
    int (*media_skip_id3v2)(FILE *f, const char *tag);
    bool (*media_drop_trailing_tag_if_present)(uint8_t **ptr, int *bytes_left, const char *tag);

    esp_err_t (*init_audio_decoder)(void);
    void (*cleanup_audio_decoder)(void);
    void (*stop_playback_and_reset)(bool wait_task, const char *reason);

    void (*log_bt_state)(const char *reason);
} playback_engine_ctx_t;

void playback_engine_bind(playback_engine_ctx_t *ctx);
void playback_engine_decode_task(void *pvParameter);
void playback_engine_start_current_track(void);

#endif
