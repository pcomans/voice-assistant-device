#include "proxy_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "smart_assistant.h"
#include "websocket_client.h"
#include "wifi_credentials.h"

static const char *TAG = "proxy_client";

#define PROXY_DEFAULT_URL   WEBSOCKET_URL

#define PROXY_DEFAULT_TOKEN "498b1b65-26a3-49e8-a55e-46a0b47365e2"

// NVS keys for persistent session ID
#define NVS_NAMESPACE       "proxy_client"
#define NVS_SESSION_ID_KEY  "session_id"

typedef struct {
    char url[128];
    char token[64];
    char session_id[32];  // Persistent session ID
    bool session_id_loaded;
} proxy_config_t;

static proxy_config_t s_config = {
    .url = PROXY_DEFAULT_URL,
    .token = PROXY_DEFAULT_TOKEN,
    .session_id = {0},
    .session_id_loaded = false,
};

// WebSocket state for receiving audio
static bool s_ws_connected = false;
static bool s_ws_receiving_audio = false;
static size_t s_received_audio_bytes = 0;

// User callbacks
static proxy_ws_state_cb_t s_user_ws_state_cb = NULL;
static proxy_audio_received_cb_t s_user_audio_cb = NULL;
static void *s_user_ctx = NULL;

static void *proxy_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void proxy_free(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

static void load_or_create_session_id(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        goto generate_new;
    }

    // Try to load existing session ID
    size_t session_id_len = sizeof(s_config.session_id);
    err = nvs_get_str(nvs_handle, NVS_SESSION_ID_KEY, s_config.session_id, &session_id_len);
    if (err == ESP_OK && session_id_len > 0) {
        ESP_LOGI(TAG, "Loaded persistent session ID: %s", s_config.session_id);
        s_config.session_id_loaded = true;
        nvs_close(nvs_handle);
        return;
    }

    // Generate new session ID if not found
    ESP_LOGI(TAG, "No session ID found, generating new one");

generate_new:
    snprintf(s_config.session_id, sizeof(s_config.session_id), "esp32-%08lx", (unsigned long)esp_random());
    ESP_LOGI(TAG, "Generated new session ID: %s", s_config.session_id);

    // Save to NVS
    if (err == ESP_OK) {  // NVS handle is still open
        err = nvs_set_str(nvs_handle, NVS_SESSION_ID_KEY, s_config.session_id);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Saved session ID to NVS");
            } else {
                ESP_LOGW(TAG, "Failed to commit session ID to NVS: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Failed to save session ID to NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
    }
    s_config.session_id_loaded = true;
}

// WebSocket callbacks
static void ws_audio_received_handler(const uint8_t *data, size_t len, void *user_ctx)
{
    // Call user audio callback if registered
    if (s_user_audio_cb) {
        s_user_audio_cb(data, len, s_user_ctx);
    } else {
        // Fallback: Stream audio directly to playback
        if (audio_playback_stream_write(data, len)) {
            ESP_LOGD(TAG, "Streamed %zu bytes to playback", len);
        } else {
            ESP_LOGW(TAG, "Ring buffer full, dropped %zu bytes", len);
        }
    }
}

static void ws_state_change_handler(bool connected, uint16_t close_code, void *user_ctx)
{
    (void)user_ctx;

    s_ws_connected = connected;
    if (connected) {
        ESP_LOGI(TAG, "WebSocket connected to proxy");
    } else {
        ESP_LOGW(TAG, "WebSocket disconnected from proxy (code=%d)", close_code);
        s_ws_receiving_audio = false;
    }

    // Call user callback if registered
    if (s_user_ws_state_cb) {
        s_user_ws_state_cb(connected, close_code, s_user_ctx);
    }
}

void proxy_client_init(proxy_ws_state_cb_t ws_state_cb, proxy_audio_received_cb_t audio_cb, proxy_speech_event_cb_t speech_cb, void *user_ctx)
{
    // Store user callbacks
    s_user_ws_state_cb = ws_state_cb;
    s_user_audio_cb = audio_cb;
    s_user_ctx = user_ctx;

    load_or_create_session_id();
    ESP_LOGI(TAG, "Proxy client initialised using %s (session: %s)", s_config.url, s_config.session_id);

    // Initialize WebSocket client (but don't connect yet - wait for WiFi)
    esp_err_t err = ws_client_init(s_config.url, ws_audio_received_handler, ws_state_change_handler, speech_cb, user_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "WebSocket client initialized (waiting for WiFi to connect)");
    // TODO: load proxy URL/token from NVS, warm up TLS credentials.
}

void proxy_client_connect(void)
{
    ESP_LOGI(TAG, "WiFi ready, connecting WebSocket to proxy...");

    // Connect to WebSocket server
    esp_err_t err = ws_client_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket connection: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "WebSocket connection initiated");
}

const char *proxy_get_session_id(void)
{
    if (!s_config.session_id_loaded) {
        load_or_create_session_id();
    }
    return s_config.session_id;
}

// ==================== Chunked Streaming API ====================

typedef struct {
    char session_id[32];
} proxy_stream_ctx_t;

proxy_stream_handle_t proxy_stream_begin(const char *session_id)
{
    if (!session_id) {
        ESP_LOGE(TAG, "session_id is NULL");
        return NULL;
    }

    proxy_stream_ctx_t *ctx = malloc(sizeof(proxy_stream_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate stream context");
        return NULL;
    }

    snprintf(ctx->session_id, sizeof(ctx->session_id), "%s", session_id);
    ESP_LOGI(TAG, "Started streaming session: %s", ctx->session_id);
    return (proxy_stream_handle_t)ctx;
}

bool proxy_stream_send_chunk(proxy_stream_handle_t handle, const uint8_t *pcm_data, size_t pcm_len, int chunk_index)
{
    if (!handle || !pcm_data || pcm_len == 0) {
        ESP_LOGE(TAG, "Invalid chunk parameters");
        return false;
    }

    // Send raw binary PCM directly via WebSocket
    if (!s_ws_connected) {
        ESP_LOGW(TAG, "WebSocket not connected, cannot send chunk %d", chunk_index);
        return false;
    }

    // Enable receiving mode on FIRST chunk (proxy echoes immediately)
    if (chunk_index == 0 && !s_ws_receiving_audio) {
        ESP_LOGI(TAG, "First chunk - enabling audio receiving and starting playback buffer");
        s_received_audio_bytes = 0;
        s_ws_receiving_audio = true;

        // Start playback stream to receive echoed audio (but don't change state yet - still recording)
        if (!audio_playback_stream_start()) {
            ESP_LOGE(TAG, "Failed to start streaming playback");
            s_ws_receiving_audio = false;
            return false;
        }
    }

    esp_err_t err = ws_client_send_audio(pcm_data, pcm_len);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Chunk %d sent via WebSocket (%zu bytes PCM)", chunk_index, pcm_len);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to send chunk %d via WebSocket: %s", chunk_index, esp_err_to_name(err));
        return false;
    }
}

typedef struct {
    proxy_stream_ctx_t *stream_ctx;
    uint8_t *pcm_data;
    size_t pcm_len;
    int chunk_index;
    proxy_result_cb_t cb;
    void *user_ctx;
} proxy_stream_end_task_ctx_t;

static void proxy_stream_end_task(void *arg)
{
    proxy_stream_end_task_ctx_t *task_ctx = (proxy_stream_end_task_ctx_t *)arg;
    proxy_result_t result = PROXY_RESULT_FAILED;

    ESP_LOGI(TAG, "Sending final chunk %d via WebSocket (%zu bytes PCM)", task_ctx->chunk_index, task_ctx->pcm_len);

    // Check WebSocket connection
    if (!s_ws_connected) {
        ESP_LOGE(TAG, "WebSocket not connected, cannot send final chunk");
        goto cleanup;
    }

    // Playback and receiving should already be started from first chunk
    // NOTE: This code path is no longer used - keeping for compatibility
    ESP_LOGI(TAG, "Sending final chunk (playback already active)");

    // Send final chunk (raw binary PCM) - proxy will echo immediately
    esp_err_t err = ws_client_send_audio(task_ctx->pcm_data, task_ctx->pcm_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send final chunk via WebSocket: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG, "Final chunk sent, waiting for response");

    // Wait for audio to complete (with timeout)
    // TODO: Implement proper end-of-stream detection from proxy
    const int max_wait_seconds = 30;
    const int check_interval_ms = 100;
    int elapsed_ms = 0;
    size_t last_audio_bytes = 0;
    int idle_checks = 0;
    const int idle_threshold = 20;  // 2 seconds of no new audio = done

    while (elapsed_ms < (max_wait_seconds * 1000)) {
        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
        elapsed_ms += check_interval_ms;

        // Check if WebSocket disconnected during streaming
        if (!s_ws_connected) {
            ESP_LOGW(TAG, "WebSocket disconnected during stream end, aborting");
            result = PROXY_RESULT_FAILED;
            break;
        }

        // Check if we're still receiving audio
        size_t current_audio_bytes = s_received_audio_bytes;
        if (current_audio_bytes == last_audio_bytes) {
            idle_checks++;
            if (idle_checks >= idle_threshold) {
                ESP_LOGI(TAG, "No new audio for %d checks, assuming complete (received %zu bytes total)",
                         idle_checks, current_audio_bytes);
                break;
            }
        } else {
            idle_checks = 0;
            last_audio_bytes = current_audio_bytes;
        }
    }

    // End streaming playback
    audio_playback_stream_end();
    s_ws_receiving_audio = false;

    ESP_LOGI(TAG, "WebSocket audio streaming completed");
    result = PROXY_RESULT_OK;

cleanup:
    proxy_free(task_ctx->pcm_data);
    free(task_ctx->stream_ctx);

    if (task_ctx->cb) {
        task_ctx->cb(result, task_ctx->user_ctx);
    }

    free(task_ctx);
    vTaskDelete(NULL);
}

void proxy_stream_end(proxy_stream_handle_t handle, const uint8_t *pcm_data, size_t pcm_len,
                     int chunk_index, proxy_result_cb_t cb, void *user_ctx)
{
    if (!handle) {
        ESP_LOGE(TAG, "Invalid stream handle");
        if (cb) {
            cb(PROXY_RESULT_FAILED, user_ctx);
        }
        return;
    }

    proxy_stream_ctx_t *stream_ctx = (proxy_stream_ctx_t *)handle;

    // Allocate task context
    proxy_stream_end_task_ctx_t *task_ctx = malloc(sizeof(proxy_stream_end_task_ctx_t));
    if (!task_ctx) {
        ESP_LOGE(TAG, "Failed to allocate stream end task context");
        free(stream_ctx);
        if (cb) {
            cb(PROXY_RESULT_FAILED, user_ctx);
        }
        return;
    }

    // Copy PCM data for async processing (if any)
    if (pcm_len > 0) {
        task_ctx->pcm_data = proxy_alloc(pcm_len);
        if (!task_ctx->pcm_data) {
            ESP_LOGE(TAG, "Failed to allocate PCM data copy");
            free(task_ctx);
            free(stream_ctx);
            if (cb) {
                cb(PROXY_RESULT_FAILED, user_ctx);
            }
            return;
        }
        memcpy(task_ctx->pcm_data, pcm_data, pcm_len);
    } else {
        task_ctx->pcm_data = NULL;  // No data for empty final chunk
    }

    task_ctx->stream_ctx = stream_ctx;
    task_ctx->pcm_len = pcm_len;
    task_ctx->chunk_index = chunk_index;
    task_ctx->cb = cb;
    task_ctx->user_ctx = user_ctx;

    // Spawn task to handle final chunk and streaming response
    if (xTaskCreatePinnedToCore(proxy_stream_end_task, "proxy_stream_end", 24576, task_ctx, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start stream end task");
        proxy_free(task_ctx->pcm_data);
        free(task_ctx);
        free(stream_ctx);
        if (cb) {
            cb(PROXY_RESULT_FAILED, user_ctx);
        }
    }
}
