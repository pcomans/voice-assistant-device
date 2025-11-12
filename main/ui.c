#include "ui.h"

#include "esp_log.h"
#include "lvgl.h"
#include <string.h>
#include "ST77916.h"
#include "LVGL_Driver.h"

static const char *TAG = "ui";

static ui_event_cb_t s_event_cb = NULL;
static void *s_event_ctx = NULL;
static lv_obj_t *s_button = NULL;
static lv_obj_t *s_label = NULL;

static void button_event_cb(lv_event_t *event)
{
    if (!s_event_cb) {
        return;
    }

    assistant_status_t status = assistant_get_status();
    ui_event_t ui_event = {
        .type = (status.state == ASSISTANT_STATE_STREAMING) ? UI_EVENT_RECORD_STOP : UI_EVENT_RECORD_START,
    };

    s_event_cb(&ui_event, s_event_ctx);
}

void ui_init(ui_event_cb_t cb, void *user_ctx)
{
    s_event_cb = cb;
    s_event_ctx = user_ctx;

    // Initialize LCD hardware and LVGL
    LCD_Init();     // Initializes ST77916, backlight, and touch
    LVGL_Init();    // Initializes LVGL with hardware display driver
    ESP_LOGI(TAG, "LCD and LVGL initialized");

    // Create UI elements
    lv_obj_t *screen = lv_scr_act();
    s_button = lv_btn_create(screen);
    lv_obj_set_size(s_button, 300, 120);  // 2x bigger button (default is ~150x60)
    lv_obj_center(s_button);
    lv_obj_add_event_cb(s_button, button_event_cb, LV_EVENT_CLICKED, NULL);

    s_label = lv_label_create(s_button);
    lv_label_set_text(s_label, "Unmute");
    lv_obj_center(s_label);

    // Make label text 2x bigger
    static lv_style_t style_label;
    lv_style_init(&style_label);
    lv_style_set_text_font(&style_label, &lv_font_montserrat_28);
    lv_obj_add_style(s_label, &style_label, 0);

    ESP_LOGI(TAG, "UI initialised");
}

void ui_update_state(assistant_status_t status)
{
    if (!s_label) {
        return;
    }

    const char *text = NULL;
    switch (status.state) {
    case ASSISTANT_STATE_IDLE:
        text = "Unmute";
        lv_obj_clear_state(s_button, LV_STATE_DISABLED);
        break;
    case ASSISTANT_STATE_STREAMING:
        text = "Mute";
        lv_obj_clear_state(s_button, LV_STATE_DISABLED);
        break;
    case ASSISTANT_STATE_ERROR:
    default:
        text = "Error – Tap";
        lv_obj_clear_state(s_button, LV_STATE_DISABLED);
        break;
    }

    lv_label_set_text(s_label, text);
}
