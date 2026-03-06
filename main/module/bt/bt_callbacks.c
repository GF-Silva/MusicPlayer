#include "bt_callbacks.h"

#include <string.h>

#include "app_log.h"
#include "control_queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "input_manager.h"

static bt_callbacks_ctx_t *s_ctx;

static inline bool ctx_ready(void)
{
    return s_ctx && s_ctx->set_bt_connecting && s_ctx->log_bt_state;
}

static inline void enqueue_cmd(player_cmd_t cmd)
{
    if (s_ctx && s_ctx->tag) {
        control_enqueue(cmd, s_ctx->tag);
    }
}

void bt_callbacks_bind(bt_callbacks_ctx_t *ctx)
{
    s_ctx = ctx;
}

void bt_callbacks_connection_timeout_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!ctx_ready()) {
        return;
    }
    ESP_LOGW(s_ctx->tag, "⏱️ Timeout de conexão BT");
    s_ctx->log_bt_state("conn_timeout");
    if (!*s_ctx->bt_connected && *s_ctx->bt_connecting) {
        s_ctx->set_bt_connecting(false);
        enqueue_cmd(CMD_RETRY_CONNECTION);
    }
}

void bt_callbacks_discovery_timeout_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!ctx_ready()) {
        return;
    }
    ESP_LOGW(s_ctx->tag, "⏱️ Timeout de discovery");
    s_ctx->log_bt_state("disc_timeout");
    if (!*s_ctx->device_found && !*s_ctx->bt_connected && *s_ctx->bt_connecting) {
        s_ctx->set_bt_connecting(false);
        enqueue_cmd(CMD_RESTART_DISCOVERY);
    }
}

void bt_callbacks_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (!ctx_ready() || !param) {
        return;
    }

    ESP_LOGD(s_ctx->tag, "GAP evt: %d", event);
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char bda_str[18];
            snprintf(bda_str,
                     sizeof(bda_str),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     param->disc_res.bda[0],
                     param->disc_res.bda[1],
                     param->disc_res.bda[2],
                     param->disc_res.bda[3],
                     param->disc_res.bda[4],
                     param->disc_res.bda[5]);

            bool mac_match = false;
            if (s_ctx->target_mac_addr &&
                ((*s_ctx->target_mac_addr)[0] != 0 || (*s_ctx->target_mac_addr)[1] != 0 ||
                 (*s_ctx->target_mac_addr)[2] != 0) &&
                memcmp(param->disc_res.bda, *s_ctx->target_mac_addr, ESP_BD_ADDR_LEN) == 0) {
                mac_match = true;
            }

            if (mac_match && !*s_ctx->device_found) {
                *s_ctx->device_found = true;
                memcpy(*s_ctx->target_device_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);

                ESP_LOGI(s_ctx->tag, "✅ Dispositivo encontrado por MAC!");
                ESP_LOGI(s_ctx->tag, "   MAC: %s", bda_str);

                if (s_ctx->discovery_timer && *s_ctx->discovery_timer) {
                    xTimerStop(*s_ctx->discovery_timer, 0);
                }

                if (*s_ctx->discovery_active) {
                    *s_ctx->connect_after_discovery_stop = true;
                    if (!*s_ctx->discovery_stop_pending) {
                        *s_ctx->discovery_stop_pending = true;
                        esp_bt_gap_cancel_discovery();
                        ESP_LOGI(s_ctx->tag, "🛑 Solicitado stop discovery antes de conectar");
                    }
                } else {
                    enqueue_cmd(CMD_CONNECT_TARGET);
                }
            }
            break;
        }

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                if (*s_ctx->discovery_active) {
                    ESP_LOGI(s_ctx->tag, "🔍 Discovery parado");
                }
                *s_ctx->discovery_active = false;
                *s_ctx->discovery_stop_pending = false;
                if (!*s_ctx->bt_connected && !*s_ctx->connect_after_discovery_stop) {
                    s_ctx->set_bt_connecting(false);
                }
                s_ctx->log_bt_state("disc_stopped");
                if (*s_ctx->connect_after_discovery_stop && *s_ctx->device_found && !*s_ctx->bt_connected) {
                    *s_ctx->connect_after_discovery_stop = false;
                    enqueue_cmd(CMD_CONNECT_TARGET);
                }
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                *s_ctx->discovery_active = true;
                *s_ctx->discovery_stop_pending = false;
                ESP_LOGI(s_ctx->tag, "🔍 Discovery iniciado");
                s_ctx->log_bt_state("disc_started");
            }
            break;
        }

        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(s_ctx->tag, "✅ Autenticação OK");
            } else {
                ESP_LOGW(s_ctx->tag, "⚠️ Auth falhou: %d", param->auth_cmpl.stat);
            }
            break;

        default:
            break;
    }
}

void bt_callbacks_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (!ctx_ready() || !param) {
        return;
    }

    ESP_LOGD(s_ctx->tag, "A2DP evt: %d", event);
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t state = param->conn_stat.state;
            ESP_LOGD(s_ctx->tag, "A2DP conn state: %d", state);

            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(s_ctx->tag, "✅ A2DP conectado!");
                s_ctx->set_bt_connecting(false);
                *s_ctx->connect_after_discovery_stop = false;
                *s_ctx->a2dp_open_fail_streak = 0;

                if (s_ctx->connection_timer && *s_ctx->connection_timer) {
                    xTimerStop(*s_ctx->connection_timer, 0);
                }
                if (s_ctx->discovery_timer && *s_ctx->discovery_timer) {
                    xTimerStop(*s_ctx->discovery_timer, 0);
                }
                if (*s_ctx->discovery_active) {
                    esp_bt_gap_cancel_discovery();
                    *s_ctx->discovery_active = false;
                    *s_ctx->discovery_stop_pending = false;
                }

                esp_err_t start_err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                if (start_err == ESP_OK) {
                    ESP_LOGI(s_ctx->tag, "✅ Media control START enviado");
                } else {
                    ESP_LOGW(s_ctx->tag, "⚠️ Media control falhou: %s", esp_err_to_name(start_err));
                }

                *s_ctx->bt_connected = true;
                xEventGroupSetBits(*s_ctx->player_event_group, s_ctx->bt_connected_bit);
                s_ctx->log_bt_state("a2dp_connected");
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGW(s_ctx->tag, "⚠️ A2DP desconectado");
                bool had_stream_progress = *s_ctx->audio_playing ||
                                           *s_ctx->streaming_active ||
                                           (*s_ctx->total_bytes_streamed > 0) ||
                                           (*s_ctx->callback_count > 0);
                if (!had_stream_progress) {
                    if (*s_ctx->a2dp_open_fail_streak < UINT32_MAX) {
                        (*s_ctx->a2dp_open_fail_streak)++;
                    }
                } else {
                    *s_ctx->a2dp_open_fail_streak = 0;
                }

                ESP_LOGW(s_ctx->tag,
                         "A2DP fail streak: %lu",
                         (unsigned long)*s_ctx->a2dp_open_fail_streak);

                *s_ctx->bt_connected = false;
                *s_ctx->streaming_active = false;
                s_ctx->set_bt_connecting(false);
                *s_ctx->connect_after_discovery_stop = false;
                xEventGroupClearBits(*s_ctx->player_event_group,
                                     s_ctx->bt_connected_bit | s_ctx->stream_ready_bit);

                bool needs_stop_cleanup = had_stream_progress || *s_ctx->file_reader_task_running || (*s_ctx->current_file != NULL);
                if (needs_stop_cleanup) {
                    ESP_LOGW(s_ctx->tag, "Música interrompida, limpando estado...");
                    enqueue_cmd(CMD_STOP);
                }

                if (*s_ctx->a2dp_open_fail_streak >= s_ctx->a2dp_open_fail_rediscovery_threshold) {
                    ESP_LOGW(s_ctx->tag, "Falhas seguidas de abertura A2DP, forçando novo discovery");
                    enqueue_cmd(CMD_RESTART_DISCOVERY);
                } else {
                    enqueue_cmd(CMD_RETRY_CONNECTION);
                }
                s_ctx->log_bt_state("a2dp_disconnected");
            }
            break;
        }

        case ESP_A2D_AUDIO_STATE_EVT: {
            esp_a2d_audio_state_t state = param->audio_stat.state;

            if (state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(s_ctx->tag, "▶️ Stream de áudio iniciado");
                *s_ctx->streaming_active = true;
                xEventGroupSetBits(*s_ctx->player_event_group, s_ctx->stream_ready_bit);
                if (!*s_ctx->audio_playing) {
                    enqueue_cmd(CMD_PLAY_NEXT);
                }
                s_ctx->log_bt_state("audio_started");
            } else if (state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(s_ctx->tag, "⏸️ Stream de áudio parado");
                *s_ctx->streaming_active = false;
                xEventGroupClearBits(*s_ctx->player_event_group, s_ctx->stream_ready_bit);
                s_ctx->log_bt_state("audio_stopped");
            }
            break;
        }

        case ESP_A2D_AUDIO_CFG_EVT:
            ESP_LOGI(s_ctx->tag, "🔧 Codec configurado");
            break;

        default:
            break;
    }
}

void bt_callbacks_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (!s_ctx || !param) {
        return;
    }

    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        ESP_LOGI(s_ctx->tag, "%s", param->conn_stat.connected ? "✅ AVRC conectado" : "AVRC desconectado");
    }
}

void bt_callbacks_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    if (!s_ctx || !param) {
        return;
    }

    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            ESP_LOGI(s_ctx->tag,
                     "AVRC TG: %s",
                     param->conn_stat.connected ? "conectado" : "desconectado");
            break;

        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            if (param->psth_cmd.key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
                break;
            }
            switch (param->psth_cmd.key_code) {
                case ESP_AVRC_PT_CMD_PLAY:
                    ESP_LOGI(s_ctx->tag, "AVRCP clique: PLAY/PAUSE toggle");
                    enqueue_cmd(*s_ctx->playback_paused ? CMD_RESUME : CMD_PAUSE);
                    break;
                case ESP_AVRC_PT_CMD_PAUSE:
                    ESP_LOGI(s_ctx->tag, "AVRCP clique: PAUSE");
                    enqueue_cmd(CMD_PAUSE);
                    break;
                case ESP_AVRC_PT_CMD_STOP:
                    ESP_LOGI(s_ctx->tag, "AVRCP clique: STOP");
                    enqueue_cmd(CMD_STOP);
                    break;
                case ESP_AVRC_PT_CMD_FORWARD:
                    ESP_LOGI(s_ctx->tag, "AVRCP duplo clique: NEXT");
                    enqueue_cmd(CMD_PLAY_NEXT);
                    break;
                case ESP_AVRC_PT_CMD_BACKWARD:
                    ESP_LOGI(s_ctx->tag, "AVRCP duplo clique: PREV");
                    enqueue_cmd(CMD_PLAY_PREV);
                    break;
                case ESP_AVRC_PT_CMD_VOL_UP:
                    if (s_ctx->volume_up) {
                        s_ctx->volume_up();
                    }
                    break;
                case ESP_AVRC_PT_CMD_VOL_DOWN:
                    if (s_ctx->volume_down) {
                        s_ctx->volume_down();
                    }
                    break;
                default:
                    ESP_LOGD(s_ctx->tag,
                             "AVRCP cmd não mapeado: 0x%02X",
                             param->psth_cmd.key_code);
                    break;
            }
            break;

        default:
            break;
    }
}
