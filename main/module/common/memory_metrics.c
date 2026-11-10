#include "memory_metrics.h"

#include "esp_log.h"
#include "esp_system.h"

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
             "MEM[%s] free8=%u min8=%u largest8=%u free_internal=%u largest_internal=%u",
             label ? label : "snapshot",
             (unsigned)s.free_8bit,
             (unsigned)s.min_free_8bit,
             (unsigned)s.largest_8bit,
             (unsigned)s.free_internal,
             (unsigned)s.largest_internal);
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
             "MEM[%s] delta8=%ld delta_internal=%ld free8=%u largest8=%u free_internal=%u largest_internal=%u",
             label ? label : "delta",
             delta_8bit,
             delta_internal,
             (unsigned)after.free_8bit,
             (unsigned)after.largest_8bit,
             (unsigned)after.free_internal,
             (unsigned)after.largest_internal);
}
