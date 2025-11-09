#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PROXY_RESULT_OK = 0,
    PROXY_RESULT_RETRY,
    PROXY_RESULT_FAILED,
} proxy_result_t;

typedef void (*proxy_result_cb_t)(proxy_result_t result, void *user_ctx);

void proxy_client_init(void);
void proxy_send_recording(proxy_result_cb_t cb, void *user_ctx);

// Chunked streaming API (Option C)
typedef void *proxy_stream_handle_t;

// Start a new streaming session
proxy_stream_handle_t proxy_stream_begin(const char *session_id);

// Send a chunk (non-blocking, queues for background send)
bool proxy_stream_send_chunk(proxy_stream_handle_t handle, const uint8_t *pcm_data, size_t pcm_len, int chunk_index);

// Send final chunk and receive streaming response
void proxy_stream_end(proxy_stream_handle_t handle, const uint8_t *pcm_data, size_t pcm_len, int chunk_index, proxy_result_cb_t cb, void *user_ctx);
