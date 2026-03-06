#include "sleep_manager.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "power_manager.h"

void sleep_manager_enter_deep_sleep(const sleep_manager_ctx_t *ctx,
                                    bool from_power_button)
{
    if (!ctx || !ctx->tag || !ctx->stop_playback_and_reset || !ctx->free_playlist_cache) {
        return;
    }

    ESP_LOGW(ctx->tag, "Entrando em deep sleep (hold power button)");

    uint32_t wait_ms = from_power_button
                           ? ctx->release_wait_ms_from_button
                           : ctx->release_wait_ms_auto;

    if (!pm_wait_pin_inactive(ctx->wake_pin, wait_ms, ctx->tag)) {
        ESP_LOGW(ctx->tag, "Deep sleep cancelado: botão wake permaneceu LOW");
        return;
    }

    ctx->stop_playback_and_reset(true, "deep_sleep");
    ctx->free_playlist_cache();

    if (ctx->connection_timer) {
        xTimerStop(ctx->connection_timer, 0);
    }
    if (ctx->discovery_timer) {
        xTimerStop(ctx->discovery_timer, 0);
    }
    if (ctx->buffer_monitor_timer) {
        xTimerStop(ctx->buffer_monitor_timer, 0);
    }

    pm_hold_led_off_during_sleep(ctx->led_gpio, ctx->led_active_high);
    pm_configure_ext0_wakeup(ctx->wake_pin, 0, ctx->tag);

    vTaskDelay(pdMS_TO_TICKS(80));
    esp_deep_sleep_start();
}
