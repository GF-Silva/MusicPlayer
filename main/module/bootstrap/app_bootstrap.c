#include "app_bootstrap.h"

#include <string.h>

#include "a2dp_stream.h"
#include "app_facade.h"
#include "audio_pipeline.h"
#include "bt_callbacks.h"
#include "control_queue.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_gap_bt_api.h"
#include "input_manager.h"
#include "media_library.h"
#include "player_utils.h"

esp_err_t app_bootstrap_init(app_bootstrap_ctx_t *ctx)
{
    if (!ctx || !ctx->player_event_group || !ctx->connection_timer || !ctx->discovery_timer ||
        !ctx->buffer_monitor_timer || !ctx->sleep_manager || !ctx->app_facade || !ctx->bt_manager ||
        !ctx->audio_pipeline || !ctx->bt_callbacks || !ctx->playback_engine || !ctx->a2dp_stream ||
        !ctx->player_tasks || !ctx->bt_connected || !ctx->streaming_active) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->sd_mounted && ctx->bt_initialized && ctx->bt_connected && ctx->bt_connecting &&
        ctx->audio_playing && ctx->codec_configured && ctx->streaming_active &&
        ctx->connection_retries && ctx->file_errors) {
        *ctx->sd_mounted = false;
        *ctx->bt_initialized = false;
        *ctx->bt_connected = false;
        *ctx->bt_connecting = false;
        *ctx->audio_playing = false;
        *ctx->codec_configured = false;
        *ctx->streaming_active = false;
        *ctx->connection_retries = 0;
        *ctx->file_errors = 0;
    }

    *ctx->player_event_group = xEventGroupCreate();
    esp_err_t qret = control_queue_init(ctx->control_queue_len);

    *ctx->connection_timer = xTimerCreate("conn", pdMS_TO_TICKS(60000),
                                          pdFALSE, NULL, bt_callbacks_connection_timeout_cb);
    *ctx->discovery_timer = xTimerCreate("disc", pdMS_TO_TICKS(ctx->discovery_timeout_sec * 1000),
                                         pdFALSE, NULL, bt_callbacks_discovery_timeout_cb);
    *ctx->buffer_monitor_timer = xTimerCreate("buf_mon", pdMS_TO_TICKS(500),
                                              pdTRUE, NULL, app_facade_buffer_monitor_callback);

    *ctx->audio_pipeline = (audio_pipeline_ctx_t){
        .tag = ctx->tag,
        .mp3_input_buffer = ctx->mp3_input_buffer,
        .pcm_output_buffer = ctx->pcm_output_buffer,
        .stream_buffer = ctx->stream_buffer,
        .mp3_decoder = ctx->mp3_decoder,
        .current_file = ctx->current_file,
        .file_reader_task_handle = ctx->file_reader_task_handle,
        .file_reader_task_running = ctx->file_reader_task_running,
        .bytes_left_in_mp3 = ctx->bytes_left_in_mp3,
        .read_ptr = ctx->read_ptr,
        .audio_playing = ctx->audio_playing,
        .playback_paused = ctx->playback_paused,
        .codec_configured = ctx->codec_configured,
        .streaming_active = ctx->streaming_active,
        .player_event_group = ctx->player_event_group,
        .codec_ready_bit = ctx->codec_ready_bit,
        .track_finished_bit = ctx->track_finished_bit,
        .buffer_low_events = ctx->buffer_low_events,
        .buffer_high_events = ctx->buffer_high_events,
    };

    if (!*ctx->player_event_group || qret != ESP_OK || !*ctx->connection_timer ||
        !*ctx->discovery_timer || !*ctx->buffer_monitor_timer) {
        return ESP_FAIL;
    }

    *ctx->sleep_manager = (sleep_manager_ctx_t){
        .tag = ctx->tag,
        .wake_pin = (gpio_num_t)ctx->power_pin,
        .led_gpio = (gpio_num_t)ctx->board_led_gpio,
        .led_active_high = ctx->board_led_active_high,
        .release_wait_ms_from_button = ctx->pwr_release_wait_ms,
        .release_wait_ms_auto = 600,
        .connection_timer = *ctx->connection_timer,
        .discovery_timer = *ctx->discovery_timer,
        .buffer_monitor_timer = *ctx->buffer_monitor_timer,
        .stop_playback_and_reset = app_facade_stop_playback_and_reset,
        .free_playlist_cache = app_facade_free_playlist_cache,
    };

    *ctx->app_facade = (app_facade_ctx_t){
        .tag = ctx->tag,
        .sd_mounted = ctx->sd_mounted,
        .bt_initialized = ctx->bt_initialized,
        .bt_connected = ctx->bt_connected,
        .bt_connecting = ctx->bt_connecting,
        .audio_playing = ctx->audio_playing,
        .codec_configured = ctx->codec_configured,
        .streaming_active = ctx->streaming_active,
        .device_found = ctx->device_found,
        .discovery_active = ctx->discovery_active,
        .connect_after_discovery_stop = ctx->connect_after_discovery_stop,
        .discovery_stop_pending = ctx->discovery_stop_pending,
        .mp3_count = ctx->mp3_count,
        .current_track = ctx->current_track,
        .current_volume = ctx->current_volume,
        .target_device_addr = ctx->target_device_addr,
        .target_mac_addr = ctx->target_mac_addr,
        .bt_connecting_since = ctx->bt_connecting_since,
        .total_bytes_streamed = ctx->total_bytes_streamed,
        .underrun_count = ctx->underrun_count,
        .callback_count = ctx->callback_count,
        .cb_lock_fail_count = ctx->cb_lock_fail_count,
        .cb_empty_count = ctx->cb_empty_count,
        .cb_partial_count = ctx->cb_partial_count,
        .buffer_low_events = ctx->buffer_low_events,
        .buffer_high_events = ctx->buffer_high_events,
        .player_event_group = ctx->player_event_group,
        .discovery_timer = *ctx->discovery_timer,
        .bt_manager = ctx->bt_manager,
        .sleep_manager = ctx->sleep_manager,
        .audio_pipeline = ctx->audio_pipeline,
        .gap_callback = bt_callbacks_gap_cb,
        .a2dp_callback = bt_callbacks_a2dp_cb,
        .avrc_ct_callback = bt_callbacks_avrc_ct_cb,
        .avrc_tg_callback = bt_callbacks_avrc_tg_cb,
        .source_data_callback = a2dp_stream_source_data_cb,
        .stream_buffer_size = ctx->stream_buffer_size,
        .stream_low_bytes = ctx->stream_low_bytes,
        .stream_high_bytes = ctx->stream_high_bytes,
    };
    app_facade_bind(ctx->app_facade);

    *ctx->bt_callbacks = (bt_callbacks_ctx_t){
        .tag = ctx->tag,
        .bt_connected = ctx->bt_connected,
        .bt_connecting = ctx->bt_connecting,
        .audio_playing = ctx->audio_playing,
        .streaming_active = ctx->streaming_active,
        .playback_paused = ctx->playback_paused,
        .device_found = ctx->device_found,
        .discovery_active = ctx->discovery_active,
        .connect_after_discovery_stop = ctx->connect_after_discovery_stop,
        .discovery_stop_pending = ctx->discovery_stop_pending,
        .target_device_addr = ctx->target_device_addr,
        .target_mac_addr = ctx->target_mac_addr,
        .player_event_group = ctx->player_event_group,
        .bt_connected_bit = ctx->bt_connected_bit,
        .stream_ready_bit = ctx->stream_ready_bit,
        .connection_timer = ctx->connection_timer,
        .discovery_timer = ctx->discovery_timer,
        .a2dp_open_fail_streak = ctx->a2dp_open_fail_streak,
        .a2dp_open_fail_rediscovery_threshold = ctx->a2dp_open_fail_rediscovery_threshold,
        .total_bytes_streamed = ctx->total_bytes_streamed,
        .callback_count = ctx->callback_count,
        .file_reader_task_running = ctx->file_reader_task_running,
        .current_file = ctx->current_file,
        .set_bt_connecting = app_facade_set_bt_connecting,
        .log_bt_state = app_facade_log_bt_state,
        .volume_up = input_manager_volume_up,
        .volume_down = input_manager_volume_down,
    };
    bt_callbacks_bind(ctx->bt_callbacks);

    *ctx->playback_engine = (playback_engine_ctx_t){
        .tag = ctx->tag,
        .mount_point = ctx->mount_point,
        .max_path_len = ctx->max_path_len,
        .prebuffer_frames = ctx->prebuffer_frames,
        .mp3_input_buffer_size = ctx->mp3_input_buffer_size,
        .stream_buffer_size = ctx->stream_buffer_size,
        .mp3_critical_bytes = ctx->mp3_critical_bytes,
        .mp3_read_min = ctx->mp3_read_min,
        .mp3_read_max = ctx->mp3_read_max,
        .mp3_no_sync_drop_bytes = ctx->mp3_no_sync_drop_bytes,
        .current_track = ctx->current_track,
        .mp3_count = ctx->mp3_count,
        .current_file = ctx->current_file,
        .current_mp3_info = ctx->current_mp3_info,
        .file_reader_task_handle = ctx->file_reader_task_handle,
        .file_reader_task_running = ctx->file_reader_task_running,
        .mp3_decoder = ctx->mp3_decoder,
        .mp3_input_buffer = ctx->mp3_input_buffer,
        .pcm_output_buffer = ctx->pcm_output_buffer,
        .bytes_left_in_mp3 = ctx->bytes_left_in_mp3,
        .read_ptr = ctx->read_ptr,
        .volume_scale = ctx->volume_scale,
        .playback_paused = ctx->playback_paused,
        .bt_connected = ctx->bt_connected,
        .streaming_active = ctx->streaming_active,
        .audio_playing = ctx->audio_playing,
        .file_errors = ctx->file_errors,
        .total_bytes_streamed = ctx->total_bytes_streamed,
        .underrun_count = ctx->underrun_count,
        .callback_count = ctx->callback_count,
        .cb_lock_fail_count = ctx->cb_lock_fail_count,
        .cb_empty_count = ctx->cb_empty_count,
        .cb_partial_count = ctx->cb_partial_count,
        .producer_frame_count = ctx->producer_frame_count,
        .producer_drop_count = ctx->producer_drop_count,
        .stream_stabilized = ctx->stream_stabilized,
        .last_producer_tick = ctx->last_producer_tick,
        .player_event_group = ctx->player_event_group,
        .stream_ready_bit = ctx->stream_ready_bit,
        .track_finished_bit = ctx->track_finished_bit,
        .stream_buffer = ctx->stream_buffer,
        .ringbuf_decode_continue = app_facade_ringbuf_decode_continue,
        .media_get_mp3_path = media_get_mp3_path,
        .media_analyze_mp3_file = media_analyze_mp3_file,
        .media_skip_id3v2 = media_skip_id3v2,
        .media_drop_trailing_tag_if_present = media_drop_trailing_tag_if_present,
        .init_audio_decoder = app_facade_init_audio_decoder,
        .cleanup_audio_decoder = app_facade_cleanup_audio_decoder,
        .stop_playback_and_reset = app_facade_stop_playback_and_reset,
        .log_bt_state = app_facade_log_bt_state,
    };
    playback_engine_bind(ctx->playback_engine);

    *ctx->a2dp_stream = (a2dp_stream_ctx_t){
        .tag = ctx->tag,
        .stream_buffer = ctx->stream_buffer,
        .bt_connected = ctx->bt_connected,
        .playback_paused = ctx->playback_paused,
        .stream_stabilized = ctx->stream_stabilized,
        .total_bytes_streamed = ctx->total_bytes_streamed,
        .underrun_count = ctx->underrun_count,
        .callback_count = ctx->callback_count,
        .cb_lock_fail_count = ctx->cb_lock_fail_count,
        .cb_empty_count = ctx->cb_empty_count,
        .cb_partial_count = ctx->cb_partial_count,
    };
    a2dp_stream_bind(ctx->a2dp_stream);

    *ctx->player_tasks = (player_tasks_ctx_t){
        .tag = ctx->tag,
        .player_event_group = ctx->player_event_group,
        .bt_connected_bit = ctx->bt_connected_bit,
        .track_finished_bit = ctx->track_finished_bit,
        .stream_ready_bit = ctx->stream_ready_bit,
        .bt_connected = ctx->bt_connected,
        .bt_connecting = ctx->bt_connecting,
        .audio_playing = ctx->audio_playing,
        .streaming_active = ctx->streaming_active,
        .playback_paused = ctx->playback_paused,
        .restart_discovery_in_progress = ctx->restart_discovery_in_progress,
        .discovery_active = ctx->discovery_active,
        .discovery_stop_pending = ctx->discovery_stop_pending,
        .device_found = ctx->device_found,
        .connect_after_discovery_stop = ctx->connect_after_discovery_stop,
        .current_track = ctx->current_track,
        .mp3_count = ctx->mp3_count,
        .connection_retries = ctx->connection_retries,
        .file_errors = ctx->file_errors,
        .a2dp_open_fail_streak = ctx->a2dp_open_fail_streak,
        .bt_connecting_since = ctx->bt_connecting_since,
        .last_producer_tick = ctx->last_producer_tick,
        .target_device_addr = ctx->target_device_addr,
        .target_mac_addr = ctx->target_mac_addr,
        .connection_timer = ctx->connection_timer,
        .bt_connecting_stuck_ms = ctx->bt_connecting_stuck_ms,
        .auto_sleep_idle_ms = ctx->auto_sleep_idle_ms,
        .a2dp_fail_rediscovery_threshold = ctx->a2dp_open_fail_rediscovery_threshold,
        .decode_stall_recovery_ms = ctx->decode_stall_recovery_ms,
        .bt_ready_for_playback = player_bt_ready_for_playback,
        .get_random_track = player_get_random_track,
        .get_previous_track = player_get_previous_track,
        .start_current_track_playback = playback_engine_start_current_track,
        .stop_playback_and_reset = app_facade_stop_playback_and_reset,
        .bluetooth_search_and_connect = app_facade_bluetooth_search_and_connect,
        .log_system_status = app_facade_log_system_status,
        .set_bt_connecting = app_facade_set_bt_connecting,
        .enter_deep_sleep = app_facade_input_on_power_hold,
        .log_bt_state = app_facade_log_bt_state,
    };
    player_tasks_bind(ctx->player_tasks);

    return ESP_OK;
}
