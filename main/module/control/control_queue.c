#include "control_queue.h"

#include "esp_log.h"
#include "freertos/queue.h"
#include "memory_metrics.h"

static QueueHandle_t s_control_queue = NULL;
static size_t s_control_queue_depth;
static size_t s_control_queue_item_size;
static volatile bool s_cmd_play_next_pending = false;
static volatile bool s_cmd_play_prev_pending = false;
static volatile bool s_cmd_retry_pending = false;
static volatile bool s_cmd_restart_disc_pending = false;
static volatile bool s_cmd_connect_pending = false;

static volatile bool *pending_flag_for_cmd(player_cmd_t cmd)
{
    switch (cmd) {
        case CMD_PLAY_NEXT: return &s_cmd_play_next_pending;
        case CMD_PLAY_PREV: return &s_cmd_play_prev_pending;
        case CMD_RETRY_CONNECTION: return &s_cmd_retry_pending;
        case CMD_RESTART_DISCOVERY: return &s_cmd_restart_disc_pending;
        case CMD_CONNECT_TARGET: return &s_cmd_connect_pending;
        default: return NULL;
    }
}

const char *control_cmd_to_str(player_cmd_t cmd)
{
    switch (cmd) {
        case CMD_PLAY_NEXT: return "PLAY_NEXT";
        case CMD_PLAY_PREV: return "PLAY_PREV";
        case CMD_STOP: return "STOP";
        case CMD_PAUSE: return "PAUSE";
        case CMD_RESUME: return "RESUME";
        case CMD_RETRY_CONNECTION: return "RETRY_CONNECTION";
        case CMD_RESTART_DISCOVERY: return "RESTART_DISCOVERY";
        case CMD_CONNECT_TARGET: return "CONNECT_TARGET";
        case CMD_FILL_BUFFERS: return "FILL_BUFFERS";
        case CMD_TOGGLE_PAUSE: return "TOGGLE_PAUSE";
        default: return "UNKNOWN";
    }
}

esp_err_t control_queue_init(size_t depth)
{
    if (s_control_queue) return ESP_OK;

    memory_metrics_snapshot_t before = memory_metrics_take_snapshot();
    s_control_queue = xQueueCreate(depth, sizeof(player_cmd_t));
    if (!s_control_queue) return ESP_ERR_NO_MEM;

    s_control_queue_depth = depth;
    s_control_queue_item_size = sizeof(player_cmd_t);
    ESP_LOGI("control_queue", "MEM[config:control_queue] depth=%zu item=%zu payload=%zu bytes (queue metadata additional)",
             depth, sizeof(player_cmd_t), depth * sizeof(player_cmd_t));
    memory_metrics_log_delta("control_queue", "control_queue_created", &before);
    return ESP_OK;
}

void control_queue_deinit(void)
{
    if (s_control_queue) {
        memory_metrics_snapshot_t before = memory_metrics_take_snapshot();
        vQueueDelete(s_control_queue);
        s_control_queue = NULL;
        memory_metrics_log_delta("control_queue", "control_queue_deleted", &before);
    }
    s_control_queue_depth = 0;
    s_control_queue_item_size = 0;
    s_cmd_play_next_pending = false;
    s_cmd_play_prev_pending = false;
    s_cmd_retry_pending = false;
    s_cmd_restart_disc_pending = false;
    s_cmd_connect_pending = false;
}

bool control_enqueue(player_cmd_t cmd, const char *tag)
{
    volatile bool *pending = pending_flag_for_cmd(cmd);
    if (!s_control_queue) {
        ESP_LOGW(tag, "Queue nao inicializada: %s", control_cmd_to_str(cmd));
        return false;
    }
    if (pending && *pending) {
        ESP_LOGD(tag, "Queue dedup: %s ja pendente", control_cmd_to_str(cmd));
        return false;
    }
    if (pending) *pending = true;
    if (xQueueSend(s_control_queue, &cmd, 0) != pdTRUE) {
        if (pending) *pending = false;
        ESP_LOGW(tag, "Queue cheia ao enfileirar: %s", control_cmd_to_str(cmd));
        control_log_state(tag, "send_fail");
        return false;
    }
    ESP_LOGD(tag, "Queue <- %s", control_cmd_to_str(cmd));
    control_log_state(tag, "after_enqueue");
    return true;
}

bool control_dequeue(player_cmd_t *out_cmd, TickType_t wait_ticks)
{
    return s_control_queue && out_cmd && xQueueReceive(s_control_queue, out_cmd, wait_ticks) == pdTRUE;
}

void control_mark_handled(player_cmd_t cmd)
{
    volatile bool *pending = pending_flag_for_cmd(cmd);
    if (pending) *pending = false;
}

void control_flush(void)
{
    if (!s_control_queue) return;
    player_cmd_t cmd;
    while (xQueueReceive(s_control_queue, &cmd, 0) == pdTRUE) control_mark_handled(cmd);
}

bool control_is_pending(player_cmd_t cmd)
{
    volatile bool *pending = pending_flag_for_cmd(cmd);
    return pending ? *pending : false;
}

UBaseType_t control_pending_count(void)
{
    return s_control_queue ? uxQueueMessagesWaiting(s_control_queue) : 0;
}

void control_log_state(const char *tag, const char *reason)
{
    ESP_LOGD(tag, "QUEUE[%s] pending:%lu", reason ? reason : "-",
             (unsigned long)control_pending_count());
}
