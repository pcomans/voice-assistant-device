#include "audio_controller.h"
#include "pcm_buffer.h"
#include "smart_assistant.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_SAMPLE_RATE_HZ     16000
#define AUDIO_BITS_PER_SAMPLE    I2S_DATA_BIT_WIDTH_32BIT  // MEMS mic outputs 32-bit I2S data
#define AUDIO_CHANNEL_COUNT      1
#define AUDIO_BUFFER_SECONDS     10  // 10 seconds = 320KB (using SPIRAM)
#define AUDIO_FRAME_SAMPLES      256

static const char *TAG = "audio_ctrl";

static TaskHandle_t capture_task_handle = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static audio_capture_complete_cb_t s_capture_complete_cb = NULL;
static void *s_capture_complete_ctx = NULL;

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

static void capture_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Capture task started (I2S channel: %p)", (void*)s_rx_chan);

    if (!s_rx_chan) {
        ESP_LOGE(TAG, "I2S channel is NULL!");
        capture_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // MEMS mic outputs 32-bit I2S data, we need to convert to 16-bit PCM
    int32_t i2s_buffer[AUDIO_FRAME_SAMPLES] = {0};
    int16_t pcm_buffer[AUDIO_FRAME_SAMPLES] = {0};
    const size_t i2s_bytes = sizeof(i2s_buffer);

    // Calculate max recording duration in bytes (10 seconds)
    const size_t max_recording_bytes = AUDIO_SAMPLE_RATE_HZ * AUDIO_BUFFER_SECONDS * sizeof(int16_t);
    size_t total_bytes_captured = 0;

    ESP_LOGI(TAG, "Starting I2S read loop (max recording: %u bytes / %d seconds)",
             (unsigned int)max_recording_bytes, AUDIO_BUFFER_SECONDS);
    size_t total_reads = 0;

    while (assistant_get_status().state == ASSISTANT_STATE_RECORDING) {
        size_t bytes_read = 0;
        // Use blocking read like the demo does - wait for data to be available
        esp_err_t err = i2s_channel_read(s_rx_chan, i2s_buffer, i2s_bytes, &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s read failed: %d (after %u successful reads)", err, (unsigned int)total_reads);
            break;
        }
        if (bytes_read == 0) {
            ESP_LOGW(TAG, "i2s read returned 0 bytes");
            continue;
        }

        total_reads++;
        if (total_reads == 1) {
            ESP_LOGI(TAG, "First I2S read successful, read %u bytes", (unsigned int)bytes_read);
        }

        const size_t sample_count = bytes_read / sizeof(int32_t);

        // Convert 32-bit I2S data to 16-bit PCM (shift right by 14 bits to amplify signal)
        for (size_t i = 0; i < sample_count; i++) {
            pcm_buffer[i] = (int16_t)(i2s_buffer[i] >> 14);
        }

        size_t samples_pushed = pcm_buffer_push(pcm_buffer, sample_count);
        total_bytes_captured += samples_pushed * sizeof(int16_t);

        // Auto-stop after 10 seconds
        if (total_bytes_captured >= max_recording_bytes) {
            ESP_LOGI(TAG, "Recording limit reached (%u bytes / %d seconds), auto-stopping",
                     (unsigned int)total_bytes_captured, AUDIO_BUFFER_SECONDS);
            assistant_set_state(ASSISTANT_STATE_IDLE);

            // Notify that recording completed via auto-stop
            if (s_capture_complete_cb) {
                s_capture_complete_cb(s_capture_complete_ctx);
            }
            break;
        }
    }

    ESP_LOGI(TAG, "Capture task exit (total reads: %u, total bytes: %u)",
             (unsigned int)total_reads, (unsigned int)total_bytes_captured);
    capture_task_handle = NULL;
    vTaskDelete(NULL);
}

void audio_controller_init(void)
{
    const size_t capacity_bytes = AUDIO_SAMPLE_RATE_HZ * AUDIO_BUFFER_SECONDS * sizeof(int16_t);
    pcm_buffer_config_t cfg = {
        .bytes_per_sample = sizeof(int16_t),
        .capacity_bytes = capacity_bytes,
    };

    if (!pcm_buffer_init(&cfg)) {
        ESP_LOGE(TAG, "PCM buffer init failed");
    }

    esp_err_t err = configure_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S configuration failed: %d", err);
    } else {
        ESP_LOGI(TAG, "Audio controller initialised (I2S channel: %p)", (void*)s_rx_chan);
    }
}

void audio_start_capture(void)
{
    if (capture_task_handle) {
        ESP_LOGW(TAG, "Capture already running");
        return;
    }

    // Don't restart the I2S channel - keep it running continuously
    // Just reset the buffer and start capturing
    ESP_LOGI(TAG, "Starting audio capture (I2S channel: %p)", (void*)s_rx_chan);

    pcm_buffer_reset();
    xTaskCreatePinnedToCore(capture_task, "audio_capture", 4096, NULL, 5, &capture_task_handle, 0);
}

void audio_stop_capture(void)
{
    if (!capture_task_handle) {
        ESP_LOGW(TAG, "Capture not running");
        return;
    }
    ESP_LOGI(TAG, "Stopping capture");
    // The task checks the state and exits.
}

void audio_set_capture_complete_callback(audio_capture_complete_cb_t callback, void *ctx)
{
    s_capture_complete_cb = callback;
    s_capture_complete_ctx = ctx;
}
