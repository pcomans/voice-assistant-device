#include "audio_controller.h"
#include "pcm_buffer.h"
#include "smart_assistant.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_SAMPLE_RATE_HZ     16000
#define AUDIO_BITS_PER_SAMPLE    I2S_DATA_BIT_WIDTH_16BIT
#define AUDIO_CHANNEL_COUNT      1
#define AUDIO_BUFFER_SECONDS     5
#define AUDIO_FRAME_SAMPLES      256

static const char *TAG = "audio_ctrl";

static TaskHandle_t capture_task_handle = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;

static esp_err_t configure_i2s(void)
{
    if (s_rx_chan) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan), TAG, "i2s_new_channel failed");

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

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "i2s enable failed");

    return ESP_OK;
}

static void capture_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Capture task started");

    int16_t frame_buffer[AUDIO_FRAME_SAMPLES] = {0};
    const size_t frame_bytes = sizeof(frame_buffer);

    while (assistant_get_status().state == ASSISTANT_STATE_RECORDING) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, frame_buffer, frame_bytes, &bytes_read, pdMS_TO_TICKS(50));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s read failed: %d", err);
            continue;
        }
        if (bytes_read == 0) {
            continue;
        }

        const size_t sample_count = bytes_read / sizeof(int16_t);
        pcm_buffer_push(frame_buffer, sample_count);
    }

    ESP_LOGI(TAG, "Capture task exit");
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
        ESP_LOGI(TAG, "Audio peripherals initialised");
    }
}

void audio_start_capture(void)
{
    if (capture_task_handle) {
        ESP_LOGW(TAG, "Capture already running");
        return;
    }
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
