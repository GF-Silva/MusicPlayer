#ifndef SLEEP_MANAGER_H
#define SLEEP_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

typedef struct {
    const char *tag;
    gpio_num_t wake_pin;
    gpio_num_t led_gpio;
    bool led_active_high;

    uint32_t release_wait_ms_from_button;
    uint32_t release_wait_ms_auto;

    TimerHandle_t connection_timer;
    TimerHandle_t discovery_timer;
    TimerHandle_t buffer_monitor_timer;

    void (*stop_playback_and_reset)(bool wait_task, const char *reason);
    void (*free_playlist_cache)(void);
} sleep_manager_ctx_t;

void sleep_manager_enter_deep_sleep(const sleep_manager_ctx_t *ctx,
                                    bool from_power_button);

#endif
