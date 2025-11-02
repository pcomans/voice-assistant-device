#pragma once

#include <stdbool.h>

typedef enum {
    ASSISTANT_STATE_IDLE = 0,
    ASSISTANT_STATE_RECORDING,
    ASSISTANT_STATE_SENDING,
    ASSISTANT_STATE_PLAYING,
    ASSISTANT_STATE_ERROR,
} assistant_state_t;

typedef struct {
    assistant_state_t state;
    bool wifi_connected;
    bool proxy_connected;
} assistant_status_t;

void assistant_set_state(assistant_state_t new_state);
void assistant_set_wifi_connected(bool connected);
assistant_status_t assistant_get_status(void);
