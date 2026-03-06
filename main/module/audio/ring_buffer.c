#include "ring_buffer.h"

#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

ring_buffer_t *ring_buffer_create(size_t size)
{
    ring_buffer_t *rb = malloc(sizeof(ring_buffer_t));
    if (!rb) return NULL;

    rb->data = heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!rb->data) {
        free(rb);
        return NULL;
    }

    rb->size = size;
    rb->write_pos = 0;
    rb->read_pos = 0;
    rb->available = 0;
    rb->is_full = false;
    rb->end_of_stream = false;
    rb->mutex = xSemaphoreCreateMutex();

    if (!rb->mutex) {
        free(rb->data);
        free(rb);
        return NULL;
    }

    return rb;
}

void ring_buffer_destroy(ring_buffer_t *rb)
{
    if (!rb) return;

    if (rb->mutex) {
        vSemaphoreDelete(rb->mutex);
    }
    if (rb->data) {
        free(rb->data);
    }
    free(rb);
}

void ring_buffer_reset(ring_buffer_t *rb)
{
    if (!rb) return;

    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(100))) {
        rb->write_pos = 0;
        rb->read_pos = 0;
        rb->available = 0;
        rb->is_full = false;
        rb->end_of_stream = false;
        xSemaphoreGive(rb->mutex);
    }
}

size_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len)
{
    if (!rb || !data || len == 0) return 0;

    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }

    size_t space = rb->size - rb->available;
    size_t to_write = (len > space) ? space : len;
    size_t written = 0;

    while (written < to_write) {
        size_t chunk = to_write - written;
        size_t space_to_end = rb->size - rb->write_pos;

        if (chunk > space_to_end) chunk = space_to_end;

        memcpy(rb->data + rb->write_pos, data + written, chunk);
        rb->write_pos = (rb->write_pos + chunk) % rb->size;
        written += chunk;
    }

    rb->available += written;
    if (rb->available == rb->size) {
        rb->is_full = true;
    }

    xSemaphoreGive(rb->mutex);

    return written;
}

size_t ring_buffer_write_blocking(ring_buffer_t *rb,
                                  const uint8_t *data,
                                  size_t len,
                                  TickType_t max_wait_ticks,
                                  bool (*should_continue)(void *ctx),
                                  void *ctx)
{
    if (!rb || !data || len == 0) return 0;

    size_t total_written = 0;
    TickType_t start = xTaskGetTickCount();

    while (total_written < len) {
        if (should_continue && !should_continue(ctx)) {
            break;
        }

        size_t written = ring_buffer_write(rb, data + total_written, len - total_written);
        total_written += written;

        if (total_written >= len) {
            break;
        }

        if ((xTaskGetTickCount() - start) >= max_wait_ticks) {
            break;
        }

        vTaskDelay(1);
    }

    return total_written;
}

size_t ring_buffer_available(ring_buffer_t *rb)
{
    if (!rb) return 0;

    size_t available = 0;
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(5))) {
        available = rb->available;
        xSemaphoreGive(rb->mutex);
    }

    return available;
}
