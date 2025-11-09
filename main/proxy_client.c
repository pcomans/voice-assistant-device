#include "proxy_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback.h"
#include "cJSON.h"
#include "decoder/impl/esp_opus_dec.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "pcm_buffer.h"
#include "smart_assistant.h"

static const char *TAG = "proxy_client";

#define PROXY_DEFAULT_URL   "http://192.168.7.75:8000/v1/audio"
#define PROXY_DEFAULT_TOKEN "498b1b65-26a3-49e8-a55e-46a0b47365e2"

// NVS keys for persistent session ID
#define NVS_NAMESPACE       "proxy_client"
#define NVS_SESSION_ID_KEY  "session_id"

// Response buffer limits
#define MAX_RESPONSE_SIZE        (4 * 1024 * 1024)  // 4MB max response
#define INITIAL_RESPONSE_BUFFER  4096               // Initial buffer for unknown size
#define RESPONSE_GROW_THRESHOLD  1024               // Grow buffer when this much space left

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

    // Feed watchdog before parsing large JSON
    vTaskDelay(1);

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse proxy response JSON (first 200 chars): %.200s", body);
        return PROXY_RESULT_FAILED;
    }

    // Feed watchdog after parsing
    vTaskDelay(1);

    proxy_result_t result = PROXY_RESULT_OK;

    const cJSON *audio_b64 = cJSON_GetObjectItemCaseSensitive(root, "audio_base64");
    if (cJSON_IsString(audio_b64) && audio_b64->valuestring && audio_b64->valuestring[0] != '\0') {
        const char *audio_str = audio_b64->valuestring;
        size_t b64_len = strlen(audio_str);
        size_t opus_len = 0;

        // Decode base64 to get Opus-encoded audio
        uint8_t *opus_data = proxy_alloc(b64_len);
        if (!opus_data) {
            ESP_LOGE(TAG, "Failed to allocate buffer for base64 decode");
            result = PROXY_RESULT_FAILED;
        } else {
            int rc = mbedtls_base64_decode(opus_data, b64_len, &opus_len,
                                           (const unsigned char *)audio_str, b64_len);
            if (rc != 0) {
                ESP_LOGE(TAG, "Base64 decode failed: %d", rc);
                result = PROXY_RESULT_FAILED;
                proxy_free(opus_data);
            } else {
                ESP_LOGI(TAG, "Decoded %zu bytes of Opus audio data", opus_len);

                // Initialize Opus decoder (24kHz, mono, 20ms frames)
                esp_opus_dec_cfg_t opus_cfg = {
                    .sample_rate = 24000,  // OpenAI outputs 24kHz
                    .channel = 1,          // Mono
                    .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,  // Python encodes 20ms frames
                    .self_delimited = false,  // We manually parse RFC 6716 Appendix B length prefix
                };

                void *opus_handle = NULL;
                rc = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &opus_handle);
                if (rc != ESP_AUDIO_ERR_OK) {
                    ESP_LOGE(TAG, "Failed to open Opus decoder: %d", rc);
                    result = PROXY_RESULT_FAILED;
                    proxy_free(opus_data);
                } else {
                    // Start with initial PCM buffer estimate (opus_len * 10 is typical)
                    size_t pcm_capacity = opus_len * 10;
                    uint8_t *pcm_data = proxy_alloc(pcm_capacity);
                    if (!pcm_data) {
                        ESP_LOGE(TAG, "Failed to allocate PCM output buffer");
                        esp_opus_dec_close(opus_handle);
                        proxy_free(opus_data);
                        result = PROXY_RESULT_FAILED;
                    } else {
                        // Allocate frame buffer for decoder (reused for each frame)
                        size_t frame_buf_size = 24000 * 2 * 0.12;  // 24kHz * 16-bit * 120ms
                        uint8_t *frame_buf = proxy_alloc(frame_buf_size);
                        if (!frame_buf) {
                            ESP_LOGE(TAG, "Failed to allocate frame buffer");
                            proxy_free(pcm_data);
                            esp_opus_dec_close(opus_handle);
                            proxy_free(opus_data);
                            result = PROXY_RESULT_FAILED;
                        } else {
                            // Decode Opus to PCM
                            esp_audio_dec_in_raw_t raw = {
                                .buffer = opus_data,
                                .len = opus_len,
                                .consumed = 0,
                            };

                            esp_audio_dec_out_frame_t frame = {
                                .buffer = frame_buf,
                                .len = frame_buf_size,
                                .decoded_size = 0,
                            };

                            esp_audio_dec_info_t dec_info = {0};

                            // Decode all Opus frames, collecting into pcm_data
                            // Manually parse self-delimited frames (RFC 6716 Appendix B)
                            size_t total_pcm = 0;
                            int frame_count = 0;
                            size_t offset = 0;
                            ESP_LOGI(TAG, "Starting Opus decode loop with %zu bytes", opus_len);

                            while (offset < opus_len) {
                                vTaskDelay(1);  // Feed watchdog

                                // Parse self-delimited length prefix
                                if (offset >= opus_len) break;
                                uint8_t first_byte = opus_data[offset];
                                size_t frame_len;
                                size_t header_len;

                                if (first_byte < 252) {
                                    // Single byte length
                                    frame_len = first_byte;
                                    header_len = 1;
                                } else {
                                    // Two byte length: 252 + second byte
                                    if (offset + 1 >= opus_len) {
                                        ESP_LOGE(TAG, "Truncated length header at offset %zu", offset);
                                        break;
                                    }
                                    frame_len = 252 + opus_data[offset + 1];
                                    header_len = 2;
                                }

                                if (offset + header_len + frame_len > opus_len) {
                                    ESP_LOGE(TAG, "Frame %d extends beyond buffer: offset=%zu, header=%zu, frame=%zu, total=%zu",
                                             frame_count, offset, header_len, frame_len, opus_len);
                                    break;
                                }

                                // Setup decoder input for this frame (skip length prefix)
                                raw.buffer = opus_data + offset + header_len;
                                raw.len = frame_len;
                                raw.consumed = 0;

                                ESP_LOGI(TAG, "Frame %d: Calling decoder with offset=%zu, header=%zu, frame_len=%zu",
                                         frame_count, offset, header_len, frame_len);

                                rc = esp_opus_dec_decode(opus_handle, &raw, &frame, &dec_info);
                                ESP_LOGI(TAG, "Frame %d: rc=%d, decoded=%u, consumed=%u",
                                         frame_count, rc, frame.decoded_size, raw.consumed);

                                if (rc == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                                    // Need larger frame buffer
                                    ESP_LOGW(TAG, "Frame buffer too small, need %u bytes", frame.needed_size);
                                    uint8_t *new_frame_buf = proxy_alloc(frame.needed_size);
                                    if (!new_frame_buf) {
                                        ESP_LOGE(TAG, "Failed to reallocate frame buffer");
                                        break;
                                    }
                                    proxy_free(frame_buf);
                                    frame_buf = new_frame_buf;
                                    frame.buffer = frame_buf;
                                    frame.len = frame.needed_size;
                                    frame_buf_size = frame.needed_size;
                                    continue;
                                } else if (rc != ESP_AUDIO_ERR_OK) {
                                    ESP_LOGE(TAG, "Opus decode failed: %d", rc);
                                    break;
                                }

                                if (frame.decoded_size == 0) {
                                    ESP_LOGW(TAG, "Decoded 0 bytes, stopping decode loop");
                                    break;
                                }

                                // Check if we need to grow the output buffer
                                if (total_pcm + frame.decoded_size > pcm_capacity) {
                                    size_t new_capacity = pcm_capacity * 2;
                                    uint8_t *new_buf = proxy_alloc(new_capacity);
                                    if (!new_buf) {
                                        ESP_LOGE(TAG, "Failed to reallocate PCM buffer to %zu bytes", new_capacity);
                                        break;
                                    }
                                    memcpy(new_buf, pcm_data, total_pcm);
                                    proxy_free(pcm_data);
                                    pcm_data = new_buf;
                                    pcm_capacity = new_capacity;
                                    ESP_LOGD(TAG, "Expanded PCM buffer to %zu bytes", new_capacity);
                                }

                                // Copy this frame's decoded data to output buffer
                                memcpy(pcm_data + total_pcm, frame.buffer, frame.decoded_size);
                                total_pcm += frame.decoded_size;
                                frame_count++;

                                // Move to next self-delimited frame
                                offset += header_len + frame_len;

                                // Reset for next frame
                                frame.decoded_size = 0;
                            }

                            ESP_LOGI(TAG, "Decode loop complete: %d frames, %zu total PCM bytes", frame_count, total_pcm);

                            proxy_free(frame_buf);

                            if (total_pcm > 0) {
                                ESP_LOGI(TAG, "Decoded %zu bytes of PCM audio (24kHz mono)", total_pcm);
                                assistant_set_state(ASSISTANT_STATE_PLAYING);
                                audio_playback_play_pcm(pcm_data, total_pcm);
                                result = PROXY_RESULT_OK;
                            } else {
                                ESP_LOGE(TAG, "No PCM audio decoded");
                                result = PROXY_RESULT_FAILED;
                            }

                            proxy_free(pcm_data);
                        }
                    }

                    esp_opus_dec_close(opus_handle);
                    proxy_free(opus_data);
                }
            }
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

    // Increase stack size to 24KB to handle Opus decoder (ESP audio codec docs recommend 20KB for decoders)
    // Pin to core 0 to avoid contention with audio playback on core 1
    if (xTaskCreatePinnedToCore(proxy_upload_task, "proxy_upload", 24576, task_ctx, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start proxy upload task");
        free(task_ctx);
        if (cb) {
            cb(PROXY_RESULT_FAILED, user_ctx);
        }
    }
}

// ==================== Chunked Streaming API (Option C) ====================

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

    // Read streaming response (newline-delimited JSON) - read in chunks for efficiency
    // Audio delta lines can be 20KB+ (16KB base64 + JSON overhead)
    const size_t line_buffer_size = 24576;  // 24KB for large audio deltas
    char *line_buffer = malloc(line_buffer_size);
    if (!line_buffer) {
        ESP_LOGE(TAG, "Failed to allocate line buffer");
        audio_playback_stream_end();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto cleanup;
    }
    size_t line_pos = 0;
    bool response_complete = false;
    size_t total_audio_bytes = 0;

    // Read buffer for chunked reading (4KB)
    const size_t read_chunk_size = 4096;
    char *read_buffer = malloc(read_chunk_size);
    if (!read_buffer) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        free(line_buffer);
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

    ESP_LOGI(TAG, "Streaming response started (immediate playback)");

    int total_reads = 0;
    while (!response_complete) {
        // Read chunk from HTTP response
        int read_len = esp_http_client_read_response(client, read_buffer, read_chunk_size);
        total_reads++;

        ESP_LOGI(TAG, "Read attempt %d: got %d bytes", total_reads, read_len);

        if (read_len < 0) {
            ESP_LOGE(TAG, "Failed to read streaming response");
            break;
        }
        if (read_len == 0) {
            ESP_LOGI(TAG, "End of stream after %d reads", total_reads);
            break;  // End of stream
        }

        // Process each character in the chunk
        for (int i = 0; i < read_len && !response_complete; i++) {
            char ch = read_buffer[i];

            if (ch == '\n' && line_pos > 0) {
                // Parse JSON line
                line_buffer[line_pos] = '\0';

                ESP_LOGI(TAG, "Parsing JSON line (%zu bytes): %.100s%s",
                         line_pos, line_buffer, line_pos > 100 ? "..." : "");

                cJSON *json = cJSON_Parse(line_buffer);
                if (!json) {
                    ESP_LOGE(TAG, "JSON parse failed for line: %s", line_buffer);
                }
                if (json) {
                    // Check for completion
                    const cJSON *status_obj = cJSON_GetObjectItemCaseSensitive(json, "status");
                    if (cJSON_IsString(status_obj)) {
                        const char *status_str = status_obj->valuestring;
                        if (strcmp(status_str, "complete") == 0) {
                            ESP_LOGI(TAG, "Response complete");
                            response_complete = true;
                        }
                    }

                    // Decode and play PCM delta immediately
                    const cJSON *audio_delta = cJSON_GetObjectItemCaseSensitive(json, "audio_delta");
                    if (cJSON_IsString(audio_delta) && audio_delta->valuestring) {
                        const char *b64_str = audio_delta->valuestring;
                        size_t b64_str_len = strlen(b64_str);

                        ESP_LOGI(TAG, "Found audio_delta: %zu bytes (base64)", b64_str_len);

                        // Decode base64 PCM chunk
                        size_t decode_len = 0;
                        size_t decode_buf_len = (b64_str_len * 3) / 4 + 4;
                        uint8_t *decode_buf = proxy_alloc(decode_buf_len);

                        if (decode_buf) {
                            int decode_rc = mbedtls_base64_decode(decode_buf, decode_buf_len, &decode_len,
                                                                  (const unsigned char *)b64_str, b64_str_len);
                            if (decode_rc == 0 && decode_len > 0) {
                                // Stream chunk immediately to audio playback (low latency!)
                                if (audio_playback_stream_write(decode_buf, decode_len)) {
                                    total_audio_bytes += decode_len;
                                    ESP_LOGI(TAG, "Streamed PCM chunk: %zu bytes (total: %zu)", decode_len, total_audio_bytes);
                                } else {
                                    ESP_LOGE(TAG, "Failed to stream audio chunk");
                                }
                            } else {
                                ESP_LOGE(TAG, "Base64 decode failed: rc=%d len=%zu", decode_rc, decode_len);
                            }
                            proxy_free(decode_buf);
                        }
                    }

                    cJSON_Delete(json);
                }

                line_pos = 0;
            } else if (ch != '\n' && ch != '\r' && line_pos < line_buffer_size - 1) {
                line_buffer[line_pos++] = ch;
            }
        }

        vTaskDelay(1);  // Feed watchdog
    }

    free(read_buffer);
    free(line_buffer);

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
