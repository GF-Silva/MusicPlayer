#include "power_manager.h"

#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void pm_set_power_led(gpio_num_t led_gpio, bool active_high, bool on)
{
    int level = on ? (active_high ? 1 : 0)
                   : (active_high ? 0 : 1);

    if (rtc_gpio_is_valid_gpio(led_gpio)) {
        rtc_gpio_hold_dis(led_gpio);
        rtc_gpio_deinit(led_gpio);
    }

    gpio_reset_pin(led_gpio);
    gpio_set_direction(led_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(led_gpio, level);
}

void pm_hold_led_off_during_sleep(gpio_num_t led_gpio, bool active_high)
{
    if (!rtc_gpio_is_valid_gpio(led_gpio)) {
        return;
    }

    rtc_gpio_deinit(led_gpio);
    rtc_gpio_init(led_gpio);
    rtc_gpio_set_direction(led_gpio, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(led_gpio, active_high ? 0 : 1);
    rtc_gpio_pullup_dis(led_gpio);
    rtc_gpio_pulldown_dis(led_gpio);
    rtc_gpio_hold_en(led_gpio);
}

esp_err_t pm_configure_ext0_wakeup(gpio_num_t wake_pin, int wake_level, const char *tag)
{
    rtc_gpio_init(wake_pin);
    rtc_gpio_set_direction(wake_pin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(wake_pin);
    rtc_gpio_pulldown_dis(wake_pin);

    esp_err_t err = esp_sleep_enable_ext0_wakeup(wake_pin, wake_level);
    if (err == ESP_OK) {
        ESP_LOGI(tag, "Wakeup deep sleep ativo no GPIO %d (nivel %s)",
                 (int)wake_pin, wake_level == 0 ? "LOW" : "HIGH");
    } else {
        ESP_LOGW(tag, "Falha ao configurar wakeup ext0: %s", esp_err_to_name(err));
    }
    return err;
}

bool pm_wait_pin_inactive(gpio_num_t wake_pin, uint32_t timeout_ms, const char *tag)
{
    if (gpio_get_level(wake_pin) != 0) {
        return true;
    }

    ESP_LOGW(tag, "Botao wake ainda pressionado, aguardando soltar...");
    TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(wake_pin) == 0) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    vTaskDelay(pdMS_TO_TICKS(80));
    return true;
}

bool pm_require_hold_low(gpio_num_t wake_pin,
                         uint32_t hold_ms,
                         uint32_t sample_ms,
                         const char *tag)
{
    if (sample_ms == 0) {
        sample_ms = 20;
    }

    gpio_reset_pin(wake_pin);
    gpio_set_direction(wake_pin, GPIO_MODE_INPUT);
    gpio_pullup_en(wake_pin);
    gpio_pulldown_dis(wake_pin);

    if (gpio_get_level(wake_pin) != 0) {
        ESP_LOGI(tag, "Wake sem hold (botao nao pressionado no inicio)");
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(hold_ms)) {
        if (gpio_get_level(wake_pin) != 0) {
            ESP_LOGI(tag, "Wake cancelado: hold menor que %lu ms", (unsigned long)hold_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(sample_ms));
    }

    ESP_LOGI(tag, "Wake confirmado por hold de %lu ms", (unsigned long)hold_ms);
    return true;
}
