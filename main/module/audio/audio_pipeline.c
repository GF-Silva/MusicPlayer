#include "audio_pipeline.h"

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static bool ctx_has_core(const audio_pipeline_ctx_t *ctx)
{
    return ctx && ctx->tag &&
           ctx->mp3_input_buffer && ctx->pcm_output_buffer && ctx->stream_buffer &&
           ctx->mp3_decoder &&
           ctx->current_file &&
           ctx->file_reader_task_handle && ctx->file_reader_task_running &&
           ctx->bytes_left_in_mp3 && ctx->read_ptr &&
           ctx->audio_playing && ctx->playback_paused && ctx->codec_configured;
}

esp_err_t audio_pipeline_init_buffers(audio_pipeline_ctx_t *ctx,
                                      size_t mp3_input_size,
                                      size_t pcm_output_size,
                                      size_t stream_size)
{
    if (!ctx_has_core(ctx)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(ctx->tag, "Alocando buffers de áudio...");

    *ctx->mp3_input_buffer = heap_caps_malloc(mp3_input_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!*ctx->mp3_input_buffer) {
        ESP_LOGE(ctx->tag, "Falha alocar MP3 input buffer");
        return ESP_ERR_NO_MEM;
    }

    *ctx->pcm_output_buffer = heap_caps_malloc(pcm_output_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!*ctx->pcm_output_buffer) {
        ESP_LOGE(ctx->tag, "Falha alocar PCM output buffer");
        free(*ctx->mp3_input_buffer);
        *ctx->mp3_input_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    *ctx->stream_buffer = ring_buffer_create(stream_size);
    if (!*ctx->stream_buffer) {
        ESP_LOGE(ctx->tag, "Falha criar stream buffer");
        free(*ctx->mp3_input_buffer);
        free(*ctx->pcm_output_buffer);
        *ctx->mp3_input_buffer = NULL;
        *ctx->pcm_output_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(ctx->tag, "Buffers: mp3=%u pcm=%u stream=%u",
             (unsigned)mp3_input_size,
             (unsigned)pcm_output_size,
             (unsigned)stream_size);
    return ESP_OK;
}

void audio_pipeline_cleanup_buffers(audio_pipeline_ctx_t *ctx)
{
    if (!ctx_has_core(ctx)) {
        return;
    }

    if (*ctx->stream_buffer) {
        ring_buffer_destroy(*ctx->stream_buffer);
        *ctx->stream_buffer = NULL;
    }

    if (*ctx->pcm_output_buffer) {
        free(*ctx->pcm_output_buffer);
        *ctx->pcm_output_buffer = NULL;
    }

    if (*ctx->mp3_input_buffer) {
        free(*ctx->mp3_input_buffer);
        *ctx->mp3_input_buffer = NULL;
    }
}

esp_err_t audio_pipeline_init_decoder(audio_pipeline_ctx_t *ctx)
{
    if (!ctx_has_core(ctx) || !ctx->player_event_group) {
        return ESP_ERR_INVALID_ARG;
    }

    if (*ctx->mp3_decoder) {
        MP3FreeDecoder(*ctx->mp3_decoder);
        *ctx->mp3_decoder = NULL;
    }

    *ctx->mp3_decoder = MP3InitDecoder();
    if (!*ctx->mp3_decoder) {
        ESP_LOGE(ctx->tag, "Falha ao criar decoder libhelix");
        return ESP_FAIL;
    }

    *ctx->codec_configured = true;
    xEventGroupSetBits(*ctx->player_event_group, ctx->codec_ready_bit);
    return ESP_OK;
}

void audio_pipeline_cleanup_decoder(audio_pipeline_ctx_t *ctx)
{
    if (!ctx_has_core(ctx)) {
        return;
    }

    if (*ctx->mp3_decoder) {
        MP3FreeDecoder(*ctx->mp3_decoder);
        *ctx->mp3_decoder = NULL;
    }
    *ctx->codec_configured = false;
}

void audio_pipeline_stop_and_reset(audio_pipeline_ctx_t *ctx,
                                   bool wait_task,
                                   const char *reason)
{
    if (!ctx_has_core(ctx) || !ctx->player_event_group) {
        return;
    }

    ESP_LOGI(ctx->tag, "Reset playback (%s)", reason ? reason : "-");

    *ctx->file_reader_task_running = false;

    if (wait_task && *ctx->file_reader_task_handle != NULL) {
        for (int i = 0; i < 100; i++) {
            if (*ctx->file_reader_task_handle == NULL) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (*ctx->current_file) {
        fclose(*ctx->current_file);
        *ctx->current_file = NULL;
    }

    audio_pipeline_cleanup_decoder(ctx);

    if (*ctx->stream_buffer) {
        ring_buffer_reset(*ctx->stream_buffer);
    }

    *ctx->bytes_left_in_mp3 = 0;
    *ctx->read_ptr = NULL;
    *ctx->file_reader_task_handle = NULL;
    *ctx->audio_playing = false;
    *ctx->playback_paused = false;

    xEventGroupClearBits(*ctx->player_event_group,
                         ctx->codec_ready_bit | ctx->track_finished_bit);
}

void audio_pipeline_buffer_monitor_tick(audio_pipeline_ctx_t *ctx,
                                        size_t stream_size,
                                        size_t low_mark,
                                        size_t high_mark)
{
    if (!ctx || !ctx->stream_buffer || !*ctx->stream_buffer || !ctx->streaming_active ||
        !ctx->buffer_low_events || !ctx->buffer_high_events) {
        return;
    }

    if (!*ctx->streaming_active) {
        return;
    }

    size_t available = ring_buffer_available(*ctx->stream_buffer);

    if (available < low_mark) {
        (*ctx->buffer_low_events)++;
    } else if (available > high_mark) {
        (*ctx->buffer_high_events)++;
    }

    (void)stream_size;
}
