#include "audio_controller.h"
#include "smart_assistant.h"
#include "audio_aec.h"
#include "audio_aec_reference.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_SAMPLE_RATE_HZ     16000
#define AUDIO_BITS_PER_SAMPLE    I2S_DATA_BIT_WIDTH_32BIT  // MEMS mic outputs 32-bit I2S data
#define AUDIO_CHANNEL_COUNT      1
#define AUDIO_FRAME_SAMPLES      256

// AEC configuration
#define AEC_ENABLE               0     // Disable AEC - bypass AFE entirely for debugging

static const char *TAG = "audio_ctrl";

static TaskHandle_t streaming_capture_task_handle = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static audio_capture_chunk_cb_t s_chunk_cb = NULL;
static void *s_chunk_ctx = NULL;
static bool s_aec_initialized = false;

static esp_err_t configure_i2s(void)
{
    if (s_rx_chan) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %d", ret);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(AUDIO_BITS_PER_SAMPLE, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = GPIO_NUM_15,
            .ws   = GPIO_NUM_2,
            .dout = GPIO_NUM_NC,
            .din  = GPIO_NUM_39,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %d", ret);
        return ret;
    }

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "I2S channel enabled successfully");
    return ESP_OK;
}

/**
 * @brief Callback for cleaned audio from AEC (16kHz)
 *
 * Receives cleaned audio from AFE and forwards directly to proxy.
 * Proxy handles resampling to 24kHz for OpenAI.
 */
static void cleaned_audio_callback(const int16_t *pcm_16khz, size_t samples_16khz, void *ctx)
{
    (void)ctx;

    if (!s_chunk_cb || samples_16khz == 0) {
        return;
    }

    // Forward 16kHz cleaned audio directly to proxy (no resampling needed)
    size_t bytes = samples_16khz * sizeof(int16_t);
    s_chunk_cb((const uint8_t *)pcm_16khz, bytes, s_chunk_ctx);
}

void audio_controller_init(void)
{
    esp_err_t err = configure_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S configuration failed: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Audio controller initialised (I2S channel: %p)", (void*)s_rx_chan);

#if AEC_ENABLE
    // Initialize AEC reference buffer (500ms buffer for playback reference)
    if (!audio_aec_reference_init(500)) {
        ESP_LOGE(TAG, "Failed to initialize AEC reference buffer");
        return;
    }

    // Initialize AEC with cleaned audio callback
    if (!audio_aec_init(cleaned_audio_callback, NULL)) {
        ESP_LOGE(TAG, "Failed to initialize AEC");
        return;
    }

    s_aec_initialized = true;
    size_t chunk_size = audio_aec_get_chunk_size();
    ESP_LOGI(TAG, "AEC initialized (chunk size: %zu samples/channel, %.1f ms @ 16kHz)",
             chunk_size, (chunk_size * 1000.0f) / 16000.0f);
#endif
}

static void streaming_capture_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Streaming capture task started");

    if (!s_rx_chan) {
        ESP_LOGE(TAG, "I2S channel is NULL!");
        streaming_capture_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

#if AEC_ENABLE
    // Get AEC chunk size (typically 512 samples @ 16kHz = 32ms chunks)
    size_t aec_chunk_size = s_aec_initialized ? audio_aec_get_chunk_size() : 0;
    if (aec_chunk_size == 0) {
        ESP_LOGE(TAG, "AEC not initialized or invalid chunk size");
        streaming_capture_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Using AEC chunk size: %zu samples/channel (%.1f ms @ 16kHz)",
             aec_chunk_size, (aec_chunk_size * 1000.0f) / 16000.0f);

    // Allocate buffers using regular malloc (defaults to internal RAM)
    int32_t *i2s_buffer = malloc(AUDIO_FRAME_SAMPLES * sizeof(int32_t));
    int16_t *mic_buffer = malloc(aec_chunk_size * sizeof(int16_t));
    int16_t *ref_buffer = malloc(aec_chunk_size * sizeof(int16_t));

    if (!i2s_buffer || !mic_buffer || !ref_buffer) {
        ESP_LOGE(TAG, "Failed to allocate streaming buffers");
        if (i2s_buffer) free(i2s_buffer);
        if (mic_buffer) free(mic_buffer);
        if (ref_buffer) free(ref_buffer);
        streaming_capture_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    size_t samples_in_chunk = 0;
    int chunks_processed = 0;

    // Stream continuously until task is stopped
    while (streaming_capture_task_handle != NULL) {
        // Read I2S data in smaller frames
        size_t bytes_read = 0;
        size_t i2s_bytes = AUDIO_FRAME_SAMPLES * sizeof(int32_t);
        esp_err_t err = i2s_channel_read(s_rx_chan, i2s_buffer, i2s_bytes, &bytes_read, portMAX_DELAY);

        if (err != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TAG, "I2S read error or empty: %d", err);
            continue;
        }

        // Convert 32-bit I2S to 16-bit PCM
        size_t samples_read = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < samples_read; i++) {
            if (samples_in_chunk < aec_chunk_size) {
                mic_buffer[samples_in_chunk++] = (int16_t)(i2s_buffer[i] >> 14);
            }
        }

        // When chunk is full, process through AEC
        if (samples_in_chunk >= aec_chunk_size) {
            // Debug: Check raw mic level before AEC every 100 chunks
            static int mic_debug_counter = 0;
            if (mic_debug_counter++ % 100 == 0) {
                int32_t sum = 0;
                for (size_t i = 0; i < aec_chunk_size; i++) {
                    sum += abs(mic_buffer[i]);
                }
                int16_t avg = sum / aec_chunk_size;
                ESP_LOGI(TAG, "Raw mic BEFORE AEC: avg=%d", avg);
            }

            // Get reference samples (playback audio for echo cancellation)
            audio_aec_reference_get(ref_buffer, aec_chunk_size);

            // Process through AEC (cleaned audio will be sent via callback)
            if (!audio_aec_process(mic_buffer, ref_buffer, aec_chunk_size)) {
                ESP_LOGW(TAG, "AEC processing failed");
            }

            samples_in_chunk = 0;  // Reset for next chunk
            chunks_processed++;

            // Yield to prevent watchdog timeout (AEC processing is CPU intensive)
            taskYIELD();
        }
    }

    ESP_LOGI(TAG, "Streaming capture task exit (chunks processed: %d)", chunks_processed);

    free(i2s_buffer);
    free(mic_buffer);
    free(ref_buffer);

#else
    // Non-AEC path: Raw microphone with manual gain
    const size_t chunk_samples = 1600;  // 100ms @ 16kHz
    const size_t chunk_bytes = chunk_samples * sizeof(int16_t);
    const int manual_gain = 10;  // 10x gain to boost weak microphone signal

    int32_t *i2s_buffer = malloc(chunk_samples * sizeof(int32_t));
    int16_t *pcm_chunk = malloc(chunk_bytes);

    if (!i2s_buffer || !pcm_chunk) {
        ESP_LOGE(TAG, "Failed to allocate streaming buffers");
        if (i2s_buffer) free(i2s_buffer);
        if (pcm_chunk) free(pcm_chunk);
        streaming_capture_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Non-AEC mode: Using raw microphone with %dx manual gain", manual_gain);

    size_t samples_in_chunk = 0;
    int debug_counter = 0;

    while (streaming_capture_task_handle != NULL) {
        size_t bytes_read = 0;
        size_t i2s_bytes = AUDIO_FRAME_SAMPLES * sizeof(int32_t);
        esp_err_t err = i2s_channel_read(s_rx_chan, i2s_buffer, i2s_bytes, &bytes_read, portMAX_DELAY);

        if (err != ESP_OK || bytes_read == 0) {
            continue;
        }

        size_t samples_read = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < samples_read; i++) {
            if (samples_in_chunk < chunk_samples) {
                // Apply manual gain with clipping protection
                int32_t sample = (int32_t)(i2s_buffer[i] >> 14) * manual_gain;
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                pcm_chunk[samples_in_chunk++] = (int16_t)sample;
            }
        }

        if (samples_in_chunk >= chunk_samples) {
            // Debug: Log audio level every 100 chunks
            if (debug_counter++ % 100 == 0) {
                int32_t sum = 0;
                for (size_t i = 0; i < chunk_samples; i++) {
                    sum += abs(pcm_chunk[i]);
                }
                int16_t avg = sum / chunk_samples;
                ESP_LOGI(TAG, "Raw mic with %dx gain: avg=%d", manual_gain, avg);
            }

            if (s_chunk_cb) {
                s_chunk_cb((const uint8_t *)pcm_chunk, chunk_bytes, s_chunk_ctx);
            }
            samples_in_chunk = 0;
        }
    }

    free(i2s_buffer);
    free(pcm_chunk);
#endif

    streaming_capture_task_handle = NULL;
    vTaskDelete(NULL);
}

void audio_start_streaming_capture(audio_capture_chunk_cb_t chunk_cb, void *ctx)
{
    if (streaming_capture_task_handle) {
        ESP_LOGW(TAG, "Streaming capture already running");
        return;
    }

    s_chunk_cb = chunk_cb;
    s_chunk_ctx = ctx;

    ESP_LOGI(TAG, "Starting streaming audio capture (100ms chunks)");
    // Increased stack size for AEC processing (needs ~3KB for buffers + AEC overhead)
    // Pin to core 1 (same as fetch task) and very low priority to avoid starving IDLE task
    // Priority 2: Lower than fetch (5) to ensure AFE drains quickly, but still responsive
    xTaskCreatePinnedToCore(streaming_capture_task, "audio_stream", 8192, NULL, 2, &streaming_capture_task_handle, 1);
}

void audio_stop_streaming_capture(void)
{
    if (!streaming_capture_task_handle) {
        ESP_LOGW(TAG, "Streaming capture not running");
        return;
    }
    ESP_LOGI(TAG, "Stopping streaming capture");

    // Clear task handle to signal task to exit
    streaming_capture_task_handle = NULL;

    // Wait a bit for task to exit gracefully
    vTaskDelay(pdMS_TO_TICKS(50));
}
