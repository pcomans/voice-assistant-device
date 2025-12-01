#include "websocket_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "ws_client";

// WebSocket client state
static esp_websocket_client_handle_t s_client = NULL;
static ws_audio_received_cb_t s_audio_cb = NULL;
static ws_state_change_cb_t s_state_cb = NULL;
static ws_speech_event_cb_t s_speech_cb = NULL;
static void *s_user_ctx = NULL;
static bool s_connected = false;
static SemaphoreHandle_t s_state_mutex = NULL;
static uint16_t s_last_close_code = 0;

/**
 * @brief WebSocket event handler
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_connected = true;
        xSemaphoreGive(s_state_mutex);

        if (s_state_cb) {
            s_state_cb(true, 0, s_user_ctx);  // 0 for connected
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_connected = false;
        xSemaphoreGive(s_state_mutex);

        if (s_state_cb) {
            s_state_cb(false, s_last_close_code, s_user_ctx);
        }
        s_last_close_code = 0;  // Reset after passing to callback
        break;

    case WEBSOCKET_EVENT_DATA:
        ESP_LOGD(TAG, "WebSocket data received: %d bytes (offset=%d, payload_len=%d, fin=%d, opcode=0x%02x)",
                 data->data_len, data->payload_offset, data->payload_len, data->fin, data->op_code);

        // Handle different WebSocket frame types
        if (data->op_code == 0x02) {  // Binary frame (audio data)
            if (s_audio_cb && data->data_ptr && data->data_len > 0) {
                ESP_LOGD(TAG, "Calling audio callback with %d bytes (offset=%d/%d)",
                         data->data_len, data->payload_offset, data->payload_len);
                s_audio_cb((const uint8_t *)data->data_ptr, data->data_len, s_user_ctx);
            } else {
                ESP_LOGW(TAG, "Binary frame but no callback or empty data");
            }
        } else if (data->op_code == 0x01) {  // Text frame (control messages)
            if (data->data_ptr && data->data_len > 0) {
                ESP_LOGD(TAG, "Received text message: %.*s", data->data_len, (char *)data->data_ptr);

                // Parse JSON control message using cJSON
                if (s_speech_cb) {
                    cJSON *json = cJSON_ParseWithLength((char *)data->data_ptr, data->data_len);
                    if (json != NULL) {
                        cJSON *type = cJSON_GetObjectItem(json, "type");
                        if (cJSON_IsString(type) && type->valuestring != NULL) {
                            if (strcmp(type->valuestring, "speech_start") == 0) {
                                ESP_LOGI(TAG, "Assistant started speaking");
                                s_speech_cb(true, s_user_ctx);
                            } else if (strcmp(type->valuestring, "speech_end") == 0) {
                                ESP_LOGI(TAG, "Assistant stopped speaking");
                                s_speech_cb(false, s_user_ctx);
                            }
                        }
                        cJSON_Delete(json);
                    } else {
                        ESP_LOGW(TAG, "Failed to parse JSON control message");
                    }
                }
            }
        } else if (data->op_code == 0x08) {  // Close frame
            uint16_t close_code = 0;
            const char *close_reason = "";
            int reason_len = 0;

            if (data->data_ptr && data->data_len >= 2) {
                // Extract close code (first 2 bytes, network byte order)
                close_code = (((uint16_t)data->data_ptr[0]) << 8) | ((uint16_t)data->data_ptr[1]);
                s_last_close_code = close_code;

                // Extract close reason (remaining bytes)
                if (data->data_len > 2) {
                    close_reason = (const char *)(data->data_ptr + 2);
                    reason_len = data->data_len - 2;
                }
            }

            if (close_code == 1000) {
                ESP_LOGI(TAG, "WebSocket close: Normal closure (code=%d, reason='%.*s')",
                         close_code, reason_len, close_reason);
            } else {
                ESP_LOGW(TAG, "WebSocket Error: code=%d, reason='%.*s')",
                         close_code, reason_len, close_reason);
            }

            // Update connected state and call disconnect callback immediately
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_connected = false;
            xSemaphoreGive(s_state_mutex);

            // Call state callback immediately with close code
            if (s_state_cb) {
                s_state_cb(false, close_code, s_user_ctx);
            }

            // Reset close code after callback (will be 0 if DISCONNECTED event fires later)
            s_last_close_code = 0;
        } else if (data->op_code == 0x09) {  // Ping frame
            ESP_LOGD(TAG, "Received WebSocket ping frame");
        } else if (data->op_code == 0x0a) {  // Pong frame
            ESP_LOGD(TAG, "Received WebSocket pong frame (keepalive)");
        } else {
            ESP_LOGW(TAG, "Unknown opcode: 0x%02x", data->op_code);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error occurred");
        if (data) {
            ESP_LOGE(TAG, "Error details - type: %d, handshake_status: %d, tls_err: %d, sock_errno: %d",
                     data->error_handle.error_type,
                     data->error_handle.esp_ws_handshake_status_code,
                     data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_transport_sock_errno);
        }
        break;

    default:
        ESP_LOGD(TAG, "WebSocket event: %ld", event_id);
        break;
    }
}

esp_err_t ws_client_init(const char *uri,
                          ws_audio_received_cb_t audio_cb,
                          ws_state_change_cb_t state_cb,
                          ws_speech_event_cb_t speech_cb,
                          void *user_ctx)
{
    if (!uri) {
        ESP_LOGE(TAG, "URI cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_client) {
        ESP_LOGW(TAG, "WebSocket client already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Create mutex for state management
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    // Store callbacks
    s_audio_cb = audio_cb;
    s_state_cb = state_cb;
    s_speech_cb = speech_cb;
    s_user_ctx = user_ctx;
    s_connected = false;

    // Configure WebSocket client
    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .buffer_size = 4096,  // Larger buffer for audio chunks
        .task_stack = 8192,   // Increased stack for audio processing
        .task_prio = 5,
        .disable_auto_reconnect = true,   // Disable auto-reconnect for explicit state control
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
    };

    s_client = esp_websocket_client_init(&ws_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return ESP_FAIL;
    }

    // Register event handler
    esp_err_t err = esp_websocket_register_events(s_client,
                                                   WEBSOCKET_EVENT_ANY,
                                                   websocket_event_handler,
                                                   NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebSocket events: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WebSocket client initialized: %s", uri);
    return ESP_OK;
}

esp_err_t ws_client_connect(void)
{
    if (!s_client) {
        ESP_LOGE(TAG, "WebSocket client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Connecting to WebSocket server...");
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t ws_client_send_audio(const uint8_t *data, size_t len)
{
    if (!s_client) {
        ESP_LOGE(TAG, "WebSocket client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Check connection state
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool connected = s_connected;
    xSemaphoreGive(s_state_mutex);

    if (!connected) {
        ESP_LOGW(TAG, "Cannot send: WebSocket not connected");
        return ESP_ERR_INVALID_STATE;
    }

    // Send binary frame (opcode 0x02) with timeout to prevent blocking
    // Empty frames (len=0) are sent to signal end of turn to the proxy
    int ret = esp_websocket_client_send_bin(s_client, (const char *)data, len, pdMS_TO_TICKS(5000));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send WebSocket data (timeout or network error)");
        return ESP_ERR_TIMEOUT;
    }

    if (len == 0) {
        ESP_LOGI(TAG, "Sent empty frame to signal end of turn");
    } else {
        ESP_LOGD(TAG, "Sent %d bytes via WebSocket", ret);
    }
    return ESP_OK;
}

bool ws_client_is_connected(void)
{
    if (!s_state_mutex) {
        return false;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool connected = s_connected;
    xSemaphoreGive(s_state_mutex);

    return connected && esp_websocket_client_is_connected(s_client);
}

esp_err_t ws_client_disconnect(void)
{
    if (!s_client) {
        return ESP_OK;  // Already disconnected
    }

    ESP_LOGI(TAG, "Disconnecting WebSocket client...");
    esp_err_t err = esp_websocket_client_stop(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WebSocket client: %s", esp_err_to_name(err));
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_connected = false;
    xSemaphoreGive(s_state_mutex);

    return err;
}

esp_err_t ws_client_destroy(void)
{
    if (!s_client) {
        return ESP_OK;  // Already destroyed
    }

    // Stop if still running
    ws_client_disconnect();

    // Destroy client
    esp_err_t err = esp_websocket_client_destroy(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to destroy WebSocket client: %s", esp_err_to_name(err));
    }

    s_client = NULL;
    s_audio_cb = NULL;
    s_state_cb = NULL;
    s_speech_cb = NULL;
    s_user_ctx = NULL;

    if (s_state_mutex) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }

    ESP_LOGI(TAG, "WebSocket client destroyed");
    return err;
}
