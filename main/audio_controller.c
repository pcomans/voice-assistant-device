#include "audio_controller.h"
#include "smart_assistant.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_SAMPLE_RATE_HZ     16000
#define AUDIO_BITS_PER_SAMPLE    I2S_DATA_BIT_WIDTH_32BIT  // MEMS mic outputs 32-bit I2S data
#define AUDIO_CHANNEL_COUNT      1
#define AUDIO_FRAME_SAMPLES      256
#define STREAMING_CHUNK_MS       100  // Send 100ms chunks for streaming

static const char *TAG = "audio_ctrl";

static TaskHandle_t streaming_capture_task_handle = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static audio_capture_chunk_cb_t s_chunk_cb = NULL;
static void *s_chunk_ctx = NULL;

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

void audio_controller_init(void)
{
    esp_err_t err = configure_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S configuration failed: %d", err);
    } else {
        ESP_LOGI(TAG, "Audio controller initialised (I2S channel: %p)", (void*)s_rx_chan);
    }
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

    // Calculate chunk size for 100ms at 16kHz, 16-bit PCM
    const size_t chunk_samples = (AUDIO_SAMPLE_RATE_HZ * STREAMING_CHUNK_MS) / 1000;
    const size_t chunk_bytes = chunk_samples * sizeof(int16_t);  // 3200 bytes for 100ms

    // Buffers for I2S and PCM conversion
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

    size_t samples_in_chunk = 0;
    int frames_read = 0;

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

        frames_read++;

        // Convert 32-bit I2S to 16-bit PCM
        size_t samples_read = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < samples_read; i++) {
            if (samples_in_chunk < chunk_samples) {
                pcm_chunk[samples_in_chunk++] = (int16_t)(i2s_buffer[i] >> 14);
            }
        }

        // When chunk is full, send it via callback
        if (samples_in_chunk >= chunk_samples) {
            if (s_chunk_cb) {
                s_chunk_cb((const uint8_t *)pcm_chunk, chunk_bytes, s_chunk_ctx);
            }
            samples_in_chunk = 0;  // Reset for next chunk
        }
    }

    // Send final empty chunk to signal end of streaming
    if (s_chunk_cb) {
        ESP_LOGI(TAG, "Sending final empty chunk to signal end of streaming");
        s_chunk_cb(NULL, 0, s_chunk_ctx);
    }

    free(i2s_buffer);
    free(pcm_chunk);

    ESP_LOGI(TAG, "Streaming capture task exit (frames read: %d)", frames_read);
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
    xTaskCreatePinnedToCore(streaming_capture_task, "audio_stream", 4096, NULL, 5, &streaming_capture_task_handle, 0);
}

void audio_stop_streaming_capture(void)
{
    if (!streaming_capture_task_handle) {
        ESP_LOGW(TAG, "Streaming capture not running");
        return;
    }
    ESP_LOGI(TAG, "Stopping streaming capture");

    // Clear task handle to signal task to exit
    TaskHandle_t task = streaming_capture_task_handle;
    streaming_capture_task_handle = NULL;

    // Wait a bit for task to exit gracefully
    vTaskDelay(pdMS_TO_TICKS(50));
}
