#include "audio_playback.h"

#include <assert.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PLAYBACK_I2S_PORT      I2S_NUM_0
#define PLAYBACK_SAMPLE_RATE   24000  // Match OpenAI Realtime API output
#define PLAYBACK_DATA_WIDTH    I2S_DATA_BIT_WIDTH_16BIT

#define PLAYBACK_GPIO_BCLK     GPIO_NUM_48
#define PLAYBACK_GPIO_WS       GPIO_NUM_38
#define PLAYBACK_GPIO_DOUT     GPIO_NUM_47
#define PLAYBACK_GPIO_MCLK     GPIO_NUM_NC

static const char *TAG = "audio_playback";
static i2s_chan_handle_t s_tx_chan = NULL;
static TaskHandle_t s_playback_task = NULL;
static audio_playback_callback_t s_callback = NULL;
static void *s_callback_ctx = NULL;
static uint8_t s_volume = 100;  // Default volume 100%

typedef struct {
    uint8_t *data;
    size_t length;
} playback_task_params_t;

void audio_playback_init(void)
{
    if (s_tx_chan) {
        return;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(PLAYBACK_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PLAYBACK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(PLAYBACK_DATA_WIDTH, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PLAYBACK_GPIO_MCLK,
            .bclk = PLAYBACK_GPIO_BCLK,
            .ws   = PLAYBACK_GPIO_WS,
            .dout = PLAYBACK_GPIO_DOUT,
            .din  = GPIO_NUM_NC,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // Don't set slot_mask - use default (both channels) for mono playback

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "Playback pipeline initialised");
}

void audio_playback_set_callback(audio_playback_callback_t callback, void *user_ctx)
{
    s_callback = callback;
    s_callback_ctx = user_ctx;
}

void audio_playback_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
    ESP_LOGI(TAG, "Volume set to %u%%", volume);
}

uint8_t audio_playback_get_volume(void)
{
    return s_volume;
}

// Apply volume scaling in-place to 16-bit PCM samples
static void apply_volume(int16_t *samples, size_t num_samples, uint8_t volume)
{
    if (volume == 100) {
        return;  // No scaling needed
    }

    for (size_t i = 0; i < num_samples; i++) {
        int32_t scaled = ((int32_t)samples[i] * volume) / 100;
        samples[i] = (int16_t)scaled;
    }
}

static void playback_task(void *arg)
{
    playback_task_params_t *params = (playback_task_params_t *)arg;

    // Notify started
    if (s_callback) {
        s_callback(AUDIO_PLAYBACK_EVENT_STARTED, s_callback_ctx);
    }

    // Apply volume scaling in-place (only if volume != 100%)
    if (s_volume != 100) {
        size_t num_samples = params->length / sizeof(int16_t);
        apply_volume((int16_t *)params->data, num_samples, s_volume);
    }

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, params->data, params->length, &bytes_written, portMAX_DELAY);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
        if (s_callback) {
            s_callback(AUDIO_PLAYBACK_EVENT_ERROR, s_callback_ctx);
        }
    } else {
        ESP_LOGI(TAG, "Played %u/%u bytes of PCM data (volume: %u%%)",
                 (unsigned int)bytes_written, (unsigned int)params->length, s_volume);
        if (s_callback) {
            s_callback(AUDIO_PLAYBACK_EVENT_COMPLETED, s_callback_ctx);
        }
    }

    // Cleanup
    free(params->data);
    free(params);
    s_playback_task = NULL;
    vTaskDelete(NULL);
}

void audio_playback_play_pcm(const uint8_t *data, size_t length_bytes)
{
    if (!s_tx_chan) {
        ESP_LOGW(TAG, "Playback channel not initialised");
        return;
    }
    if (!data || length_bytes == 0) {
        ESP_LOGW(TAG, "No PCM payload to play");
        return;
    }
    if (s_playback_task) {
        ESP_LOGW(TAG, "Playback already in progress");
        return;
    }

    // Allocate memory for task parameters and audio data
    playback_task_params_t *params = malloc(sizeof(playback_task_params_t));
    assert(params != NULL && "Critical: Failed to allocate task params");
    if (!params) {
        ESP_LOGE(TAG, "Failed to allocate task params");
        if (s_callback) {
            s_callback(AUDIO_PLAYBACK_EVENT_ERROR, s_callback_ctx);
        }
        return;
    }

    // Large audio buffer - must use SPIRAM
    params->data = heap_caps_malloc(length_bytes, MALLOC_CAP_SPIRAM);
    assert(params->data != NULL && "Critical: Failed to allocate audio buffer from SPIRAM");
    if (!params->data) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes from SPIRAM for audio buffer", (unsigned int)length_bytes);
        free(params);
        if (s_callback) {
            s_callback(AUDIO_PLAYBACK_EVENT_ERROR, s_callback_ctx);
        }
        return;
    }
    ESP_LOGI(TAG, "Allocated %u bytes for playback", (unsigned int)length_bytes);

    memcpy(params->data, data, length_bytes);
    params->length = length_bytes;

    // Create playback task pinned to core 1 (following demo best practice)
    BaseType_t result = xTaskCreatePinnedToCore(
        playback_task,
        "audio_playback",
        4096,
        params,
        5,
        &s_playback_task,
        1  // Pin to core 1
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        free(params->data);
        free(params);
        if (s_callback) {
            s_callback(AUDIO_PLAYBACK_EVENT_ERROR, s_callback_ctx);
        }
    }
}

void audio_playback_stop(void)
{
    if (s_playback_task) {
        vTaskDelete(s_playback_task);
        s_playback_task = NULL;
        ESP_LOGI(TAG, "Playback stopped");
    }
}
