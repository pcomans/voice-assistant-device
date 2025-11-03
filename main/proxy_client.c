#include "proxy_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "pcm_buffer.h"
#include "smart_assistant.h"

static const char *TAG = "proxy_client";

#define PROXY_DEFAULT_URL   "http://192.168.7.75:8000/v1/audio"
#define PROXY_DEFAULT_TOKEN "498b1b65-26a3-49e8-a55e-46a0b47365e2"

// Response buffer limits
#define MAX_RESPONSE_SIZE        (4 * 1024 * 1024)  // 4MB max response
#define INITIAL_RESPONSE_BUFFER  4096               // Initial buffer for unknown size
#define RESPONSE_GROW_THRESHOLD  1024               // Grow buffer when this much space left

typedef struct {
    char url[128];
    char token[64];
} proxy_config_t;

static proxy_config_t s_config = {
    .url = PROXY_DEFAULT_URL,
    .token = PROXY_DEFAULT_TOKEN,
};

typedef struct {
    proxy_result_cb_t cb;
    void *ctx;
} proxy_task_ctx_t;

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

void proxy_client_init(void)
{
    ESP_LOGI(TAG, "Proxy client initialised using %s", s_config.url);
    // TODO: load proxy URL/token from NVS, warm up TLS credentials.
}

// Helper function to read HTTP response body into a dynamically sized buffer
static char *read_response_body(esp_http_client_handle_t client, int64_t content_length, size_t *out_len)
{
    size_t response_len = 0;
    char *response_body = NULL;
    size_t capacity;

    // Validate content_length if provided
    if (content_length >= MAX_RESPONSE_SIZE) {
        ESP_LOGE(TAG, "Response content-length %lld exceeds maximum %u",
                 (long long)content_length, MAX_RESPONSE_SIZE);
        return NULL;
    }

    // Allocate initial buffer based on whether we know the content length
    if (content_length > 0) {
        capacity = (size_t)content_length + 1;
    } else {
        capacity = INITIAL_RESPONSE_BUFFER;
    }

    response_body = proxy_alloc(capacity);
    if (!response_body) {
        ESP_LOGE(TAG, "Failed to allocate response buffer (%u bytes)", (unsigned int)capacity);
        return NULL;
    }

    // Read response in chunks
    while (true) {
        // Check if we know the size and have read everything
        if (content_length > 0 && response_len >= (size_t)content_length) {
            break;
        }

        // Grow buffer if needed (for unknown content length)
        if (capacity - response_len <= RESPONSE_GROW_THRESHOLD) {
            size_t new_capacity = capacity * 2;
            if (new_capacity > MAX_RESPONSE_SIZE) {
                ESP_LOGE(TAG, "Response exceeds maximum size");
                proxy_free(response_body);
                return NULL;
            }
            char *new_buf = proxy_alloc(new_capacity);
            if (!new_buf) {
                ESP_LOGE(TAG, "Failed to grow response buffer");
                proxy_free(response_body);
                return NULL;
            }
            memcpy(new_buf, response_body, response_len);
            proxy_free(response_body);
            response_body = new_buf;
            capacity = new_capacity;
        }

        // Read next chunk
        int read_len = esp_http_client_read_response(client,
                                                     response_body + response_len,
                                                     capacity - response_len - 1);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Failed to read proxy response chunk");
            proxy_free(response_body);
            return NULL;
        }
        if (read_len == 0) {
            break;  // End of response
        }
        response_len += (size_t)read_len;

        // Yield to prevent watchdog timeout on large responses
        vTaskDelay(1);
    }

    response_body[response_len] = '\0';
    *out_len = response_len;
    return response_body;
}

static proxy_result_t handle_response_body(const char *body)
{
    if (!body) {
        return PROXY_RESULT_FAILED;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse proxy response JSON: %s", body);
        return PROXY_RESULT_FAILED;
    }

    proxy_result_t result = PROXY_RESULT_OK;

    const cJSON *transcript = cJSON_GetObjectItemCaseSensitive(root, "transcript");
    if (cJSON_IsString(transcript) && transcript->valuestring) {
        ESP_LOGI(TAG, "Proxy transcript: %s", transcript->valuestring);
    }

    const cJSON *audio_b64 = cJSON_GetObjectItemCaseSensitive(root, "audio_base64");
    if (cJSON_IsString(audio_b64) && audio_b64->valuestring && audio_b64->valuestring[0] != '\0') {
        const char *audio_str = audio_b64->valuestring;
        size_t input_len = strlen(audio_str);
        size_t decoded_len = 0;

        uint8_t *decoded = proxy_alloc(input_len);
        if (!decoded) {
            ESP_LOGE(TAG, "Failed to allocate buffer for audio decode");
            result = PROXY_RESULT_FAILED;
        } else {
            int rc = mbedtls_base64_decode(decoded, input_len, &decoded_len,
                                           (const unsigned char *)audio_str, input_len);
            if (rc != 0) {
                ESP_LOGE(TAG, "Audio base64 decode failed: %d", rc);
                result = PROXY_RESULT_FAILED;
            } else {
                assistant_set_state(ASSISTANT_STATE_PLAYING);
                audio_playback_play_pcm(decoded, decoded_len);
                result = PROXY_RESULT_OK;
            }
            proxy_free(decoded);
        }
    } else {
        ESP_LOGW(TAG, "Proxy response missing audio payload");
        result = PROXY_RESULT_FAILED;
    }

    cJSON_Delete(root);
    return result;
}

static proxy_result_t perform_upload(void)
{
    const size_t bytes_available = pcm_buffer_size();
    if (bytes_available == 0) {
        ESP_LOGW(TAG, "No PCM data to upload");
        return PROXY_RESULT_FAILED;
    }

    ESP_LOGI(TAG, "Preparing upload of %u bytes (max buffer %u)", (unsigned int)bytes_available,
             (unsigned int)pcm_buffer_capacity());

    uint8_t *pcm_data = proxy_alloc(bytes_available);
    if (!pcm_data) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for PCM copy", (unsigned int)bytes_available);
        return PROXY_RESULT_FAILED;
    }

    size_t bytes_copied = pcm_buffer_pop(pcm_data, bytes_available);
    if (bytes_copied == 0) {
        ESP_LOGW(TAG, "Ring buffer produced zero bytes");
        proxy_free(pcm_data);
        return PROXY_RESULT_FAILED;
    }

    size_t b64_len = 0;
    size_t b64_buff_len = ((bytes_copied + 2) / 3) * 4 + 1;
    char *b64_output = proxy_alloc(b64_buff_len);
    if (!b64_output) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer");
        proxy_free(pcm_data);
        return PROXY_RESULT_FAILED;
    }

    int rc = mbedtls_base64_encode((unsigned char *)b64_output, b64_buff_len, &b64_len, pcm_data, bytes_copied);
    proxy_free(pcm_data);
    if (rc != 0) {
        ESP_LOGE(TAG, "Base64 encode failed: %d", rc);
        proxy_free(b64_output);
        return PROXY_RESULT_FAILED;
    }
    b64_output[b64_len] = '\0';

    char session_id[32];
    snprintf(session_id, sizeof(session_id), "%08lx", (unsigned long)esp_random());

    const char *payload_fmt = "{"
                              "\"session_id\":\"%s\","
                              "\"chunk_index\":0,"
                              "\"pcm_base64\":\"%s\","
                              "\"is_final\":true"
                              "}";
    // Calculate exact payload size: format string - 4 (for two %s) + actual string lengths + null terminator
    const size_t payload_len = strlen(payload_fmt) - 4 + strlen(session_id) + b64_len + 1;
    char *payload = proxy_alloc(payload_len);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate JSON payload");
        proxy_free(b64_output);
        return PROXY_RESULT_FAILED;
    }
    snprintf(payload, payload_len, payload_fmt, session_id, b64_output);
    proxy_free(b64_output);

    ESP_LOGI(TAG, "Posting session %s (%u bytes JSON)", session_id, (unsigned int)strlen(payload));

    esp_http_client_config_t config = {
        .url = s_config.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        proxy_free(payload);
        return PROXY_RESULT_FAILED;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Assistant-Token", s_config.token);

    proxy_result_t result = PROXY_RESULT_FAILED;
    esp_err_t err = esp_http_client_open(client, strlen(payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        proxy_free(payload);
        return PROXY_RESULT_RETRY;
    }

    int wlen = esp_http_client_write(client, payload, strlen(payload));
    if (wlen < 0) {
        ESP_LOGE(TAG, "Failed to write request body");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        proxy_free(payload);
        return PROXY_RESULT_RETRY;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP POST completed with status %d (content_length=%lld)", status, content_length);

    if (status == 200) {
        size_t response_len = 0;
        char *response_body = read_response_body(client, content_length, &response_len);

        if (!response_body) {
            result = PROXY_RESULT_FAILED;
        } else if (response_len == 0) {
            ESP_LOGW(TAG, "Proxy returned empty response (claimed Content-Length: %lld)",
                     (long long)content_length);
            result = PROXY_RESULT_FAILED;
            proxy_free(response_body);
        } else {
            ESP_LOGI(TAG, "Proxy response (%u bytes): %.120s%s",
                     (unsigned int)response_len,
                     response_body,
                     response_len > 120 ? "..." : "");
            result = handle_response_body(response_body);
            proxy_free(response_body);
        }
    } else if (status >= 500) {
        result = PROXY_RESULT_RETRY;
    } else {
        ESP_LOGW(TAG, "Proxy returned status %d", status);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    proxy_free(payload);
    ESP_LOGI(TAG, "Upload finished with result %d", result);
    return result;
}

static void proxy_upload_task(void *arg)
{
    proxy_task_ctx_t ctx = *(proxy_task_ctx_t *)arg;
    free(arg);

    proxy_result_t result = perform_upload();

    if (ctx.cb) {
        ctx.cb(result, ctx.ctx);
    }

    vTaskDelete(NULL);
}

void proxy_send_recording(proxy_result_cb_t cb, void *user_ctx)
{
    proxy_task_ctx_t *task_ctx = malloc(sizeof(proxy_task_ctx_t));
    if (!task_ctx) {
        ESP_LOGE(TAG, "Failed to allocate task context");
        if (cb) {
            cb(PROXY_RESULT_FAILED, user_ctx);
        }
        return;
    }
    task_ctx->cb = cb;
    task_ctx->ctx = user_ctx;

    // Increase stack size to 8KB to handle large allocations
    // Pin to core 0 to avoid contention with audio playback on core 1
    if (xTaskCreatePinnedToCore(proxy_upload_task, "proxy_upload", 8192, task_ctx, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start proxy upload task");
        free(task_ctx);
        if (cb) {
            cb(PROXY_RESULT_FAILED, user_ctx);
        }
    }
}
