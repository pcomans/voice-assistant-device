#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AUDIO_PLAYBACK_EVENT_STARTED,
    AUDIO_PLAYBACK_EVENT_COMPLETED,
    AUDIO_PLAYBACK_EVENT_ERROR
} audio_playback_event_t;

typedef void (*audio_playback_callback_t)(audio_playback_event_t event, void *user_ctx);

/**
 * @brief Callback for playback reference signal (for AEC)
 *
 * Called with audio data BEFORE it's sent to the speaker.
 * Data is 16-bit PCM at 24kHz (same as I2S TX).
 *
 * @param pcm_data Audio data being played
 * @param pcm_len Length in bytes
 * @param ctx User context
 */
typedef void (*audio_playback_reference_cb_t)(const int16_t *pcm_data, size_t pcm_len, void *ctx);

void audio_playback_init(void);
void audio_playback_set_callback(audio_playback_callback_t callback, void *user_ctx);

/**
 * @brief Set reference callback for AEC (captures what's being played)
 *
 * @param ref_cb Reference callback function
 * @param user_ctx User context passed to callback
 */
void audio_playback_set_reference_callback(audio_playback_reference_cb_t ref_cb, void *user_ctx);

// Buffered playback (accumulate then play)
void audio_playback_play_pcm(const uint8_t *data, size_t length_bytes);

// Streaming playback (immediate, low latency)
bool audio_playback_stream_start(void);
bool audio_playback_stream_write(const uint8_t *data, size_t length_bytes);
void audio_playback_stream_end(void);

void audio_playback_stop(void);
void audio_playback_set_volume(uint8_t volume);  // 0-100
uint8_t audio_playback_get_volume(void);
