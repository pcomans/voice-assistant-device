#pragma once

#include "smart_assistant.h"

typedef enum {
    UI_EVENT_NONE = 0,
    UI_EVENT_RECORD_START,
    UI_EVENT_RECORD_STOP,
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
} ui_event_t;

typedef void (*ui_event_cb_t)(const ui_event_t *event, void *user_ctx);

void ui_init(ui_event_cb_t cb, void *user_ctx);
void ui_update_state(assistant_status_t status);
