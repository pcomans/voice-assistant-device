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

/**
 * @brief Callback for assistant speech events (start/end)
 *
 * @param is_speaking true when assistant starts speaking, false when it finishes
 * @param user_ctx User context pointer
 */
typedef void (*proxy_speech_event_cb_t)(bool is_speaking, void *user_ctx);

/**
 * @brief Initialize proxy client
 *
 * @param speech_cb Callback for assistant speech events (can be NULL)
 * @param user_ctx User context passed to speech callback
 */
void proxy_client_init(proxy_speech_event_cb_t speech_cb, void *user_ctx);

// Connect to proxy (call after WiFi is connected)
void proxy_client_connect(void);

// Get persistent session ID (loaded from NVS, persists across reboots)
const char *proxy_get_session_id(void);

// Chunked streaming API
typedef void *proxy_stream_handle_t;

// Start a new streaming session
proxy_stream_handle_t proxy_stream_begin(const char *session_id);

// Send a chunk (non-blocking, queues for background send)
bool proxy_stream_send_chunk(proxy_stream_handle_t handle, const uint8_t *pcm_data, size_t pcm_len, int chunk_index);

// Send final chunk and receive streaming response
void proxy_stream_end(proxy_stream_handle_t handle, const uint8_t *pcm_data, size_t pcm_len, int chunk_index, proxy_result_cb_t cb, void *user_ctx);
