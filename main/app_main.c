#include "smart_assistant.h"
#include "audio_controller.h"
#include "audio_playback.h"
#include "proxy_client.h"
#include "ui.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "smart_assistant";

static assistant_status_t g_status = {
    .state = ASSISTANT_STATE_IDLE,
    .proxy_connected = false,
};

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

static void initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char *)wifi_config.sta.ssid, "CHANGEME", sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, "CHANGEME", sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialised; configure SSID/password in NVS UI");
}

static void command_callback(proxy_result_t result, void *user_ctx)
{
    (void)user_ctx;

    assistant_state_t next_state = ASSISTANT_STATE_IDLE;
    if (result == PROXY_RESULT_RETRY) {
        next_state = ASSISTANT_STATE_SENDING;
    } else if (result == PROXY_RESULT_FAILED) {
        next_state = ASSISTANT_STATE_ERROR;
    }

    assistant_set_state(next_state);
}

static void ui_event_handler(const ui_event_t *event, void *ctx)
{
    (void)ctx;

    switch (event->type) {
    case UI_EVENT_RECORD_START:
        if (assistant_get_status().state == ASSISTANT_STATE_IDLE) {
            assistant_set_state(ASSISTANT_STATE_RECORDING);
            audio_start_capture();
        }
        break;
    case UI_EVENT_RECORD_STOP:
        if (assistant_get_status().state == ASSISTANT_STATE_RECORDING) {
            assistant_set_state(ASSISTANT_STATE_SENDING);
            audio_stop_capture();
            proxy_send_recording(command_callback, NULL);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unhandled UI event: %d", event->type);
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    initialise_wifi();

    ui_init(ui_event_handler, NULL);
    audio_controller_init();
    audio_playback_init();
    proxy_client_init();
    assistant_set_state(ASSISTANT_STATE_IDLE);
}
