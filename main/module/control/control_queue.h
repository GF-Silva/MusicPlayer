#ifndef CONTROL_QUEUE_H
#define CONTROL_QUEUE_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    CMD_PLAY_NEXT,
    CMD_PLAY_PREV,
    CMD_STOP,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_RETRY_CONNECTION,
    CMD_RESTART_DISCOVERY,
    CMD_CONNECT_TARGET,
    CMD_FILL_BUFFERS,
    CMD_TOGGLE_PAUSE
} player_cmd_t;

const char *control_cmd_to_str(player_cmd_t cmd);

esp_err_t control_queue_init(size_t depth);
void control_queue_deinit(void);

bool control_enqueue(player_cmd_t cmd, const char *tag);
bool control_dequeue(player_cmd_t *out_cmd, TickType_t wait_ticks);

void control_mark_handled(player_cmd_t cmd);
void control_flush(void);

bool control_is_pending(player_cmd_t cmd);
UBaseType_t control_pending_count(void);
void control_log_state(const char *tag, const char *reason);

#endif
