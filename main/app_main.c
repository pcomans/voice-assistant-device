#include "smart_assistant.h"
#include "audio_controller.h"
#include "audio_playback.h"
#include "proxy_client.h"
#include "websocket_client.h"
#include "ui.h"
#include "wifi_credentials.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "smart_assistant";

static assistant_status_t g_status = {
    .state = ASSISTANT_STATE_IDLE,
    .wifi_connected = false,
    .proxy_connected = false,
};

// Mic muting state (to prevent echo when assistant is speaking)
static bool g_mic_muted_for_speech = false;

// Forward declarations
static void streaming_chunk_handler(const uint8_t *pcm_data, size_t pcm_len, void *ctx);

static void playback_event_handler(audio_playback_event_t event, void *ctx)
{
    (void)ctx;

    switch (event) {
    case AUDIO_PLAYBACK_EVENT_STARTED:
        ESP_LOGI(TAG, "Playback started");
        break;
    case AUDIO_PLAYBACK_EVENT_COMPLETED:
        ESP_LOGI(TAG, "Playback completed");
        // No state change needed - can stay in STREAMING if mic still unmuted
        break;
    case AUDIO_PLAYBACK_EVENT_ERROR:
        ESP_LOGE(TAG, "Playback error");
        assistant_set_state(ASSISTANT_STATE_ERROR);
        break;
    }
}

assistant_status_t assistant_get_status(void)
{
    return g_status;
}

void assistant_set_state(assistant_state_t new_state)
{
    if (g_status.state != new_state) {
        g_status.state = new_state;
        ui_update_state(g_status);
    }
}

void assistant_set_wifi_connected(bool connected)
{
    if (g_status.wifi_connected != connected) {
        g_status.wifi_connected = connected;
        ui_update_state(g_status);
        ESP_LOGI(TAG, "Wi-Fi %s", connected ? "connected" : "disconnected");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Wi-Fi connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        assistant_set_wifi_connected(false);
        esp_wifi_connect();
        ESP_LOGI(TAG, "Reconnecting to Wi-Fi...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        assistant_set_wifi_connected(true);

        // WiFi is ready, connect to proxy
        proxy_client_connect();
    }
}

static void initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialised; configure SSID/password in NVS UI");
}

static void speech_event_handler(bool is_speaking, void *ctx)
{
    (void)ctx;

    if (is_speaking) {
        ESP_LOGI(TAG, "Assistant started speaking - muting microphone to prevent echo");
        g_mic_muted_for_speech = true;
    } else {
        ESP_LOGI(TAG, "Assistant stopped speaking - unmuting microphone");
        g_mic_muted_for_speech = false;
    }
}

static void streaming_chunk_handler(const uint8_t *pcm_data, size_t pcm_len, void *ctx)
{
    (void)ctx;

    // Skip sending audio if muted due to assistant speaking (prevents echo)
    if (g_mic_muted_for_speech) {
        ESP_LOGD(TAG, "Mic muted for speech - skipping %zu bytes", pcm_len);
        return;
    }

    // Forward audio chunks directly to WebSocket
    // With server-side VAD, OpenAI will detect when user stops speaking
    // We don't send empty frames - just stop sending audio when muted
    if (pcm_len > 0) {
        esp_err_t err = ws_client_send_audio(pcm_data, pcm_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send audio chunk: %s", esp_err_to_name(err));
        }
    }
}

static void ui_event_handler(const ui_event_t *event, void *ctx)
{
    (void)ctx;

    switch (event->type) {
    case UI_EVENT_RECORD_START:
        if (!assistant_get_status().wifi_connected) {
            ESP_LOGW(TAG, "Cannot start streaming: Wi-Fi not connected");
            break;
        }
        if (assistant_get_status().state == ASSISTANT_STATE_IDLE) {
            ESP_LOGI(TAG, "Unmuting microphone - starting continuous streaming");

            // Stop any previous playback stream and start fresh
            audio_playback_stream_end();

            // Start playback stream to receive OpenAI responses
            if (!audio_playback_stream_start()) {
                ESP_LOGE(TAG, "Failed to start playback stream");
                break;
            }

            assistant_set_state(ASSISTANT_STATE_STREAMING);
            audio_start_streaming_capture(streaming_chunk_handler, NULL);
        }
        break;
    case UI_EVENT_RECORD_STOP:
        if (assistant_get_status().state == ASSISTANT_STATE_STREAMING) {
            ESP_LOGI(TAG, "Muting microphone - stopping streaming");
            assistant_set_state(ASSISTANT_STATE_IDLE);
            audio_stop_streaming_capture();
            // Final empty chunk will be sent by audio_controller.c

            // Keep playback stream open to let assistant finish speaking
            // It will be stopped when starting a new streaming session
        }
        break;
    default:
        ESP_LOGW(TAG, "Unhandled UI event: %d", event->type);
        break;
    }
}

static void lvgl_task(void *pvParameter)
{
    (void)pvParameter;

    while (1) {
        // Call LVGL task handler to process timers and render UI
        lv_timer_handler();

        // Delay for 10ms (LVGL recommends 5-20ms)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    initialise_wifi();

    ui_init(ui_event_handler, NULL);
    audio_controller_init();
    audio_playback_init();
    audio_playback_set_callback(playback_event_handler, NULL);
    proxy_client_init(speech_event_handler, NULL);  // Pass speech event handler for mic muting
    assistant_set_state(ASSISTANT_STATE_IDLE);

    // Create LVGL task to periodically update the display
    xTaskCreate(lvgl_task, "lvgl_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "LVGL task created");
}
