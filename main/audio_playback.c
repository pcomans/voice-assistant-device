#include "audio_playback.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define PLAYBACK_I2S_PORT      I2S_NUM_0
#define PLAYBACK_SAMPLE_RATE   16000
#define PLAYBACK_DATA_WIDTH    I2S_DATA_BIT_WIDTH_16BIT

#define PLAYBACK_GPIO_BCLK     GPIO_NUM_48
#define PLAYBACK_GPIO_WS       GPIO_NUM_38
#define PLAYBACK_GPIO_DOUT     GPIO_NUM_47
#define PLAYBACK_GPIO_MCLK     GPIO_NUM_NC

static const char *TAG = "audio_playback";
static i2s_chan_handle_t s_tx_chan = NULL;

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
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(PLAYBACK_DATA_WIDTH, I2S_SLOT_MODE_MONO),
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
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "Playback pipeline initialised");
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

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, data, length_bytes, &bytes_written, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Played %u/%u bytes of PCM data", (unsigned int)bytes_written, (unsigned int)length_bytes);
    }
}
