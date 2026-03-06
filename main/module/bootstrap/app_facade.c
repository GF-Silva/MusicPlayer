#include "app_facade.h"

#include <string.h>

#include "app_log.h"
#include "esp_log.h"
#include "esp_system.h"
#include "player_utils.h"
#include "ring_buffer.h"

static app_facade_ctx_t *s_ctx;

void app_facade_bind(app_facade_ctx_t *ctx)
{
    s_ctx = ctx;
}

void app_facade_set_bt_connecting(bool connecting)
{
    if (!s_ctx || !s_ctx->bt_connecting || !s_ctx->bt_connecting_since) {
        return;
    }
    player_set_bt_connecting(s_ctx->bt_connecting, s_ctx->bt_connecting_since, connecting);
}

void app_facade_log_bt_state(const char *reason)
{
    if (!s_ctx || !s_ctx->tag || !s_ctx->bt_connected || !s_ctx->bt_connecting ||
        !s_ctx->streaming_active || !s_ctx->discovery_active || !s_ctx->discovery_stop_pending ||
        !s_ctx->device_found) {
        return;
    }

    ESP_LOGD(s_ctx->tag,
             "BT_STATE[%s] conn:%d connecting:%d stream:%d discovery:%d disc_stop_pending:%d found:%d",
             reason ? reason : "-",
             *s_ctx->bt_connected,
             *s_ctx->bt_connecting,
             *s_ctx->streaming_active,
             *s_ctx->discovery_active,
             *s_ctx->discovery_stop_pending,
             *s_ctx->device_found);
}

bool app_facade_ringbuf_decode_continue(void *ignored)
{
    (void)ignored;
    if (!s_ctx || !s_ctx->audio_pipeline || !s_ctx->audio_pipeline->file_reader_task_running || !s_ctx->bt_connected) {
        return false;
    }
    return *s_ctx->audio_pipeline->file_reader_task_running && *s_ctx->bt_connected;
}

esp_err_t app_facade_init_audio_buffers(size_t mp3_input_buffer_size,
                                        size_t pcm_output_buffer_size,
                                        size_t stream_buffer_size)
{
    if (!s_ctx || !s_ctx->audio_pipeline) {
        return ESP_ERR_INVALID_STATE;
    }
    return audio_pipeline_init_buffers(s_ctx->audio_pipeline,
                                       mp3_input_buffer_size,
                                       pcm_output_buffer_size,
                                       stream_buffer_size);
}

void app_facade_cleanup_audio_buffers(void)
{
    if (!s_ctx || !s_ctx->audio_pipeline) {
        return;
    }
    audio_pipeline_cleanup_buffers(s_ctx->audio_pipeline);
}

esp_err_t app_facade_init_audio_decoder(void)
{
    if (!s_ctx || !s_ctx->audio_pipeline) {
        return ESP_ERR_INVALID_STATE;
    }
    return audio_pipeline_init_decoder(s_ctx->audio_pipeline);
}

void app_facade_cleanup_audio_decoder(void)
{
    if (!s_ctx || !s_ctx->audio_pipeline) {
        return;
    }
    audio_pipeline_cleanup_decoder(s_ctx->audio_pipeline);
}

void app_facade_free_playlist_cache(void)
{
}

void app_facade_log_system_status(void)
{
    if (!s_ctx || !s_ctx->tag || !s_ctx->sd_mounted || !s_ctx->bt_initialized || !s_ctx->bt_connected ||
        !s_ctx->codec_configured || !s_ctx->audio_playing || !s_ctx->streaming_active ||
        !s_ctx->mp3_count || !s_ctx->current_track || !s_ctx->current_volume ||
        !s_ctx->callback_count || !s_ctx->underrun_count || !s_ctx->cb_lock_fail_count ||
        !s_ctx->cb_empty_count || !s_ctx->cb_partial_count || !s_ctx->total_bytes_streamed ||
        !s_ctx->buffer_low_events || !s_ctx->buffer_high_events || !s_ctx->audio_pipeline || !s_ctx->audio_pipeline->stream_buffer) {
        return;
    }

    ESP_LOGI(s_ctx->tag, "");
    ESP_LOGI(s_ctx->tag, "📊 Status do Sistema:");
    ESP_LOGI(s_ctx->tag, "   SD Card: %s", *s_ctx->sd_mounted ? "✅" : "❌");
    ESP_LOGI(s_ctx->tag, "   Bluetooth: %s", *s_ctx->bt_initialized ? "✅" : "❌");
    ESP_LOGI(s_ctx->tag, "   BT Conectado: %s", *s_ctx->bt_connected ? "✅" : "❌");
    ESP_LOGI(s_ctx->tag, "   Decoder: %s", *s_ctx->codec_configured ? "✅" : "❌");
    ESP_LOGI(s_ctx->tag, "   Tocando: %s", *s_ctx->audio_playing ? "✅" : "❌");
    ESP_LOGI(s_ctx->tag, "   Stream ativo: %s", *s_ctx->streaming_active ? "✅" : "❌");
    ESP_LOGI(s_ctx->tag, "   MP3s: %d arquivos", *s_ctx->mp3_count);
    ESP_LOGI(s_ctx->tag, "   Track: [%d/%d]", *s_ctx->current_track + 1, *s_ctx->mp3_count);
    ESP_LOGI(s_ctx->tag, "   Volume: %u%%", (unsigned)*s_ctx->current_volume);
    ESP_LOGI(s_ctx->tag, "   Heap livre: %lu bytes", (unsigned long)esp_get_free_heap_size());

    if (*s_ctx->audio_pipeline->stream_buffer) {
        size_t avail = ring_buffer_available(*s_ctx->audio_pipeline->stream_buffer);
        ESP_LOGI(s_ctx->tag, "   Buffer: %zu/%zu bytes", avail, s_ctx->stream_buffer_size);
    }

    ESP_LOGI(s_ctx->tag, "   Callbacks A2DP: %lu", (unsigned long)*s_ctx->callback_count);
    ESP_LOGI(s_ctx->tag, "   Underruns: %lu", (unsigned long)*s_ctx->underrun_count);
    ESP_LOGI(s_ctx->tag, "   CB lock/empty/partial: %lu/%lu/%lu",
             (unsigned long)*s_ctx->cb_lock_fail_count,
             (unsigned long)*s_ctx->cb_empty_count,
             (unsigned long)*s_ctx->cb_partial_count);
    ESP_LOGI(s_ctx->tag, "   Bytes streamed: %lu", (unsigned long)*s_ctx->total_bytes_streamed);
    ESP_LOGI(s_ctx->tag, "   Buffer evt low/high: %lu/%lu",
             (unsigned long)*s_ctx->buffer_low_events,
             (unsigned long)*s_ctx->buffer_high_events);

    app_facade_log_bt_state("status");
    ESP_LOGI(s_ctx->tag, "");
}

void app_facade_stop_playback_and_reset(bool wait_task, const char *reason)
{
    if (!s_ctx || !s_ctx->audio_pipeline) {
        return;
    }
    audio_pipeline_stop_and_reset(s_ctx->audio_pipeline, wait_task, reason);
}

void app_facade_buffer_monitor_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!s_ctx || !s_ctx->audio_pipeline) {
        return;
    }
    audio_pipeline_buffer_monitor_tick(s_ctx->audio_pipeline,
                                       s_ctx->stream_buffer_size,
                                       s_ctx->stream_low_bytes,
                                       s_ctx->stream_high_bytes);
}

void app_facade_input_on_power_hold(bool from_power_button)
{
    if (!s_ctx || !s_ctx->sleep_manager) {
        return;
    }
    sleep_manager_enter_deep_sleep(s_ctx->sleep_manager, from_power_button);
}

esp_err_t app_facade_bluetooth_init(void)
{
    if (!s_ctx || !s_ctx->bt_manager || !s_ctx->tag || !s_ctx->bt_initialized || !s_ctx->bt_connected ||
        !s_ctx->bt_connecting || !s_ctx->device_found || !s_ctx->discovery_active ||
        !s_ctx->connect_after_discovery_stop || !s_ctx->discovery_stop_pending ||
        !s_ctx->target_device_addr || !s_ctx->target_mac_addr) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_ctx->bt_manager, 0, sizeof(*s_ctx->bt_manager));
    s_ctx->bt_manager->tag = s_ctx->tag;
    s_ctx->bt_manager->bt_initialized = s_ctx->bt_initialized;
    s_ctx->bt_manager->bt_connected = s_ctx->bt_connected;
    s_ctx->bt_manager->bt_connecting = s_ctx->bt_connecting;
    s_ctx->bt_manager->device_found = s_ctx->device_found;
    s_ctx->bt_manager->discovery_active = s_ctx->discovery_active;
    s_ctx->bt_manager->connect_after_discovery_stop = s_ctx->connect_after_discovery_stop;
    s_ctx->bt_manager->discovery_stop_pending = s_ctx->discovery_stop_pending;
    s_ctx->bt_manager->target_device_addr = s_ctx->target_device_addr;
    s_ctx->bt_manager->target_mac_addr = s_ctx->target_mac_addr;
    s_ctx->bt_manager->discovery_timer = s_ctx->discovery_timer;
    s_ctx->bt_manager->set_bt_connecting = app_facade_set_bt_connecting;
    s_ctx->bt_manager->log_bt_state = app_facade_log_bt_state;
    s_ctx->bt_manager->gap_callback = s_ctx->gap_callback;
    s_ctx->bt_manager->a2dp_callback = s_ctx->a2dp_callback;
    s_ctx->bt_manager->avrc_ct_callback = s_ctx->avrc_ct_callback;
    s_ctx->bt_manager->avrc_tg_callback = s_ctx->avrc_tg_callback;
    s_ctx->bt_manager->source_data_callback = s_ctx->source_data_callback;

    esp_err_t ret = bt_manager_init(s_ctx->bt_manager);
    if (ret == ESP_OK) {
        ESP_LOGI(s_ctx->tag, "✅ Bluetooth inicializado");
    }
    return ret;
}

esp_err_t app_facade_bluetooth_search_and_connect(void)
{
    if (!s_ctx || !s_ctx->bt_manager) {
        return ESP_ERR_INVALID_STATE;
    }
    return bt_manager_search_and_connect(s_ctx->bt_manager);
}
