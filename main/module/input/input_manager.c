#include "input_manager.h"

#include "driver/gpio.h"
#include "esp_a2dp_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint8_t pin;
    bool last_state;
    bool pressed;
    TickType_t last_change_time;
    TickType_t press_start_time;
    TickType_t last_click_time;
    uint8_t click_count;
    bool is_long_press;
} button_state_t;

static input_manager_cfg_t s_cfg;
static bool s_initialized = false;

static button_state_t s_vol_up_btn;
static button_state_t s_vol_down_btn;

static bool s_pwr_last_level = true;
static TickType_t s_pwr_last_change_time = 0;
static TickType_t s_pwr_press_start_time = 0;
static bool s_pwr_pressed = false;
static bool s_pwr_hold_handled = false;

static uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)ticks * (uint32_t)portTICK_PERIOD_MS;
}

static void handle_button(button_state_t *btn)
{
    bool current_state = gpio_get_level((gpio_num_t)btn->pin);
    TickType_t now = xTaskGetTickCount();

    if (current_state != btn->last_state) {
        if ((now - btn->last_change_time) > pdMS_TO_TICKS(s_cfg.debounce_ms)) {
            btn->last_change_time = now;
            btn->last_state = current_state;

            if (!current_state) {
                btn->pressed = true;
                btn->press_start_time = now;
                btn->is_long_press = false;
            } else if (btn->pressed) {
                btn->pressed = false;

                TickType_t press_duration = now - btn->press_start_time;
                if (!btn->is_long_press &&
                    press_duration < pdMS_TO_TICKS(s_cfg.long_click_threshold_ms)) {
                    if ((now - btn->last_click_time) < pdMS_TO_TICKS(s_cfg.double_click_interval_ms)) {
                        btn->click_count++;
                    } else {
                        btn->click_count = 1;
                    }
                    btn->last_click_time = now;
                }
            }
        }
    } else if (btn->pressed && !btn->is_long_press) {
        TickType_t press_duration = now - btn->press_start_time;
        if (press_duration >= pdMS_TO_TICKS(s_cfg.long_click_threshold_ms)) {
            btn->is_long_press = true;
        }
    }
}

static void set_volume(uint8_t volume)
{
    if (!s_initialized || !s_cfg.volume_percent || !s_cfg.volume_scale) {
        return;
    }

    if (volume > 100) {
        volume = 100;
    }

    *s_cfg.volume_percent = volume;
    *s_cfg.volume_scale = volume / 100.0f;

    ESP_LOGI(s_cfg.log_tag, "Volume: %u%% (%.2f)",
             (unsigned)*s_cfg.volume_percent,
             *s_cfg.volume_scale);
}

esp_err_t input_manager_init(const input_manager_cfg_t *cfg)
{
    if (!cfg || !cfg->volume_percent || !cfg->volume_scale || !cfg->on_power_hold || !cfg->log_tag) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *cfg;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin_vol_up) |
                        (1ULL << s_cfg.pin_vol_down) |
                        (1ULL << s_cfg.pin_power),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_vol_up_btn = (button_state_t){
        .pin = (uint8_t)s_cfg.pin_vol_up,
        .last_state = true,
    };
    s_vol_down_btn = (button_state_t){
        .pin = (uint8_t)s_cfg.pin_vol_down,
        .last_state = true,
    };

    s_pwr_last_level = true;
    s_pwr_last_change_time = 0;
    s_pwr_press_start_time = 0;
    s_pwr_pressed = false;
    s_pwr_hold_handled = false;

    s_initialized = true;

    ESP_LOGI(s_cfg.log_tag, "Botões configurados (VOL:%d/%d POWER:%d)",
             s_cfg.pin_vol_up, s_cfg.pin_vol_down, s_cfg.pin_power);

    return ESP_OK;
}

void input_manager_volume_up(void)
{
    if (!s_initialized || !s_cfg.volume_percent) {
        return;
    }

    uint8_t new_vol = (uint8_t)(*s_cfg.volume_percent + s_cfg.volume_step);
    if (new_vol > 100) {
        new_vol = 100;
    }
    set_volume(new_vol);
}

void input_manager_volume_down(void)
{
    if (!s_initialized || !s_cfg.volume_percent) {
        return;
    }

    int new_vol = (int)(*s_cfg.volume_percent) - (int)s_cfg.volume_step;
    if (new_vol < 0) {
        new_vol = 0;
    }
    set_volume((uint8_t)new_vol);
}

void input_manager_task(void *pvParameter)
{
    (void)pvParameter;
    if (!s_initialized) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(s_cfg.log_tag, "Task controle volume iniciada");

    TickType_t last_process_time = xTaskGetTickCount();
    TickType_t last_next_track_cmd = 0;

    while (1) {
        handle_button(&s_vol_up_btn);
        handle_button(&s_vol_down_btn);

        TickType_t now = xTaskGetTickCount();

        bool pwr_level = gpio_get_level((gpio_num_t)s_cfg.pin_power);
        if (pwr_level != s_pwr_last_level &&
            (now - s_pwr_last_change_time) > pdMS_TO_TICKS(s_cfg.debounce_ms)) {
            s_pwr_last_change_time = now;
            s_pwr_last_level = pwr_level;
            if (!pwr_level) {
                s_pwr_pressed = true;
                s_pwr_hold_handled = false;
                s_pwr_press_start_time = now;
            } else {
                s_pwr_pressed = false;
                s_pwr_hold_handled = false;
            }
        }

        if (s_pwr_pressed && !s_pwr_hold_handled &&
            (now - s_pwr_press_start_time) > pdMS_TO_TICKS(s_cfg.power_hold_ms)) {
            s_pwr_hold_handled = true;
            ESP_LOGW(s_cfg.log_tag, "Power hold detectado (%u ms)",
                     (unsigned)s_cfg.power_hold_ms);
            s_cfg.on_power_hold(true);
        }

        if (s_vol_up_btn.click_count > 0 &&
            now - s_vol_up_btn.last_click_time > pdMS_TO_TICKS(s_cfg.click_timeout_ms)) {
            if (s_vol_up_btn.click_count == 2) {
                if ((now - last_next_track_cmd) > pdMS_TO_TICKS(s_cfg.next_track_guard_ms)) {
                    ESP_LOGI(s_cfg.log_tag, "Duplo clique: Música ALEATÓRIA");

                    int cleared = (int)control_pending_count();
                    control_flush();
                    if (cleared > 0) {
                        ESP_LOGI(s_cfg.log_tag, "Removidos %d comandos da queue", cleared);
                    }

                    control_enqueue(CMD_PLAY_NEXT, s_cfg.log_tag);
                    last_next_track_cmd = now;
                } else {
                    ESP_LOGW(s_cfg.log_tag, "Duplo clique ignorado (aguarde %u ms)",
                             (unsigned)s_cfg.next_track_guard_ms);
                }
            } else if (s_vol_up_btn.click_count == 1) {
                input_manager_volume_up();
            }
            s_vol_up_btn.click_count = 0;
        }

        if (s_vol_up_btn.is_long_press) {
            input_manager_volume_up();
            s_vol_up_btn.is_long_press = false;
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (s_vol_down_btn.click_count > 0 &&
            now - s_vol_down_btn.last_click_time > pdMS_TO_TICKS(s_cfg.click_timeout_ms)) {
            input_manager_volume_down();
            s_vol_down_btn.click_count = 0;
        }

        if (s_vol_down_btn.is_long_press) {
            input_manager_volume_down();
            s_vol_down_btn.is_long_press = false;
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (now - last_process_time >= pdMS_TO_TICKS(50)) {
            last_process_time = now;
        }

        (void)ticks_to_ms(now - last_process_time);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
