#ifndef A2DP_STREAM_H
#define A2DP_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "ring_buffer.h"

typedef struct {
    const char *tag;

    ring_buffer_t **stream_buffer;

    bool *bt_connected;
    bool *playback_paused;
    bool *stream_stabilized;

    uint32_t *total_bytes_streamed;
    uint32_t *underrun_count;
    uint32_t *callback_count;
    uint32_t *cb_lock_fail_count;
    uint32_t *cb_empty_count;
    uint32_t *cb_partial_count;
} a2dp_stream_ctx_t;

void a2dp_stream_bind(a2dp_stream_ctx_t *ctx);
int32_t a2dp_stream_source_data_cb(uint8_t *data, int32_t len);

#endif
