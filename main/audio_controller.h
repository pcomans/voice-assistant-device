#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*audio_capture_complete_cb_t)(void *ctx);
typedef void (*audio_capture_chunk_cb_t)(const uint8_t *pcm_data, size_t pcm_len, void *ctx);

void audio_controller_init(void);

// Original capture API (accumulate all audio in buffer)
void audio_start_capture(void);
void audio_stop_capture(void);
void audio_set_capture_complete_callback(audio_capture_complete_cb_t callback, void *ctx);

// Streaming capture API (send chunks immediately)
void audio_start_streaming_capture(audio_capture_chunk_cb_t chunk_cb, void *ctx);
void audio_stop_streaming_capture(void);
