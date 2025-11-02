#pragma once

typedef void (*audio_capture_complete_cb_t)(void *ctx);

void audio_controller_init(void);
void audio_start_capture(void);
void audio_stop_capture(void);
void audio_set_capture_complete_callback(audio_capture_complete_cb_t callback, void *ctx);
