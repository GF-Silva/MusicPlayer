#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    uint8_t *data;
    size_t size;
    size_t write_pos;
    size_t read_pos;
    size_t available;
    SemaphoreHandle_t mutex;
    bool is_full;
    bool end_of_stream;
} ring_buffer_t;

ring_buffer_t *ring_buffer_create(size_t size);
void ring_buffer_destroy(ring_buffer_t *rb);
void ring_buffer_reset(ring_buffer_t *rb);
size_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len);
size_t ring_buffer_write_blocking(ring_buffer_t *rb,
                                  const uint8_t *data,
                                  size_t len,
                                  TickType_t max_wait_ticks,
                                  bool (*should_continue)(void *ctx),
                                  void *ctx);
size_t ring_buffer_available(ring_buffer_t *rb);

#endif
