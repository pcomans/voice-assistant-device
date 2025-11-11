#include "proxy_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "smart_assistant.h"

static const char *TAG = "proxy_client";

#define PROXY_DEFAULT_URL   "http://192.168.7.75:8000/v1/audio"
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

void proxy_client_init(void)
{
    load_or_create_session_id();
    ESP_LOGI(TAG, "Proxy client initialised using %s (session: %s)", s_config.url, s_config.session_id);
    // TODO: load proxy URL/token from NVS, warm up TLS credentials.
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

    proxy_stream_ctx_t *ctx = (proxy_stream_ctx_t *)handle;

    // Base64 encode PCM data
    size_t b64_len = 0;
    size_t b64_buff_len = ((pcm_len + 2) / 3) * 4 + 1;
    char *b64_output = proxy_alloc(b64_buff_len);
    if (!b64_output) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer for chunk");
        return false;
    }

    int rc = mbedtls_base64_encode((unsigned char *)b64_output, b64_buff_len, &b64_len, pcm_data, pcm_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Base64 encode failed for chunk: %d", rc);
        proxy_free(b64_output);
        return false;
    }
    b64_output[b64_len] = '\0';

    // Build JSON payload
    const char *payload_fmt = "{"
                              "\"session_id\":\"%s\","
                              "\"chunk_index\":%d,"
                              "\"pcm_base64\":\"%s\","
                              "\"is_final\":false"
                              "}";
    const size_t payload_len = strlen(payload_fmt) + strlen(ctx->session_id) + 20 + b64_len + 1;
    char *payload = proxy_alloc(payload_len);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate JSON payload for chunk");
        proxy_free(b64_output);
        return false;
    }
    snprintf(payload, payload_len, payload_fmt, ctx->session_id, chunk_index, b64_output);
    proxy_free(b64_output);

    // Send chunk (quick POST, expect {"status": "partial"})
    esp_http_client_config_t config = {
        .url = s_config.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,  // Shorter timeout for non-final chunks
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for chunk");
        proxy_free(payload);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Assistant-Token", s_config.token);

    bool success = false;
    esp_err_t err = esp_http_client_open(client, strlen(payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for chunk: %s", esp_err_to_name(err));
    } else {
        int wlen = esp_http_client_write(client, payload, strlen(payload));
        if (wlen < 0) {
            ESP_LOGE(TAG, "Failed to write chunk request");
        } else {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                ESP_LOGD(TAG, "Chunk %d sent successfully (%zu bytes PCM)", chunk_index, pcm_len);
                success = true;
            } else {
                ESP_LOGW(TAG, "Chunk %d failed with status %d", chunk_index, status);
            }
        }
        esp_http_client_close(client);
    }

    esp_http_client_cleanup(client);
    proxy_free(payload);
    return success;
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

    // Base64 encode final chunk
    size_t b64_len = 0;
    size_t b64_buff_len = ((task_ctx->pcm_len + 2) / 3) * 4 + 1;
    char *b64_output = proxy_alloc(b64_buff_len);
    if (!b64_output) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer for final chunk");
        goto cleanup;
    }

    int rc = mbedtls_base64_encode((unsigned char *)b64_output, b64_buff_len, &b64_len,
                                  task_ctx->pcm_data, task_ctx->pcm_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Base64 encode failed for final chunk: %d", rc);
        proxy_free(b64_output);
        goto cleanup;
    }
    b64_output[b64_len] = '\0';

    // Build JSON payload
    const char *payload_fmt = "{"
                              "\"session_id\":\"%s\","
                              "\"chunk_index\":%d,"
                              "\"pcm_base64\":\"%s\","
                              "\"is_final\":true"
                              "}";
    const size_t payload_len = strlen(payload_fmt) + strlen(task_ctx->stream_ctx->session_id) + 20 + b64_len + 1;
    char *payload = proxy_alloc(payload_len);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate JSON payload for final chunk");
        proxy_free(b64_output);
        goto cleanup;
    }
    snprintf(payload, payload_len, payload_fmt, task_ctx->stream_ctx->session_id, task_ctx->chunk_index, b64_output);
    proxy_free(b64_output);

    ESP_LOGI(TAG, "Sending final chunk %d (%zu bytes PCM)", task_ctx->chunk_index, task_ctx->pcm_len);

    // Configure HTTP client for streaming response
    esp_http_client_config_t config = {
        .url = s_config.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,  // Longer timeout for streaming response
        .buffer_size = 4096,  // Read buffer for streaming
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for final chunk");
        proxy_free(payload);
        goto cleanup;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Assistant-Token", s_config.token);

    esp_err_t err = esp_http_client_open(client, strlen(payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for final chunk: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        proxy_free(payload);
        goto cleanup;
    }

    int wlen = esp_http_client_write(client, payload, strlen(payload));
    proxy_free(payload);
    if (wlen < 0) {
        ESP_LOGE(TAG, "Failed to write final chunk request");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto cleanup;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Final chunk response: status=%d", status);

    if (status != 200) {
        ESP_LOGW(TAG, "Final chunk failed with status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto cleanup;
    }

    // Start streaming playback - audio plays immediately as chunks arrive
    assistant_set_state(ASSISTANT_STATE_PLAYING);

    if (!audio_playback_stream_start()) {
        ESP_LOGE(TAG, "Failed to start streaming playback");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto cleanup;
    }

    // Read streaming response - raw binary PCM (no JSON, no base64!)
    // Read buffer for binary chunks (4KB)
    const size_t read_chunk_size = 4096;
    uint8_t *read_buffer = malloc(read_chunk_size);
    if (!read_buffer) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        audio_playback_stream_end();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto cleanup;
    }

    // Debug: Log response headers
    int content_len = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "Response headers: status=%d content_len=%d", status, content_len);

    char *transfer_encoding = NULL;
    esp_http_client_get_header(client, "Transfer-Encoding", &transfer_encoding);
    if (transfer_encoding) {
        ESP_LOGI(TAG, "Transfer-Encoding: %s", transfer_encoding);
    }

    ESP_LOGI(TAG, "Binary streaming started - reading raw PCM chunks");

    size_t total_audio_bytes = 0;
    int total_reads = 0;

    while (true) {
        // Read raw binary PCM chunk from HTTP response
        int read_len = esp_http_client_read_response(client, (char *)read_buffer, read_chunk_size);
        total_reads++;

        if (read_len < 0) {
            ESP_LOGE(TAG, "Failed to read streaming response");
            break;
        }
        if (read_len == 0) {
            ESP_LOGI(TAG, "End of binary stream after %d reads", total_reads);
            break;  // End of stream
        }

        // Stream raw PCM chunk directly to audio playback (no JSON, no base64!)
        if (audio_playback_stream_write(read_buffer, read_len)) {
            total_audio_bytes += read_len;
            if (total_reads % 10 == 0) {  // Log every 10th chunk to reduce spam
                ESP_LOGI(TAG, "Read %d: streamed %zu bytes total", total_reads, total_audio_bytes);
            }
        } else {
            ESP_LOGW(TAG, "Ring buffer full, dropped %d bytes", read_len);
        }

        vTaskDelay(1);  // Feed watchdog
    }

    free(read_buffer);

    // End streaming playback
    audio_playback_stream_end();

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_audio_bytes > 0) {
        ESP_LOGI(TAG, "Streamed %zu bytes of PCM audio (24kHz mono) with immediate playback", total_audio_bytes);
        result = PROXY_RESULT_OK;
    } else {
        ESP_LOGW(TAG, "No PCM audio received");
    }

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
