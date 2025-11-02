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

#define PROXY_DEFAULT_URL   "http://smart-assistant.local:8000/v1/audio"
#define PROXY_DEFAULT_TOKEN "498b1b65-26a3-49e8-a55e-46a0b47365e2"

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

void proxy_client_init(void)
{
    ESP_LOGI(TAG, "Proxy client initialised using %s", s_config.url);
    // TODO: load proxy URL/token from NVS, warm up TLS credentials.
}

static proxy_result_t handle_response_body(const char *body)
{
    if (!body) {
        return PROXY_RESULT_FAILED;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse proxy response JSON");
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

        uint8_t *decoded = malloc(input_len);
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
            free(decoded);
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

    // Try SPIRAM first, fall back to internal RAM
    uint8_t *pcm_data = heap_caps_malloc(bytes_available, MALLOC_CAP_SPIRAM);
    if (!pcm_data) {
        pcm_data = heap_caps_malloc(bytes_available, MALLOC_CAP_INTERNAL);
    }
    if (!pcm_data) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for PCM copy", (unsigned int)bytes_available);
        return PROXY_RESULT_FAILED;
    }

    size_t bytes_copied = pcm_buffer_pop(pcm_data, bytes_available);
    if (bytes_copied == 0) {
        ESP_LOGW(TAG, "Ring buffer produced zero bytes");
        free(pcm_data);
        return PROXY_RESULT_FAILED;
    }

    size_t b64_len = 0;
    size_t b64_buff_len = ((bytes_copied + 2) / 3) * 4 + 1;
    char *b64_output = malloc(b64_buff_len);
    if (!b64_output) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer");
        free(pcm_data);
        return PROXY_RESULT_FAILED;
    }

    int rc = mbedtls_base64_encode((unsigned char *)b64_output, b64_buff_len, &b64_len, pcm_data, bytes_copied);
    free(pcm_data);
    if (rc != 0) {
        ESP_LOGE(TAG, "Base64 encode failed: %d", rc);
        free(b64_output);
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
    const size_t payload_len = strlen(payload_fmt) + strlen(session_id) + b64_len + 16;
    char *payload = malloc(payload_len);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate JSON payload");
        free(b64_output);
        return PROXY_RESULT_FAILED;
    }
    snprintf(payload, payload_len, payload_fmt, session_id, b64_output);
    free(b64_output);

    esp_http_client_config_t config = {
        .url = s_config.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(payload);
        return PROXY_RESULT_FAILED;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Assistant-Token", s_config.token);
    esp_http_client_set_post_field(client, payload, strlen(payload));

    proxy_result_t result = PROXY_RESULT_FAILED;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            size_t resp_capacity = 0;
            int64_t content_length = esp_http_client_get_content_length(client);
            if (content_length > 0 && content_length < 4096) {
                resp_capacity = (size_t)content_length + 1;
            } else {
                resp_capacity = 4096;
            }

            char *response_body = malloc(resp_capacity);
            if (!response_body) {
                ESP_LOGE(TAG, "Failed to allocate response buffer");
                result = PROXY_RESULT_FAILED;
            } else {
                int read_len = esp_http_client_read_response(client, response_body, resp_capacity - 1);
                if (read_len < 0) {
                    ESP_LOGE(TAG, "Failed to read proxy response");
                    result = PROXY_RESULT_FAILED;
                } else {
                    response_body[read_len] = '\0';
                    result = handle_response_body(response_body);
                }
                free(response_body);
            }
        } else if (status >= 500) {
            result = PROXY_RESULT_RETRY;
        } else {
            ESP_LOGW(TAG, "Proxy returned status %d", status);
        }
    } else {
        ESP_LOGE(TAG, "HTTP perform error: %s", esp_err_to_name(err));
        result = PROXY_RESULT_RETRY;
    }

    esp_http_client_cleanup(client);
    free(payload);
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
    if (xTaskCreatePinnedToCore(proxy_upload_task, "proxy_upload", 8192, task_ctx, 4, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start proxy upload task");
        free(task_ctx);
        if (cb) {
            cb(PROXY_RESULT_FAILED, user_ctx);
        }
    }
}
