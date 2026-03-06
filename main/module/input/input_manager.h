#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "control_queue.h"

typedef struct {
    int pin_vol_up;
    int pin_vol_down;
    int pin_power;

    uint32_t debounce_ms;
    uint32_t click_timeout_ms;
    uint32_t double_click_interval_ms;
    uint32_t long_click_threshold_ms;
    uint32_t power_hold_ms;
    uint32_t next_track_guard_ms;

    uint8_t volume_step;
    uint8_t *volume_percent;
    float *volume_scale;

    const char *log_tag;
    void (*on_power_hold)(bool from_power_button);
} input_manager_cfg_t;

esp_err_t input_manager_init(const input_manager_cfg_t *cfg);
void input_manager_volume_up(void);
void input_manager_volume_down(void);
void input_manager_task(void *pvParameter);

#endif
