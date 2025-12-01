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
 * @brief Callback for WebSocket connection state changes
 *
 * @param connected true when WebSocket connects, false when it disconnects
 * @param close_code WebSocket close code (0 if connected, RFC 6455 code if disconnected)
 * @param user_ctx User context pointer
 */
typedef void (*proxy_ws_state_cb_t)(bool connected, uint16_t close_code, void *user_ctx);

/**
 * @brief Callback for audio data received from OpenAI
 *
 * @param audio_data PCM audio data
 * @param audio_len Length of audio data in bytes
 * @param user_ctx User context pointer
 */
typedef void (*proxy_audio_received_cb_t)(const uint8_t *audio_data, size_t audio_len, void *user_ctx);

/**
 * @brief Initialize proxy client
 *
 * @param ws_state_cb Callback for WebSocket connection state (can be NULL)
 * @param audio_cb Callback for audio data received (can be NULL)
 * @param speech_cb Callback for assistant speech events (can be NULL)
 * @param user_ctx User context passed to callbacks
 */
void proxy_client_init(proxy_ws_state_cb_t ws_state_cb, proxy_audio_received_cb_t audio_cb, proxy_speech_event_cb_t speech_cb, void *user_ctx);

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
