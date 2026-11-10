#include "memory_metrics.h"

#include "esp_log.h"
#include "freertos/task.h"

memory_metrics_snapshot_t memory_metrics_take_snapshot(void)
{
    memory_metrics_snapshot_t snapshot = {
        .free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT),
        .min_free_8bit = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
        .largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        .free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
    };
    return snapshot;
}

void memory_metrics_log_snapshot(const char *tag, const char *label)
{
    memory_metrics_snapshot_t s = memory_metrics_take_snapshot();
    ESP_LOGI(tag,
             "MEM[%s] free8=%zu min8=%zu largest8=%zu free_internal=%zu largest_internal=%zu",
             label ? label : "snapshot",
             s.free_8bit,
             s.min_free_8bit,
             s.largest_8bit,
             s.free_internal,
             s.largest_internal);
}

void memory_metrics_log_delta(const char *tag,
                              const char *label,
                              const memory_metrics_snapshot_t *before)
{
    if (!before) {
        return;
    }

    memory_metrics_snapshot_t after = memory_metrics_take_snapshot();
    long delta_8bit = (long)before->free_8bit - (long)after.free_8bit;
    long delta_internal = (long)before->free_internal - (long)after.free_internal;
    ESP_LOGI(tag,
             "MEM[%s] delta8=%ld delta_internal=%ld free8=%zu largest8=%zu free_internal=%zu largest_internal=%zu",
             label ? label : "delta",
             delta_8bit,
             delta_internal,
             after.free_8bit,
             after.largest_8bit,
             after.free_internal,
             after.largest_internal);
}

void memory_metrics_log_current_task_stack(const char *tag, const char *label)
{
    /* ESP-IDF reports this high-water mark in bytes. */
    UBaseType_t minimum_free = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(tag, "MEM[stack:%s] minimum_free=%u bytes", label ? label : "current", (unsigned)minimum_free);
}
