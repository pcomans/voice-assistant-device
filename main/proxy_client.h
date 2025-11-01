#pragma once

typedef enum {
    PROXY_RESULT_OK = 0,
    PROXY_RESULT_RETRY,
    PROXY_RESULT_FAILED,
} proxy_result_t;

typedef void (*proxy_result_cb_t)(proxy_result_t result, void *user_ctx);

void proxy_client_init(void);
void proxy_send_recording(proxy_result_cb_t cb, void *user_ctx);
