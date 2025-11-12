#include "audio_aec_reference.h"
#include "audio_resampler.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#include <string.h>

static const char *TAG = "aec_ref";

// Ring buffer for 16kHz reference samples
static RingbufHandle_t s_ref_buffer = NULL;
static int s_buffer_size = 0;

// Temporary buffer for downsampled audio
#define MAX_RESAMPLE_CHUNK 4096  // Max samples we can downsample at once
static int16_t s_downsample_buffer[MAX_RESAMPLE_CHUNK];

bool audio_aec_reference_init(int buffer_ms)
{
    if (s_ref_buffer != NULL) {
        ESP_LOGW(TAG, "Reference buffer already initialized");
        return true;
    }

    // Calculate buffer size: buffer_ms at 16kHz, 16-bit samples
    s_buffer_size = (16000 * buffer_ms / 1000) * sizeof(int16_t);

    ESP_LOGI(TAG, "Creating reference buffer: %d ms (%d bytes)", buffer_ms, s_buffer_size);

    // Create ring buffer (use BYTEBUF type for variable-size items)
    s_ref_buffer = xRingbufferCreate(s_buffer_size, RINGBUF_TYPE_BYTEBUF);
    if (!s_ref_buffer) {
        ESP_LOGE(TAG, "Failed to create reference ring buffer");
        return false;
    }

    ESP_LOGI(TAG, "Reference buffer initialized successfully");
    return true;
}

void audio_aec_reference_feed(const int16_t *pcm_24khz, size_t samples_24khz)
{
    if (!s_ref_buffer || !pcm_24khz || samples_24khz == 0) {
        return;
    }

    // Downsample from 24kHz to 16kHz
    size_t samples_16khz = audio_resample_calc_output_size(samples_24khz, 24000, 16000);

    // Limit to buffer size
    if (samples_16khz > MAX_RESAMPLE_CHUNK) {
        ESP_LOGW(TAG, "Reference chunk too large (%zu samples), truncating to %d",
                 samples_16khz, MAX_RESAMPLE_CHUNK);
        samples_24khz = audio_resample_calc_output_size(MAX_RESAMPLE_CHUNK, 16000, 24000);
        samples_16khz = MAX_RESAMPLE_CHUNK;
    }

    // Resample to 16kHz
    size_t actual_samples = audio_resample_linear(pcm_24khz, samples_24khz, 24000,
                                                  s_downsample_buffer, 16000);

    if (actual_samples == 0) {
        ESP_LOGW(TAG, "Resampling failed");
        return;
    }

    // Write to ring buffer (non-blocking - drop if full)
    BaseType_t ret = xRingbufferSend(s_ref_buffer, s_downsample_buffer,
                                     actual_samples * sizeof(int16_t), 0);
    if (ret != pdTRUE) {
        ESP_LOGD(TAG, "Reference buffer full, dropping %zu samples", actual_samples);
    }
}

bool audio_aec_reference_get(int16_t *output, size_t num_samples)
{
    if (!s_ref_buffer || !output || num_samples == 0) {
        // Fill with silence if no buffer
        if (output) {
            memset(output, 0, num_samples * sizeof(int16_t));
        }
        return false;
    }

    size_t bytes_needed = num_samples * sizeof(int16_t);
    size_t bytes_retrieved = 0;

    // Try to receive exact number of bytes needed
    size_t item_size = 0;
    int16_t *item = (int16_t *)xRingbufferReceiveUpTo(s_ref_buffer, &item_size, 0, bytes_needed);

    if (item && item_size > 0) {
        // Copy retrieved samples
        size_t samples_to_copy = item_size / sizeof(int16_t);
        if (samples_to_copy > num_samples) {
            samples_to_copy = num_samples;
        }
        memcpy(output, item, samples_to_copy * sizeof(int16_t));
        bytes_retrieved = samples_to_copy * sizeof(int16_t);

        // Return item to ring buffer
        vRingbufferReturnItem(s_ref_buffer, item);
    }

    // If we didn't get enough samples, fill remainder with silence
    if (bytes_retrieved < bytes_needed) {
        size_t silence_samples = (bytes_needed - bytes_retrieved) / sizeof(int16_t);
        memset(output + (bytes_retrieved / sizeof(int16_t)), 0, silence_samples * sizeof(int16_t));

        if (bytes_retrieved == 0) {
            // Complete silence - no playback happening
            return false;
        } else {
            // Partial data - buffer underrun
            ESP_LOGD(TAG, "Reference buffer underrun: got %zu/%zu samples",
                     bytes_retrieved / sizeof(int16_t), num_samples);
            return false;
        }
    }

    return true;
}

void audio_aec_reference_deinit(void)
{
    if (s_ref_buffer) {
        vRingbufferDelete(s_ref_buffer);
        s_ref_buffer = NULL;
    }
    ESP_LOGI(TAG, "Reference buffer deinitialized");
}
