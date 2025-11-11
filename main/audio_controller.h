#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*audio_capture_chunk_cb_t)(const uint8_t *pcm_data, size_t pcm_len, void *ctx);

void audio_controller_init(void);

// Streaming capture API
void audio_start_streaming_capture(audio_capture_chunk_cb_t chunk_cb, void *ctx);
void audio_stop_streaming_capture(void);
