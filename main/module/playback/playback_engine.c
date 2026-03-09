#include "playback_engine.h"

#include <string.h>

#include "app_log.h"
#include "esp_log.h"

static playback_engine_ctx_t *s_ctx;

void playback_engine_bind(playback_engine_ctx_t *ctx)
{
    s_ctx = ctx;
}

static bool ready(void)
{
    return s_ctx && s_ctx->tag && s_ctx->mount_point && s_ctx->stream_buffer && *s_ctx->stream_buffer &&
           s_ctx->current_track && s_ctx->mp3_count && s_ctx->current_file && s_ctx->current_mp3_info &&
           s_ctx->file_reader_task_handle && s_ctx->file_reader_task_running &&
           s_ctx->mp3_input_buffer && *s_ctx->mp3_input_buffer && s_ctx->pcm_output_buffer && *s_ctx->pcm_output_buffer &&
           s_ctx->bytes_left_in_mp3 && s_ctx->read_ptr && s_ctx->volume_scale &&
           s_ctx->playback_paused && s_ctx->bt_connected && s_ctx->streaming_active && s_ctx->audio_playing &&
           s_ctx->file_errors &&
           s_ctx->total_bytes_streamed && s_ctx->underrun_count && s_ctx->callback_count &&
           s_ctx->cb_lock_fail_count && s_ctx->cb_empty_count && s_ctx->cb_partial_count &&
           s_ctx->producer_frame_count && s_ctx->producer_drop_count && s_ctx->stream_stabilized &&
           s_ctx->last_producer_tick &&
           s_ctx->player_event_group && *s_ctx->player_event_group &&
           s_ctx->ringbuf_decode_continue &&
           s_ctx->media_get_mp3_path && s_ctx->media_analyze_mp3_file && s_ctx->media_skip_id3v2 &&
           s_ctx->media_drop_trailing_tag_if_present &&
           s_ctx->init_audio_decoder && s_ctx->cleanup_audio_decoder && s_ctx->stop_playback_and_reset;
}

void playback_engine_decode_task(void *pvParameter)
{
    (void)pvParameter;
    if (!ready()) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(s_ctx->tag, "🎵 Task de decodificação iniciada");
    ESP_LOGI(s_ctx->tag, "   Heap livre: %lu bytes", (unsigned long)esp_get_free_heap_size());

    char filepath[s_ctx->max_path_len];
    bool reached_eof = false;
    const char *exit_reason = "loop_exit";

    if (s_ctx->media_get_mp3_path(s_ctx->mount_point, *s_ctx->current_track, filepath, sizeof(filepath)) != ESP_OK) {
        goto task_fail;
    }

    ESP_LOGI(s_ctx->tag,
             "▶️ Reproduzindo [%d/%d]: %s",
             *s_ctx->current_track + 1,
             *s_ctx->mp3_count,
             filepath);

    if (s_ctx->media_analyze_mp3_file(filepath, s_ctx->current_mp3_info, s_ctx->tag) != ESP_OK) {
        goto task_fail;
    }

    *s_ctx->current_file = fopen(filepath, "rb");
    if (!*s_ctx->current_file) {
        goto task_fail;
    }

    s_ctx->media_skip_id3v2(*s_ctx->current_file, s_ctx->tag);
    ring_buffer_reset(*s_ctx->stream_buffer);

    if (s_ctx->init_audio_decoder() != ESP_OK) {
        goto task_fail;
    }

    *s_ctx->bytes_left_in_mp3 = 0;
    *s_ctx->read_ptr = *s_ctx->mp3_input_buffer;

    ESP_LOGI(s_ctx->tag, "🔄 Pré-buffer (%d frames)...", s_ctx->prebuffer_frames);

    int frame_count = 0;
    bool first_frame = true;
    TickType_t last_stream_progress_tick = xTaskGetTickCount();
    uint32_t last_stream_bytes = *s_ctx->total_bytes_streamed;
    uint32_t consecutive_short_writes = 0;
    *s_ctx->last_producer_tick = xTaskGetTickCount();
    uint16_t pre_eof_no_sync_loops = 0;
    uint16_t pre_eof_decode_err_loops = 0;
    uint16_t pre_eof_underflow_loops = 0;
    uint16_t eof_no_sync_loops = 0;
    uint16_t eof_decode_err_loops = 0;
    uint16_t eof_underflow_loops = 0;

    while (frame_count < s_ctx->prebuffer_frames && *s_ctx->file_reader_task_running) {
        if (!*s_ctx->bt_connected) {
            exit_reason = "bt_disconnected_prebuffer";
            break;
        }

        if (*s_ctx->bytes_left_in_mp3 <= s_ctx->mp3_critical_bytes) {
            if (*s_ctx->bytes_left_in_mp3 > 0 && *s_ctx->read_ptr != *s_ctx->mp3_input_buffer) {
                memmove(*s_ctx->mp3_input_buffer, *s_ctx->read_ptr, (size_t)*s_ctx->bytes_left_in_mp3);
            }
            *s_ctx->read_ptr = *s_ctx->mp3_input_buffer;

            size_t to_read = (size_t)s_ctx->mp3_input_buffer_size - (size_t)*s_ctx->bytes_left_in_mp3;
            size_t r = fread(*s_ctx->mp3_input_buffer + *s_ctx->bytes_left_in_mp3, 1, to_read, *s_ctx->current_file);

            if (r > 0) {
                *s_ctx->bytes_left_in_mp3 += (int)r;
            } else if (feof(*s_ctx->current_file) && *s_ctx->bytes_left_in_mp3 == 0) {
                reached_eof = true;
                exit_reason = "eof_prebuffer_empty";
                break;
            }
        }

        int offset = MP3FindSyncWord(*s_ctx->read_ptr, *s_ctx->bytes_left_in_mp3);
        if (offset < 0) {
            if (feof(*s_ctx->current_file) &&
                s_ctx->media_drop_trailing_tag_if_present(s_ctx->read_ptr, s_ctx->bytes_left_in_mp3, s_ctx->tag)) {
                reached_eof = true;
                exit_reason = "eof_prebuffer_trailing_tag";
                break;
            }

            if (feof(*s_ctx->current_file)) {
                pre_eof_no_sync_loops++;
            } else {
                pre_eof_no_sync_loops = 0;
            }

            if (!feof(*s_ctx->current_file) && *s_ctx->bytes_left_in_mp3 >= s_ctx->mp3_critical_bytes) {
                int drop = *s_ctx->bytes_left_in_mp3 - s_ctx->mp3_critical_bytes;
                if (drop <= 0) {
                    drop = s_ctx->mp3_no_sync_drop_bytes / 4;
                    if (drop < 64) {
                        drop = 64;
                    }
                }
                if (drop > s_ctx->mp3_no_sync_drop_bytes) {
                    drop = s_ctx->mp3_no_sync_drop_bytes;
                }
                if (drop > *s_ctx->bytes_left_in_mp3) {
                    drop = *s_ctx->bytes_left_in_mp3;
                }
                if (drop > 0) {
                    *s_ctx->read_ptr += drop;
                    *s_ctx->bytes_left_in_mp3 -= drop;
                    vTaskDelay(1);
                    continue;
                }
            }

            if (feof(*s_ctx->current_file) &&
                (*s_ctx->bytes_left_in_mp3 < 4 || pre_eof_no_sync_loops > 30)) {
                reached_eof = true;
                exit_reason = "eof_prebuffer_no_sync";
                break;
            }

            if (feof(*s_ctx->current_file) && *s_ctx->bytes_left_in_mp3 == 0) {
                reached_eof = true;
                exit_reason = "eof_prebuffer_no_sync";
                break;
            }
            vTaskDelay(1);
            continue;
        }
        pre_eof_no_sync_loops = 0;

        *s_ctx->read_ptr += offset;
        *s_ctx->bytes_left_in_mp3 -= offset;

        int err = MP3Decode(*s_ctx->mp3_decoder,
                            s_ctx->read_ptr,
                            s_ctx->bytes_left_in_mp3,
                            *s_ctx->pcm_output_buffer,
                            0);

        if (err == ERR_MP3_NONE) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(*s_ctx->mp3_decoder, &fi);

            if (first_frame) {
                ESP_LOGI(s_ctx->tag, "🎧 %d Hz | %d kbps | %d ch", fi.samprate, fi.bitrate / 1000, fi.nChans);
                first_frame = false;
                xEventGroupSetBits(*s_ctx->player_event_group, s_ctx->stream_ready_bit);
            }

            ring_buffer_write_blocking(*s_ctx->stream_buffer,
                                       (uint8_t *)*s_ctx->pcm_output_buffer,
                                       (size_t)fi.outputSamps * sizeof(int16_t),
                                       pdMS_TO_TICKS(40),
                                       s_ctx->ringbuf_decode_continue,
                                       NULL);

            frame_count++;
        } else {
            if ((err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) &&
                feof(*s_ctx->current_file)) {
                pre_eof_underflow_loops++;
                if (s_ctx->media_drop_trailing_tag_if_present(s_ctx->read_ptr,
                                                              s_ctx->bytes_left_in_mp3,
                                                              s_ctx->tag)) {
                    reached_eof = true;
                    exit_reason = "eof_prebuffer_underflow_tag";
                    break;
                }
                if (*s_ctx->bytes_left_in_mp3 < 4 || pre_eof_underflow_loops > 8) {
                    reached_eof = true;
                    exit_reason = "eof_prebuffer_underflow";
                    break;
                }
                vTaskDelay(1);
                continue;
            } else {
                pre_eof_underflow_loops = 0;
            }

            if (feof(*s_ctx->current_file)) {
                pre_eof_decode_err_loops++;
                if (s_ctx->media_drop_trailing_tag_if_present(s_ctx->read_ptr,
                                                              s_ctx->bytes_left_in_mp3,
                                                              s_ctx->tag)) {
                    reached_eof = true;
                    exit_reason = "eof_prebuffer_decodeerr_tag";
                    break;
                }
            } else {
                pre_eof_decode_err_loops = 0;
            }

            if (feof(*s_ctx->current_file) &&
                (*s_ctx->bytes_left_in_mp3 < s_ctx->mp3_critical_bytes || pre_eof_decode_err_loops > 30)) {
                reached_eof = true;
                exit_reason = "eof_prebuffer_decodeerr";
                break;
            }

            if (!feof(*s_ctx->current_file) && *s_ctx->bytes_left_in_mp3 > 0) {
                int drop = s_ctx->mp3_no_sync_drop_bytes / 4;
                if (drop < 64) {
                    drop = 64;
                }
                if (drop > *s_ctx->bytes_left_in_mp3) {
                    drop = *s_ctx->bytes_left_in_mp3;
                }
                *s_ctx->read_ptr += drop;
                *s_ctx->bytes_left_in_mp3 -= drop;
            }

            vTaskDelay(1);
            continue;
        }
    }

    ESP_LOGI(s_ctx->tag, "✅ Pré-buffer pronto (%d frames)", frame_count);

    while (*s_ctx->file_reader_task_running) {
        if (*s_ctx->playback_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!*s_ctx->bt_connected) {
            exit_reason = "bt_disconnected_main";
            break;
        }
        if (!*s_ctx->streaming_active && (frame_count % 120 == 0)) {
            ESP_LOGW(s_ctx->tag, "Stream flag OFF com BT conectado; mantendo decode para não secar buffer");
            if (s_ctx->log_bt_state) {
                s_ctx->log_bt_state("stream_flag_off_keep_decoding");
            }
        }

        size_t stream_available = ring_buffer_available(*s_ctx->stream_buffer);
        uint32_t stream_fill_percent = (uint32_t)((stream_available * 100U) / (size_t)s_ctx->stream_buffer_size);

        if (*s_ctx->bytes_left_in_mp3 <= s_ctx->mp3_critical_bytes) {
            if (*s_ctx->bytes_left_in_mp3 > 0 && *s_ctx->read_ptr != *s_ctx->mp3_input_buffer) {
                memmove(*s_ctx->mp3_input_buffer, *s_ctx->read_ptr, (size_t)*s_ctx->bytes_left_in_mp3);
            }
            *s_ctx->read_ptr = *s_ctx->mp3_input_buffer;

            size_t free = (size_t)s_ctx->mp3_input_buffer_size - (size_t)*s_ctx->bytes_left_in_mp3;
            size_t to_read;

            if (stream_fill_percent < 30U) {
                to_read = (size_t)s_ctx->mp3_read_max;
                if (to_read > free) to_read = free;
            } else if (stream_fill_percent < 45U) {
                to_read = (size_t)s_ctx->mp3_read_max / 2U;
                if (to_read > free) to_read = free;
            } else {
                to_read = (size_t)s_ctx->mp3_read_min;
                if (to_read > free) to_read = free;
            }

            to_read &= ~0x03U;

            size_t r = fread(*s_ctx->mp3_input_buffer + *s_ctx->bytes_left_in_mp3,
                             1,
                             to_read,
                             *s_ctx->current_file);

            if (r > 0) {
                *s_ctx->bytes_left_in_mp3 += (int)r;
            } else if (feof(*s_ctx->current_file) && *s_ctx->bytes_left_in_mp3 == 0) {
                ESP_LOGI(s_ctx->tag, "🏁 Fim do arquivo MP3");
                reached_eof = true;
                exit_reason = "eof_main_empty";
                break;
            }
        }

        int offset = MP3FindSyncWord(*s_ctx->read_ptr, *s_ctx->bytes_left_in_mp3);
        if (offset < 0) {
            if (feof(*s_ctx->current_file) &&
                s_ctx->media_drop_trailing_tag_if_present(s_ctx->read_ptr, s_ctx->bytes_left_in_mp3, s_ctx->tag)) {
                reached_eof = true;
                exit_reason = "eof_main_trailing_tag";
                break;
            }

            if (feof(*s_ctx->current_file)) {
                eof_no_sync_loops++;
            } else {
                eof_no_sync_loops = 0;
            }

            if (!feof(*s_ctx->current_file) && *s_ctx->bytes_left_in_mp3 >= s_ctx->mp3_critical_bytes) {
                int drop = *s_ctx->bytes_left_in_mp3 - s_ctx->mp3_critical_bytes;
                if (drop <= 0) {
                    /* Evita lock em bytes_left == mp3_critical_bytes quando não há sync. */
                    drop = s_ctx->mp3_no_sync_drop_bytes / 4;
                    if (drop < 64) {
                        drop = 64;
                    }
                }
                if (drop > s_ctx->mp3_no_sync_drop_bytes) {
                    drop = s_ctx->mp3_no_sync_drop_bytes;
                }
                if (drop > *s_ctx->bytes_left_in_mp3) {
                    drop = *s_ctx->bytes_left_in_mp3;
                }
                if (drop > 0) {
                    *s_ctx->read_ptr += drop;
                    *s_ctx->bytes_left_in_mp3 -= drop;
                    vTaskDelay(1);
                    continue;
                }
            }

            if (feof(*s_ctx->current_file) && (*s_ctx->bytes_left_in_mp3 < 4 || eof_no_sync_loops > 30)) {
                ESP_LOGI(s_ctx->tag,
                         "🏁 Fim do stream (EOF sem sync, loops=%u, bytes=%d)",
                         (unsigned)eof_no_sync_loops,
                         *s_ctx->bytes_left_in_mp3);
                reached_eof = true;
                exit_reason = "eof_main_no_sync";
                break;
            }
            vTaskDelay(1);
            continue;
        }
        eof_no_sync_loops = 0;

        *s_ctx->read_ptr += offset;
        *s_ctx->bytes_left_in_mp3 -= offset;

        int err = MP3Decode(*s_ctx->mp3_decoder,
                            s_ctx->read_ptr,
                            s_ctx->bytes_left_in_mp3,
                            *s_ctx->pcm_output_buffer,
                            0);

        if (err != ERR_MP3_NONE) {
            if ((err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) &&
                feof(*s_ctx->current_file)) {
                eof_underflow_loops++;
                if (s_ctx->media_drop_trailing_tag_if_present(s_ctx->read_ptr,
                                                              s_ctx->bytes_left_in_mp3,
                                                              s_ctx->tag)) {
                    reached_eof = true;
                    exit_reason = "eof_main_underflow_tag";
                    break;
                }
                if (*s_ctx->bytes_left_in_mp3 < 4 || eof_underflow_loops > 8) {
                    ESP_LOGI(s_ctx->tag,
                             "🏁 Fim do stream (underflow+EOF, loops=%u, bytes=%d)",
                             (unsigned)eof_underflow_loops,
                             *s_ctx->bytes_left_in_mp3);
                    reached_eof = true;
                    exit_reason = "eof_main_underflow";
                    break;
                }
                vTaskDelay(1);
                continue;
            } else {
                eof_underflow_loops = 0;
            }

            if (feof(*s_ctx->current_file)) {
                eof_decode_err_loops++;
                if (s_ctx->media_drop_trailing_tag_if_present(s_ctx->read_ptr,
                                                              s_ctx->bytes_left_in_mp3,
                                                              s_ctx->tag)) {
                    reached_eof = true;
                    exit_reason = "eof_main_decodeerr_tag";
                    break;
                }
            } else {
                eof_decode_err_loops = 0;
            }

            if (feof(*s_ctx->current_file) &&
                (*s_ctx->bytes_left_in_mp3 < s_ctx->mp3_critical_bytes || eof_decode_err_loops > 30)) {
                ESP_LOGI(s_ctx->tag,
                         "🏁 Fim do stream (decode+EOF, loops=%u, bytes=%d)",
                         (unsigned)eof_decode_err_loops,
                         *s_ctx->bytes_left_in_mp3);
                reached_eof = true;
                exit_reason = "eof_main_decodeerr";
                break;
            }
            vTaskDelay(1);
            continue;
        }
        eof_decode_err_loops = 0;
        eof_underflow_loops = 0;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(*s_ctx->mp3_decoder, &fi);

        for (int i = 0; i < fi.outputSamps; i++) {
            int32_t s = (int32_t)((float)(*s_ctx->pcm_output_buffer)[i] * *s_ctx->volume_scale);
            (*s_ctx->pcm_output_buffer)[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }

        size_t frame_bytes = (size_t)fi.outputSamps * sizeof(int16_t);
        size_t pushed = ring_buffer_write_blocking(*s_ctx->stream_buffer,
                                                   (uint8_t *)*s_ctx->pcm_output_buffer,
                                                   frame_bytes,
                                                   pdMS_TO_TICKS(30),
                                                   s_ctx->ringbuf_decode_continue,
                                                   NULL);

        (*s_ctx->producer_frame_count)++;
        if (pushed == 0) {
            (*s_ctx->producer_drop_count)++;
            consecutive_short_writes++;
            if ((consecutive_short_writes % 50U) == 0U) {
                size_t stream_av = ring_buffer_available(*s_ctx->stream_buffer);
                ESP_LOGW(s_ctx->tag,
                         "Producer sem push (%lu seguidos). buf:%zu/%d mp3buf:%d bt:%d stream:%d",
                         (unsigned long)consecutive_short_writes,
                         stream_av,
                         s_ctx->stream_buffer_size,
                         *s_ctx->bytes_left_in_mp3,
                         *s_ctx->bt_connected,
                         *s_ctx->streaming_active);
            }
        } else {
            consecutive_short_writes = 0;
            *s_ctx->last_producer_tick = xTaskGetTickCount();
        }

        frame_count++;

        if (*s_ctx->total_bytes_streamed != last_stream_bytes) {
            last_stream_bytes = *s_ctx->total_bytes_streamed;
            last_stream_progress_tick = xTaskGetTickCount();
        } else {
            TickType_t stalled_for = xTaskGetTickCount() - last_stream_progress_tick;
            if (stalled_for > pdMS_TO_TICKS(3000) && (frame_count % 100 == 0)) {
                size_t stream_av = ring_buffer_available(*s_ctx->stream_buffer);
                ESP_LOGW(s_ctx->tag,
                         "Possível stall de stream: %lu ms sem progresso A2DP | buf:%zu/%d | mp3buf:%d | bt:%d stream:%d",
                         (unsigned long)(stalled_for * portTICK_PERIOD_MS),
                         stream_av,
                         s_ctx->stream_buffer_size,
                         *s_ctx->bytes_left_in_mp3,
                         *s_ctx->bt_connected,
                         *s_ctx->streaming_active);
                if (s_ctx->log_bt_state) {
                    s_ctx->log_bt_state("decode_stall");
                }
            }
        }

        if (frame_count % 500 == 0) {
#if APP_LOG_PROFILE >= LOG_PROFILE_VERBOSE
            size_t stream_av = ring_buffer_available(*s_ctx->stream_buffer);
            PERF_LOGI(s_ctx->tag,
                      "F%04d | Stream:%zu/%d (%.0f%%) | MP3buf:%d | Heap:%lu | Prod frame/drop:%lu/%lu",
                      frame_count,
                      stream_av,
                      s_ctx->stream_buffer_size,
                      (double)((stream_av * 100U) / (size_t)s_ctx->stream_buffer_size),
                      *s_ctx->bytes_left_in_mp3,
                      (unsigned long)esp_get_free_heap_size(),
                      (unsigned long)*s_ctx->producer_frame_count,
                      (unsigned long)*s_ctx->producer_drop_count);
#endif
        }

        taskYIELD();

        size_t current_available = ring_buffer_available(*s_ctx->stream_buffer);
        uint32_t current_fill = (uint32_t)((current_available * 100U) / (size_t)s_ctx->stream_buffer_size);

        if (current_fill > 70U) {
            vTaskDelay(pdMS_TO_TICKS(2));
        } else if (current_fill > 50U) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        /* Defensive EOF close-out: if EOF was already hit by fread and decode
         * drained all pending bytes, finalize immediately. */
        if (feof(*s_ctx->current_file) && *s_ctx->bytes_left_in_mp3 == 0) {
            reached_eof = true;
            exit_reason = "eof_main_drained";
            break;
        }
    }

task_fail:
    bool file_at_eof = (*s_ctx->current_file != NULL) ? feof(*s_ctx->current_file) : false;
    ESP_LOGI(s_ctx->tag,
             "⏹ Decoder encerrado | reason=%s eof=%d file_eof=%d bytes_left=%d bt=%d running=%d",
             exit_reason,
             reached_eof,
             file_at_eof,
             *s_ctx->bytes_left_in_mp3,
             *s_ctx->bt_connected,
             *s_ctx->file_reader_task_running);
    bool natural_finish = reached_eof;

    if (!natural_finish && file_at_eof && *s_ctx->bytes_left_in_mp3 == 0 && *s_ctx->bt_connected) {
        ESP_LOGW(s_ctx->tag, "EOF confirmado no encerramento; promovendo para fim natural");
        natural_finish = true;
    }

    if (*s_ctx->current_file) {
        fclose(*s_ctx->current_file);
        *s_ctx->current_file = NULL;
    }

    s_ctx->cleanup_audio_decoder();

    *s_ctx->bytes_left_in_mp3 = 0;
    *s_ctx->read_ptr = NULL;

    *s_ctx->file_reader_task_running = false;
    *s_ctx->file_reader_task_handle = NULL;
    *s_ctx->audio_playing = false;

    if (natural_finish) {
        xEventGroupSetBits(*s_ctx->player_event_group, s_ctx->track_finished_bit);
    }

    vTaskDelete(NULL);
}

void playback_engine_start_current_track(void)
{
    if (!ready()) {
        return;
    }

    s_ctx->stop_playback_and_reset(true, "start_track");

    *s_ctx->total_bytes_streamed = 0;
    *s_ctx->underrun_count = 0;
    *s_ctx->callback_count = 0;
    *s_ctx->cb_lock_fail_count = 0;
    *s_ctx->cb_empty_count = 0;
    *s_ctx->cb_partial_count = 0;
    *s_ctx->producer_frame_count = 0;
    *s_ctx->producer_drop_count = 0;
    *s_ctx->stream_stabilized = false;

    vTaskDelay(pdMS_TO_TICKS(120));

    *s_ctx->file_reader_task_running = true;
    *s_ctx->playback_paused = false;
    if (xTaskCreatePinnedToCore(playback_engine_decode_task,
                                "decode",
                                6144,
                                NULL,
                                6,
                                s_ctx->file_reader_task_handle,
                                1) == pdPASS) {
        ESP_LOGI(s_ctx->tag, "✅ Task decode criada (prio 6)");
        *s_ctx->audio_playing = true;
    } else {
        ESP_LOGE(s_ctx->tag, "❌ Falha criar task");
        (*s_ctx->file_errors)++;
        *s_ctx->file_reader_task_running = false;
    }
}
