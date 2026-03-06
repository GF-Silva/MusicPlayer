#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "mp3dec.h"
#include "ring_buffer.h"

typedef struct {
    const char *tag;

    uint8_t **mp3_input_buffer;
    int16_t **pcm_output_buffer;
    ring_buffer_t **stream_buffer;
    HMP3Decoder *mp3_decoder;

    FILE **current_file;
    TaskHandle_t *file_reader_task_handle;
    volatile bool *file_reader_task_running;

    int *bytes_left_in_mp3;
    uint8_t **read_ptr;

    bool *audio_playing;
    bool *playback_paused;
    bool *codec_configured;
    bool *streaming_active;

    EventGroupHandle_t *player_event_group;
    EventBits_t codec_ready_bit;
    EventBits_t track_finished_bit;

    uint32_t *buffer_low_events;
    uint32_t *buffer_high_events;
} audio_pipeline_ctx_t;

esp_err_t audio_pipeline_init_buffers(audio_pipeline_ctx_t *ctx,
                                      size_t mp3_input_size,
                                      size_t pcm_output_size,
                                      size_t stream_size);

void audio_pipeline_cleanup_buffers(audio_pipeline_ctx_t *ctx);

esp_err_t audio_pipeline_init_decoder(audio_pipeline_ctx_t *ctx);
void audio_pipeline_cleanup_decoder(audio_pipeline_ctx_t *ctx);

void audio_pipeline_stop_and_reset(audio_pipeline_ctx_t *ctx,
                                   bool wait_task,
                                   const char *reason);

void audio_pipeline_buffer_monitor_tick(audio_pipeline_ctx_t *ctx,
                                        size_t stream_size,
                                        size_t low_mark,
                                        size_t high_mark);

#endif
