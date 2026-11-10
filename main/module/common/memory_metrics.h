#ifndef MEMORY_METRICS_H
#define MEMORY_METRICS_H

#include <stddef.h>

#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t free_8bit;
    size_t min_free_8bit;
    size_t largest_8bit;
    size_t free_internal;
    size_t largest_internal;
} memory_metrics_snapshot_t;

memory_metrics_snapshot_t memory_metrics_take_snapshot(void);
void memory_metrics_log_snapshot(const char *tag, const char *label);
void memory_metrics_log_delta(const char *tag,
                              const char *label,
                              const memory_metrics_snapshot_t *before);
void memory_metrics_log_current_task_stack(const char *tag, const char *label);

#ifdef __cplusplus
}
#endif

#endif
