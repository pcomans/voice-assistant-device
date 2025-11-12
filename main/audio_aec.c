#include "audio_aec.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "audio_aec";

// AFE instance and configuration
static esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static afe_config_t *s_afe_config = NULL;

// Audio processing parameters
static int s_feed_chunk_size = 0;  // Samples per channel required by AFE
static int s_fetch_chunk_size = 0; // Samples per channel output by AFE
static audio_aec_output_cb_t s_output_cb = NULL;
static void *s_user_ctx = NULL;

// Processing buffer (interleaved mic + ref)
static int16_t *s_feed_buffer = NULL;

// Queue for decoupling AFE fetch from WebSocket I/O
#define AUDIO_QUEUE_LENGTH 10
typedef struct {
    int16_t *data;
    size_t size;
} audio_chunk_t;
static QueueHandle_t s_audio_queue = NULL;

// Task handles
static TaskHandle_t s_fetch_task = NULL;
static TaskHandle_t s_output_task = NULL;
static bool s_running = false;

/**
 * @brief Output task - reads from queue and calls user callback (with blocking I/O)
 */
static void output_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Output task started on core %d", xPortGetCoreID());

    audio_chunk_t chunk;
    while (s_running) {
        // Wait for audio chunk from queue (blocking)
        if (xQueueReceive(s_audio_queue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Call user callback (may do blocking WebSocket I/O)
            if (s_output_cb && chunk.data && chunk.size > 0) {
                s_output_cb(chunk.data, chunk.size, s_user_ctx);
            }

            // Free the allocated chunk data
            if (chunk.data) {
                free(chunk.data);
            }
        }
    }

    // Drain remaining items from queue
    while (xQueueReceive(s_audio_queue, &chunk, 0) == pdTRUE) {
        if (chunk.data) {
            free(chunk.data);
        }
    }

    ESP_LOGI(TAG, "Output task exiting");
    s_output_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Fetch task - continuously drains AFE and pushes to queue (fast, no blocking I/O)
 */
static void fetch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Fetch task started on core %d", xPortGetCoreID());

    while (s_running) {
        // Fetch cleaned audio from AFE (blocks up to 2s waiting for data)
        afe_fetch_result_t *result = s_afe_handle->fetch(s_afe_data);

        if (result && result->data && result->data_size > 0) {
            // Allocate buffer and copy data for queue
            int16_t *chunk_data = heap_caps_malloc(result->data_size, MALLOC_CAP_INTERNAL);
            if (chunk_data) {
                memcpy(chunk_data, result->data, result->data_size);

                audio_chunk_t chunk = {
                    .data = chunk_data,
                    .size = result->data_size
                };

                // Push to queue (non-blocking, drop if full)
                if (xQueueSend(s_audio_queue, &chunk, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Audio queue full, dropping chunk");
                    free(chunk_data);
                }
            } else {
                ESP_LOGE(TAG, "Failed to allocate chunk buffer (%d bytes)", result->data_size);
            }

            // Log VAD state occasionally for debugging
            static int log_counter = 0;
            if (++log_counter >= 100) {
                ESP_LOGD(TAG, "VAD state: %d, volume: %.1f dB",
                         result->vad_state, result->data_volume);
                log_counter = 0;
            }
        }
    }

    ESP_LOGI(TAG, "Fetch task exiting");
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

bool audio_aec_init(audio_aec_output_cb_t output_cb, void *user_ctx)
{
    if (s_afe_data != NULL) {
        ESP_LOGW(TAG, "AEC already initialized");
        return true;
    }

    s_output_cb = output_cb;
    s_user_ctx = user_ctx;

    ESP_LOGI(TAG, "Initializing ESP-SR AFE with AEC...");

    // Initialize AFE configuration for voice communication (MR = Mic + Reference)
    // AFE_TYPE_VC includes AEC + NS + VAD optimized for voice communication
    s_afe_config = afe_config_init("MR", NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!s_afe_config) {
        ESP_LOGE(TAG, "Failed to create AFE config");
        return false;
    }

    // Customize AFE configuration for our use case
    s_afe_config->aec_init = true;  // Enable AEC (Acoustic Echo Cancellation)
    s_afe_config->se_init = false;  // Disable SE (Speech Enhancement / Beamforming) - only 1 mic
    s_afe_config->vad_init = true;  // Enable VAD (Voice Activity Detection)
    s_afe_config->ns_init = true;   // Enable NS (Noise Suppression)
    s_afe_config->agc_init = false; // Disable AGC - we control volume manually

    // Configure PCM settings (will be auto-filled by afe_config_init, but verify)
    ESP_LOGI(TAG, "PCM config: total_ch=%d, mic_num=%d, ref_num=%d, sample_rate=%d",
             s_afe_config->pcm_config.total_ch_num,
             s_afe_config->pcm_config.mic_num,
             s_afe_config->pcm_config.ref_num,
             s_afe_config->pcm_config.sample_rate);

    // Check configuration for conflicts
    s_afe_config = afe_config_check(s_afe_config);

    // Get AFE handle (function pointers for operations)
    s_afe_handle = esp_afe_handle_from_config(s_afe_config);
    if (!s_afe_handle) {
        ESP_LOGE(TAG, "Failed to get AFE handle");
        free(s_afe_config);
        s_afe_config = NULL;
        return false;
    }

    // Create AFE instance
    s_afe_data = s_afe_handle->create_from_config(s_afe_config);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "Failed to create AFE instance");
        free(s_afe_config);
        s_afe_config = NULL;
        s_afe_handle = NULL;
        return false;
    }

    // Query AFE parameters
    int total_feed_chunk = s_afe_handle->get_feed_chunksize(s_afe_data);  // Total samples across all channels
    s_fetch_chunk_size = s_afe_handle->get_fetch_chunksize(s_afe_data);
    int channel_num = s_afe_handle->get_channel_num(s_afe_data);
    int sample_rate = s_afe_handle->get_samp_rate(s_afe_data);

    // get_feed_chunksize() returns total interleaved samples, divide by channels to get per-channel
    s_feed_chunk_size = total_feed_chunk / channel_num;

    ESP_LOGI(TAG, "AFE parameters: total_feed_chunk=%d, per_channel=%d, fetch_chunk=%d, channels=%d, sample_rate=%d",
             total_feed_chunk, s_feed_chunk_size, s_fetch_chunk_size, channel_num, sample_rate);

    // Allocate feed buffer for interleaved audio (total samples = total_feed_chunk)
    size_t feed_buffer_size = total_feed_chunk * sizeof(int16_t);
    s_feed_buffer = heap_caps_malloc(feed_buffer_size, MALLOC_CAP_INTERNAL);
    if (!s_feed_buffer) {
        ESP_LOGE(TAG, "Failed to allocate feed buffer (%zu bytes)", feed_buffer_size);
        s_afe_handle->destroy(s_afe_data);
        free(s_afe_config);
        s_afe_data = NULL;
        s_afe_handle = NULL;
        s_afe_config = NULL;
        return false;
    }

    // Print AFE pipeline for debugging
    s_afe_handle->print_pipeline(s_afe_data);

    // Create audio queue for decoupling fetch from output
    s_audio_queue = xQueueCreate(AUDIO_QUEUE_LENGTH, sizeof(audio_chunk_t));
    if (!s_audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        free(s_feed_buffer);
        s_feed_buffer = NULL;
        s_afe_handle->destroy(s_afe_data);
        free(s_afe_config);
        s_afe_data = NULL;
        s_afe_handle = NULL;
        s_afe_config = NULL;
        return false;
    }

    // Start output task (handles user callback with blocking I/O)
    s_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        output_task,
        "aec_output",
        4096,           // Stack size (lower than fetch, just calls callback)
        NULL,
        4,              // Lower priority (can block on WebSocket I/O)
        &s_output_task,
        0               // Pin to core 0 (separate from fetch on core 1)
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create output task");
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
        free(s_feed_buffer);
        s_feed_buffer = NULL;
        s_afe_handle->destroy(s_afe_data);
        free(s_afe_config);
        s_afe_data = NULL;
        s_afe_handle = NULL;
        s_afe_config = NULL;
        s_running = false;
        return false;
    }

    // Start fetch task (continuously drains AFE to queue, no blocking I/O)
    ret = xTaskCreatePinnedToCore(
        fetch_task,
        "aec_fetch",
        8192,           // Stack size
        NULL,
        5,              // Higher than audio capture to drain AFE buffer quickly
        &s_fetch_task,
        1               // Pin to core 1 (same as audio capture)
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fetch task");
        s_running = false;
        // Wait for output task to exit
        vTaskDelay(pdMS_TO_TICKS(150));
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
        free(s_feed_buffer);
        s_feed_buffer = NULL;
        s_afe_handle->destroy(s_afe_data);
        free(s_afe_config);
        s_afe_data = NULL;
        s_afe_handle = NULL;
        s_afe_config = NULL;
        return false;
    }

    ESP_LOGI(TAG, "AEC initialized successfully (chunk size: %d samples/channel, %d total @ 16kHz)",
             s_feed_chunk_size, s_feed_chunk_size * 2);

    return true;
}

bool audio_aec_process(const int16_t *mic_samples, const int16_t *ref_samples, size_t num_samples)
{
    if (!s_afe_data || !s_afe_handle || !s_feed_buffer) {
        ESP_LOGW(TAG, "AEC not initialized");
        return false;
    }

    if (num_samples != (size_t)s_feed_chunk_size) {
        ESP_LOGW(TAG, "Invalid chunk size: expected %d per channel, got %zu", s_feed_chunk_size, num_samples);
        return false;
    }

    // Interleave mic and ref samples: [mic[0], ref[0], mic[1], ref[1], ...]
    for (size_t i = 0; i < num_samples; i++) {
        s_feed_buffer[i * 2 + 0] = mic_samples[i];
        s_feed_buffer[i * 2 + 1] = ref_samples[i];
    }

    // Feed interleaved data to AFE (num_samples per channel * 2 channels = total samples)
    int ret = s_afe_handle->feed(s_afe_data, s_feed_buffer);

    // The return value from feed() appears to vary - just check it's positive
    // AFE internally manages the data, return value doesn't always match input size
    if (ret <= 0) {
        ESP_LOGW(TAG, "AFE feed failed with return value: %d", ret);
        return false;
    }

    // Log occasionally for debugging (every 100 calls)
    static int feed_count = 0;
    if (++feed_count % 100 == 0) {
        ESP_LOGD(TAG, "AFE feed OK: fed %zu samples/channel, returned %d", num_samples, ret);
    }

    return true;
}

size_t audio_aec_get_chunk_size(void)
{
    return s_feed_chunk_size;
}

void audio_aec_deinit(void)
{
    if (!s_afe_data) {
        return;
    }

    ESP_LOGI(TAG, "Deinitializing AEC...");

    // Stop both tasks
    s_running = false;

    // Wait for fetch task to exit
    if (s_fetch_task) {
        int wait_count = 0;
        while (s_fetch_task && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        if (s_fetch_task) {
            vTaskDelete(s_fetch_task);
            s_fetch_task = NULL;
        }
    }

    // Wait for output task to exit
    if (s_output_task) {
        int wait_count = 0;
        while (s_output_task && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        if (s_output_task) {
            vTaskDelete(s_output_task);
            s_output_task = NULL;
        }
    }

    // Delete queue and free any remaining chunks
    if (s_audio_queue) {
        audio_chunk_t chunk;
        while (xQueueReceive(s_audio_queue, &chunk, 0) == pdTRUE) {
            if (chunk.data) {
                free(chunk.data);
            }
        }
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
    }

    // Free resources
    if (s_feed_buffer) {
        free(s_feed_buffer);
        s_feed_buffer = NULL;
    }

    if (s_afe_handle && s_afe_data) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
    }

    if (s_afe_config) {
        free(s_afe_config);
        s_afe_config = NULL;
    }

    s_afe_handle = NULL;
    s_output_cb = NULL;
    s_user_ctx = NULL;

    ESP_LOGI(TAG, "AEC deinitialized");
}
