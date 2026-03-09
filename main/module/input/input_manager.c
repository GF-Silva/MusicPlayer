#include "input_manager.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_manager.h"

static input_manager_cfg_t s_cfg;
static bool s_initialized = false;

static bool s_pwr_last_level = true;
static TickType_t s_pwr_last_change_time = 0;
static TickType_t s_pwr_press_start_time = 0;
static TickType_t s_pwr_last_click_time = 0;
static bool s_pwr_pressed = false;
static bool s_pwr_hold_handled = false;
static bool s_pwr_sleep_armed = false;
static uint8_t s_pwr_click_count = 0;

static bool led_pin_configured(void)
{
    return s_cfg.led_gpio >= 0;
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
        .pin_bit_mask = (1ULL << s_cfg.pin_power),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_pwr_last_level = true;
    s_pwr_last_change_time = 0;
    s_pwr_press_start_time = 0;
    s_pwr_last_click_time = 0;
    s_pwr_pressed = false;
    s_pwr_hold_handled = false;
    s_pwr_sleep_armed = false;
    s_pwr_click_count = 0;

    s_initialized = true;

    ESP_LOGI(s_cfg.log_tag, "Botão POWER configurado (%d)", s_cfg.pin_power);

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

    ESP_LOGI(s_cfg.log_tag, "Task de input iniciada");

    while (1) {
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
                TickType_t press_duration = now - s_pwr_press_start_time;
                s_pwr_pressed = false;
                s_pwr_hold_handled = false;

                if (s_pwr_sleep_armed) {
                    ESP_LOGW(s_cfg.log_tag, "Botão solto após hold: entrando em deep sleep");
                    s_cfg.on_power_hold(true);
                    if (led_pin_configured()) {
                        pm_set_power_led((gpio_num_t)s_cfg.led_gpio, s_cfg.led_active_high, true);
                    }
                    s_pwr_sleep_armed = false;
                    s_pwr_click_count = 0;
                } else if (press_duration < pdMS_TO_TICKS(s_cfg.long_click_threshold_ms)) {
                    if ((now - s_pwr_last_click_time) < pdMS_TO_TICKS(s_cfg.double_click_interval_ms)) {
                        s_pwr_click_count++;
                    } else {
                        s_pwr_click_count = 1;
                    }
                    s_pwr_last_click_time = now;
                }
            }
        }

        if (s_pwr_pressed && !s_pwr_hold_handled &&
            (now - s_pwr_press_start_time) > pdMS_TO_TICKS(s_cfg.power_hold_ms)) {
            s_pwr_hold_handled = true;
            s_pwr_sleep_armed = true;
            s_pwr_click_count = 0;
            ESP_LOGW(s_cfg.log_tag, "Power hold confirmado (%u ms): aguardando soltar para dormir",
                     (unsigned)s_cfg.power_hold_ms);
            if (led_pin_configured()) {
                pm_set_power_led((gpio_num_t)s_cfg.led_gpio, s_cfg.led_active_high, false);
            }
        }

        if (!s_pwr_pressed &&
            !s_pwr_sleep_armed &&
            s_pwr_click_count > 0 &&
            (now - s_pwr_last_click_time) > pdMS_TO_TICKS(s_cfg.click_timeout_ms)) {
            if (s_pwr_click_count == 2) {
                ESP_LOGI(s_cfg.log_tag, "Power: 2 cliques -> volume +");
                input_manager_volume_up();
            } else if (s_pwr_click_count == 3) {
                ESP_LOGI(s_cfg.log_tag, "Power: 3 cliques -> volume -");
                input_manager_volume_down();
            } else {
                ESP_LOGD(s_cfg.log_tag, "Power: sequencia ignorada (%u cliques)",
                         (unsigned)s_pwr_click_count);
            }
            s_pwr_click_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
