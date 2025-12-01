#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Callback for received audio data from WebSocket
 *
 * @param data Pointer to received audio data
 * @param len Length of received data in bytes
 * @param user_ctx User context pointer passed during init
 */
typedef void (*ws_audio_received_cb_t)(const uint8_t *data, size_t len, void *user_ctx);

/**
 * @brief Callback for WebSocket connection state changes
 *
 * @param connected true if connected, false if disconnected
 * @param close_code WebSocket close code (0 if connected, RFC 6455 code if disconnected)
 * @param user_ctx User context pointer passed during init
 */
typedef void (*ws_state_change_cb_t)(bool connected, uint16_t close_code, void *user_ctx);

/**
 * @brief Callback for assistant speech events (start/end)
 *
 * @param is_speaking true when assistant starts speaking, false when it finishes
 * @param user_ctx User context pointer passed during init
 */
typedef void (*ws_speech_event_cb_t)(bool is_speaking, void *user_ctx);

/**
 * @brief Initialize WebSocket client
 *
 * @param uri WebSocket URI (e.g., "ws://192.168.7.75:8000/ws")
 * @param audio_cb Callback for received audio data
 * @param state_cb Callback for connection state changes
 * @param speech_cb Callback for assistant speech events (can be NULL)
 * @param user_ctx User context passed to callbacks
 * @return ESP_OK on success
 */
esp_err_t ws_client_init(const char *uri,
                          ws_audio_received_cb_t audio_cb,
                          ws_state_change_cb_t state_cb,
                          ws_speech_event_cb_t speech_cb,
                          void *user_ctx);

/**
 * @brief Connect to WebSocket server
 *
 * @return ESP_OK on success
 */
esp_err_t ws_client_connect(void);

/**
 * @brief Send binary audio data over WebSocket
 *
 * @param data Pointer to audio data
 * @param len Length of data in bytes
 * @return ESP_OK on success
 */
esp_err_t ws_client_send_audio(const uint8_t *data, size_t len);

/**
 * @brief Check if WebSocket is connected
 *
 * @return true if connected, false otherwise
 */
bool ws_client_is_connected(void);

/**
 * @brief Disconnect from WebSocket server
 *
 * @return ESP_OK on success
 */
esp_err_t ws_client_disconnect(void);

/**
 * @brief Cleanup and destroy WebSocket client
 *
 * @return ESP_OK on success
 */
esp_err_t ws_client_destroy(void);
