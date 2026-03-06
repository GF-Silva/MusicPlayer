#include "a2dp_stream.h"

#include <string.h>

#include "app_log.h"

static a2dp_stream_ctx_t *s_ctx;

void a2dp_stream_bind(a2dp_stream_ctx_t *ctx)
{
    s_ctx = ctx;
}

static inline bool ready(void)
{
    return s_ctx && s_ctx->tag && s_ctx->stream_buffer &&
           s_ctx->bt_connected && s_ctx->playback_paused && s_ctx->stream_stabilized &&
           s_ctx->total_bytes_streamed && s_ctx->underrun_count && s_ctx->callback_count &&
           s_ctx->cb_lock_fail_count && s_ctx->cb_empty_count && s_ctx->cb_partial_count;
}

int32_t a2dp_stream_source_data_cb(uint8_t *data, int32_t len)
{
    static uint8_t silence_packets = 0;

    if (!ready() || !data || len <= 0) {
        return 0;
    }

    (*s_ctx->callback_count)++;

    if (!*s_ctx->bt_connected || *s_ctx->playback_paused) {
        memset(data, 0, (size_t)len);
        return len;
    }

    if (!*s_ctx->stream_stabilized) {
        if (silence_packets < 12) {
            memset(data, 0, (size_t)len);
            silence_packets++;
            return len;
        }
        *s_ctx->stream_stabilized = true;
    }

    if (!*s_ctx->stream_buffer) {
        memset(data, 0, (size_t)len);
        return len;
    }

    ring_buffer_t *rb = *s_ctx->stream_buffer;

    BaseType_t taken = xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(1));
    if (taken != pdTRUE) {
        memset(data, 0, (size_t)len);
        (*s_ctx->underrun_count)++;
        (*s_ctx->cb_lock_fail_count)++;
        return len;
    }

    size_t available = rb->available;
    if (available == 0) {
        xSemaphoreGive(rb->mutex);
        (*s_ctx->underrun_count)++;
        (*s_ctx->cb_empty_count)++;
        memset(data, 0, (size_t)len);
        return len;
    }

    size_t to_read = ((size_t)len > available) ? available : (size_t)len;
    size_t read_total = 0;

    while (read_total < to_read) {
        size_t chunk = to_read - read_total;
        size_t data_to_end = rb->size - rb->read_pos;

        if (chunk > data_to_end) {
            chunk = data_to_end;
        }

        memcpy(data + read_total, rb->data + rb->read_pos, chunk);
        rb->read_pos = (rb->read_pos + chunk) % rb->size;
        read_total += chunk;
    }

    rb->available -= read_total;
    rb->is_full = false;

    xSemaphoreGive(rb->mutex);

    *s_ctx->total_bytes_streamed += (uint32_t)read_total;

    if (read_total < (size_t)len) {
        memset(data + read_total, 0, (size_t)len - read_total);
        (*s_ctx->underrun_count)++;
        (*s_ctx->cb_partial_count)++;
    }

    if ((*s_ctx->callback_count % 2500U) == 0U) {
        PERF_LOGI(s_ctx->tag,
                  "A2DP: %lu KB streamed, %lu underruns, buf:%zu cb_lock/empty/partial:%lu/%lu/%lu",
                  (unsigned long)(*s_ctx->total_bytes_streamed / 1024U),
                  (unsigned long)*s_ctx->underrun_count,
                  available,
                  (unsigned long)*s_ctx->cb_lock_fail_count,
                  (unsigned long)*s_ctx->cb_empty_count,
                  (unsigned long)*s_ctx->cb_partial_count);
    }

    return len;
}
