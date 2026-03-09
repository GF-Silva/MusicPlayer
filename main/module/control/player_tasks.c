#include "player_tasks.h"

#include <limits.h>
#include <string.h>

#include "app_log.h"
#include "control_queue.h"
#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_system.h"

static player_tasks_ctx_t *s_ctx;

#define TICKS_TO_MS_LOCAL(t) ((uint32_t)(t) * (uint32_t)portTICK_PERIOD_MS)

void player_tasks_bind(player_tasks_ctx_t *ctx)
{
    s_ctx = ctx;
}

static bool ready(void)
{
    return s_ctx && s_ctx->tag && s_ctx->player_event_group && *s_ctx->player_event_group &&
           s_ctx->bt_connected && s_ctx->bt_connecting && s_ctx->audio_playing && s_ctx->streaming_active && s_ctx->playback_paused &&
           s_ctx->restart_discovery_in_progress && s_ctx->discovery_active && s_ctx->discovery_stop_pending &&
           s_ctx->device_found && s_ctx->connect_after_discovery_stop &&
           s_ctx->current_track && s_ctx->mp3_count && s_ctx->connection_retries && s_ctx->file_errors &&
           s_ctx->a2dp_open_fail_streak && s_ctx->bt_connecting_since &&
           s_ctx->last_producer_tick &&
           s_ctx->target_device_addr && s_ctx->target_mac_addr && s_ctx->connection_timer &&
           s_ctx->bt_ready_for_playback && s_ctx->get_random_track && s_ctx->get_previous_track &&
           s_ctx->start_current_track_playback && s_ctx->stop_playback_and_reset &&
           s_ctx->bluetooth_search_and_connect && s_ctx->log_system_status &&
           s_ctx->set_bt_connecting && s_ctx->enter_deep_sleep && s_ctx->log_bt_state;
}

void player_tasks_control_task(void *pvParameter)
{
    (void)pvParameter;
    if (!ready()) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(s_ctx->tag, "Task controle iniciada");

    player_cmd_t cmd;
    TickType_t last_play_next_time = 0;
    const TickType_t play_next_debounce_ms = 3000;
    bool play_next_from_track_finish_pending = false;

    while (1) {
        if (control_dequeue(&cmd, pdMS_TO_TICKS(1000))) {
            ESP_LOGD(s_ctx->tag, "Queue -> %s", control_cmd_to_str(cmd));
            control_log_state(s_ctx->tag, "after_dequeue");

            switch (cmd) {
                case CMD_PLAY_NEXT:
                case CMD_PLAY_PREV:
                case CMD_RETRY_CONNECTION:
                case CMD_CONNECT_TARGET:
                    control_mark_handled(cmd);
                    break;
                case CMD_RESTART_DISCOVERY:
                    *s_ctx->restart_discovery_in_progress = false;
                    control_mark_handled(cmd);
                    break;
                default:
                    break;
            }

            switch (cmd) {
                case CMD_PLAY_NEXT: {
                    if (!s_ctx->bt_ready_for_playback(*s_ctx->bt_connected, *s_ctx->streaming_active)) {
                        ESP_LOGW(s_ctx->tag, "⚠️ PLAY_NEXT ignorado (BT/stream não pronto)");
                        play_next_from_track_finish_pending = false;
                        break;
                    }

                    TickType_t now = xTaskGetTickCount();
                    bool bypass_debounce = play_next_from_track_finish_pending;
                    play_next_from_track_finish_pending = false;

                    if (!bypass_debounce &&
                        last_play_next_time != 0 &&
                        (now - last_play_next_time) < pdMS_TO_TICKS(play_next_debounce_ms)) {
                        ESP_LOGW(s_ctx->tag,
                                 "⚠️ PLAY_NEXT ignorado (debounce: %lu ms)",
                                 (unsigned long)TICKS_TO_MS_LOCAL(now - last_play_next_time));
                        break;
                    }
                    last_play_next_time = now;

                    int old_track = *s_ctx->current_track;
                    *s_ctx->current_track = s_ctx->get_random_track(*s_ctx->current_track, *s_ctx->mp3_count);
                    ESP_LOGI(s_ctx->tag,
                             "🎲 PLAY_NEXT aleatório: [%d/%d] → [%d/%d]",
                             old_track + 1,
                             *s_ctx->mp3_count,
                             *s_ctx->current_track + 1,
                             *s_ctx->mp3_count);

                    ESP_LOGI(s_ctx->tag,
                             "🎵 Comando: PLAY_NEXT para track [%d/%d]",
                             *s_ctx->current_track + 1,
                             *s_ctx->mp3_count);
                    s_ctx->start_current_track_playback();
                    break;
                }

                case CMD_PLAY_PREV: {
                    if (!s_ctx->bt_ready_for_playback(*s_ctx->bt_connected, *s_ctx->streaming_active)) {
                        ESP_LOGW(s_ctx->tag, "⚠️ PLAY_PREV ignorado (BT/stream não pronto)");
                        break;
                    }

                    TickType_t now = xTaskGetTickCount();
                    if (last_play_next_time != 0 &&
                        (now - last_play_next_time) < pdMS_TO_TICKS(800)) {
                        ESP_LOGW(s_ctx->tag,
                                 "⚠️ PLAY_PREV ignorado (debounce: %lu ms)",
                                 (unsigned long)TICKS_TO_MS_LOCAL(now - last_play_next_time));
                        break;
                    }
                    last_play_next_time = now;

                    int old_track = *s_ctx->current_track;
                    *s_ctx->current_track = s_ctx->get_previous_track(*s_ctx->current_track, *s_ctx->mp3_count);
                    ESP_LOGI(s_ctx->tag,
                             "⏮️ PLAY_PREV: [%d/%d] → [%d/%d]",
                             old_track + 1,
                             *s_ctx->mp3_count,
                             *s_ctx->current_track + 1,
                             *s_ctx->mp3_count);
                    s_ctx->start_current_track_playback();
                    break;
                }

                case CMD_STOP:
                    ESP_LOGI(s_ctx->tag, "⏹️ Comando: STOP");
                    s_ctx->stop_playback_and_reset(true, "cmd_stop");
                    break;

                case CMD_PAUSE:
                    ESP_LOGI(s_ctx->tag, "⏸️ Comando: PAUSE");
                    *s_ctx->playback_paused = true;
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
                    break;

                case CMD_RESUME:
                    ESP_LOGI(s_ctx->tag, "▶️ Comando: RESUME");
                    *s_ctx->playback_paused = false;
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                    break;

                case CMD_RETRY_CONNECTION:
                    if (*s_ctx->bt_connected || *s_ctx->bt_connecting) {
                        ESP_LOGI(s_ctx->tag, "RETRY_CONNECTION ignorado: já conectado/conectando");
                        break;
                    }
                    ESP_LOGI(s_ctx->tag, "🔄 Comando: RETRY_CONNECTION");

                    if (*s_ctx->a2dp_open_fail_streak >= s_ctx->a2dp_fail_rediscovery_threshold) {
                        ESP_LOGW(s_ctx->tag,
                                 "RETRY_CONNECTION -> RESTART_DISCOVERY (fail streak=%lu)",
                                 (unsigned long)*s_ctx->a2dp_open_fail_streak);
                        control_enqueue(CMD_RESTART_DISCOVERY, s_ctx->tag);
                        break;
                    }

                    {
                        uint32_t retry_delay_ms = 1200 + (*s_ctx->a2dp_open_fail_streak * 800);
                        if (retry_delay_ms > 4000) retry_delay_ms = 4000;
                        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
                    }

                    if (*s_ctx->bt_connected || *s_ctx->bt_connecting) {
                        break;
                    }
                    if (*s_ctx->connection_retries < INT_MAX) {
                        (*s_ctx->connection_retries)++;
                    }

                    memcpy(*s_ctx->target_device_addr, *s_ctx->target_mac_addr, ESP_BD_ADDR_LEN);
                    *s_ctx->device_found = true;
                    s_ctx->set_bt_connecting(true);
                    *s_ctx->connect_after_discovery_stop = false;
                    if (!control_enqueue(CMD_CONNECT_TARGET, s_ctx->tag)) {
                        s_ctx->set_bt_connecting(false);
                    }
                    break;

                case CMD_RESTART_DISCOVERY:
                    *s_ctx->restart_discovery_in_progress = true;
                    if (*s_ctx->bt_connected) {
                        ESP_LOGI(s_ctx->tag, "RESTART_DISCOVERY ignorado: já conectado");
                        *s_ctx->restart_discovery_in_progress = false;
                        break;
                    }
                    if (*s_ctx->bt_connecting && !*s_ctx->discovery_active) {
                        ESP_LOGW(s_ctx->tag, "RESTART_DISCOVERY: limpando connecting preso");
                        s_ctx->set_bt_connecting(false);
                    }
                    if (*s_ctx->bt_connecting && *s_ctx->discovery_active) {
                        ESP_LOGI(s_ctx->tag, "RESTART_DISCOVERY ignorado: discovery em andamento");
                        *s_ctx->restart_discovery_in_progress = false;
                        break;
                    }
                    ESP_LOGI(s_ctx->tag, "🔄 Comando: RESTART_DISCOVERY");

                    *s_ctx->device_found = false;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (s_ctx->bluetooth_search_and_connect() != ESP_OK) {
                        s_ctx->set_bt_connecting(false);
                    }
                    *s_ctx->restart_discovery_in_progress = false;
                    break;

                case CMD_CONNECT_TARGET: {
                    if (!*s_ctx->device_found || *s_ctx->bt_connected || !*s_ctx->bt_connecting) {
                        ESP_LOGW(s_ctx->tag,
                                 "CONNECT_TARGET ignorado | found:%d connected:%d connecting:%d",
                                 *s_ctx->device_found,
                                 *s_ctx->bt_connected,
                                 *s_ctx->bt_connecting);
                        break;
                    }
                    if (*s_ctx->discovery_active) {
                        *s_ctx->connect_after_discovery_stop = true;
                        if (!*s_ctx->discovery_stop_pending) {
                            *s_ctx->discovery_stop_pending = true;
                            esp_bt_gap_cancel_discovery();
                            ESP_LOGI(s_ctx->tag, "🛑 Cancelando discovery para conectar alvo");
                        }
                        break;
                    }

                    ESP_LOGI(s_ctx->tag, "🔗 Conectando ao A2DP...");
                    if (*s_ctx->connection_timer) {
                        xTimerStop(*s_ctx->connection_timer, 0);
                    }
                    s_ctx->set_bt_connecting(true);
                    esp_err_t conn_err = esp_a2d_source_connect(*s_ctx->target_device_addr);
                    if (conn_err != ESP_OK) {
                        ESP_LOGE(s_ctx->tag, "❌ Falha connect: %s", esp_err_to_name(conn_err));
                        *s_ctx->device_found = false;
                        s_ctx->set_bt_connecting(false);
                    } else {
                        ESP_LOGI(s_ctx->tag, "✅ Comando connect enviado");
                        if (*s_ctx->connection_timer) {
                            xTimerStart(*s_ctx->connection_timer, 0);
                        }
                        s_ctx->log_bt_state("connect_cmd_sent");
                    }
                    break;
                }

                default:
                    break;
            }
        }

        EventBits_t bits = xEventGroupGetBits(*s_ctx->player_event_group);

        if (bits & s_ctx->track_finished_bit) {
            if (!s_ctx->bt_ready_for_playback(*s_ctx->bt_connected, *s_ctx->streaming_active)) {
                ESP_LOGW(s_ctx->tag, "Track finalizada, aguardando BT pronto para avançar");
                continue;
            }

            ESP_LOGI(s_ctx->tag, "✅ Track finalizado, preparando próxima...");
            xEventGroupClearBits(*s_ctx->player_event_group, s_ctx->track_finished_bit);

            int cleared = (int)control_pending_count();
            control_flush();
            if (cleared > 0) {
                ESP_LOGI(s_ctx->tag, "🗑️ Removidos %d comandos duplicados da queue", cleared);
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
            if (control_enqueue(CMD_PLAY_NEXT, s_ctx->tag)) {
                play_next_from_track_finish_pending = true;
            } else {
                ESP_LOGW(s_ctx->tag, "⚠️ Falha ao enfileirar PLAY_NEXT após fim da track");
            }
        }

        if (*s_ctx->file_errors > 10) {
            ESP_LOGE(s_ctx->tag, "Muitos erros, reiniciando...");
            esp_restart();
        }
    }
}

void player_tasks_main_task(void *pvParameter)
{
    (void)pvParameter;
    if (!ready()) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(s_ctx->tag, "Task principal iniciada");
    ESP_LOGI(s_ctx->tag, "Aguardando conexão Bluetooth...");

    EventBits_t bits = xEventGroupWaitBits(*s_ctx->player_event_group,
                                           s_ctx->bt_connected_bit,
                                           false,
                                           false,
                                           pdMS_TO_TICKS(60000));
    if (!(bits & s_ctx->bt_connected_bit)) {
        ESP_LOGW(s_ctx->tag, "Sem conexão inicial em 60s, mantendo retries contínuos...");
        control_enqueue(CMD_RETRY_CONNECTION, s_ctx->tag);
        ESP_LOGI(s_ctx->tag, "Sistema iniciado sem BT (modo reconexão contínua).");
    } else {
        ESP_LOGI(s_ctx->tag, "Bluetooth conectado! Sistema pronto.");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    s_ctx->log_system_status();

    xEventGroupWaitBits(*s_ctx->player_event_group,
                        s_ctx->stream_ready_bit,
                        false,
                        false,
                        pdMS_TO_TICKS(5000));

    TickType_t last_alive_log = xTaskGetTickCount();
    TickType_t idle_without_bt_start = 0;
    TickType_t last_reconnect_log = 0;
    TickType_t last_decode_stall_recovery = 0;

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if (xTaskGetTickCount() - last_alive_log > pdMS_TO_TICKS(120000)) {
            PERF_LOGI(s_ctx->tag,
                      "Sistema ativo - Track: [%d/%d]",
                      *s_ctx->current_track + 1,
                      *s_ctx->mp3_count);
            PERF_LOGI(s_ctx->tag,
                      "Stream: %s, Heap: %lu",
                      *s_ctx->streaming_active ? "ON" : "OFF",
                      (unsigned long)esp_get_free_heap_size());
            last_alive_log = xTaskGetTickCount();
        }

        if (s_ctx->decode_stall_recovery_ms > 0 &&
            *s_ctx->bt_connected &&
            *s_ctx->audio_playing &&
            *s_ctx->streaming_active &&
            !*s_ctx->playback_paused &&
            *s_ctx->last_producer_tick != 0) {
            TickType_t stalled_ticks = now - *s_ctx->last_producer_tick;
            if (stalled_ticks > pdMS_TO_TICKS(s_ctx->decode_stall_recovery_ms)) {
                if ((now - last_decode_stall_recovery) > pdMS_TO_TICKS(3000)) {
                    ESP_LOGE(s_ctx->tag,
                             "⚠️ Decode stall detectado (%lu ms sem produzir PCM). Forçando PLAY_NEXT",
                             (unsigned long)TICKS_TO_MS_LOCAL(stalled_ticks));
                    last_decode_stall_recovery = now;
                }

                if (!control_is_pending(CMD_PLAY_NEXT)) {
                    control_enqueue(CMD_PLAY_NEXT, s_ctx->tag);
                }

                /* Evita loop de recovery imediato enquanto CMD_PLAY_NEXT é processado. */
                *s_ctx->last_producer_tick = now;
            }
        }

        if (!*s_ctx->bt_connected && !*s_ctx->audio_playing) {
            if (idle_without_bt_start == 0) {
                idle_without_bt_start = xTaskGetTickCount();
            }
            if (!*s_ctx->bt_connecting &&
                !control_is_pending(CMD_RETRY_CONNECTION) &&
                !control_is_pending(CMD_RESTART_DISCOVERY) &&
                !control_is_pending(CMD_CONNECT_TARGET) &&
                !*s_ctx->restart_discovery_in_progress &&
                !*s_ctx->discovery_active &&
                !*s_ctx->discovery_stop_pending) {
                TickType_t now = xTaskGetTickCount();
                if ((now - last_reconnect_log) > pdMS_TO_TICKS(15000)) {
                    ESP_LOGW(s_ctx->tag, "Conexão perdida (sem música), tentando reconectar...");
                    last_reconnect_log = now;
                }
                control_enqueue(CMD_RETRY_CONNECTION, s_ctx->tag);
            }

            if (*s_ctx->bt_connecting && *s_ctx->bt_connecting_since != 0) {
                TickType_t connecting_ticks = xTaskGetTickCount() - *s_ctx->bt_connecting_since;
                if (connecting_ticks > pdMS_TO_TICKS(s_ctx->bt_connecting_stuck_ms)) {
                    ESP_LOGW(s_ctx->tag,
                             "⚠️ bt_connecting preso por %lu ms, reiniciando ciclo",
                             (unsigned long)TICKS_TO_MS_LOCAL(connecting_ticks));
                    s_ctx->set_bt_connecting(false);
                    control_enqueue(CMD_RESTART_DISCOVERY, s_ctx->tag);
                }
            }

            TickType_t idle_ticks = xTaskGetTickCount() - idle_without_bt_start;
            if (idle_ticks > pdMS_TO_TICKS(s_ctx->auto_sleep_idle_ms)) {
                ESP_LOGW(s_ctx->tag,
                         "⏲️ Auto-sleep: %lu ms sem BT/áudio",
                         (unsigned long)TICKS_TO_MS_LOCAL(idle_ticks));
                s_ctx->enter_deep_sleep(false);
            }
        } else {
            idle_without_bt_start = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
