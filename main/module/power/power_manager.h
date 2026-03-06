#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

void pm_set_power_led(gpio_num_t led_gpio, bool active_high, bool on);
void pm_hold_led_off_during_sleep(gpio_num_t led_gpio, bool active_high);

esp_err_t pm_configure_ext0_wakeup(gpio_num_t wake_pin, int wake_level, const char *tag);
bool pm_wait_pin_inactive(gpio_num_t wake_pin, uint32_t timeout_ms, const char *tag);

#endif
