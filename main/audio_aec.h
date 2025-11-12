#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Callback for cleaned audio output from AFE (16kHz PCM)
 *
 * @param pcm_data Cleaned audio data (16-bit PCM, 16kHz)
 * @param pcm_len Length of audio data in bytes
 * @param ctx User context pointer
 */
typedef void (*audio_aec_output_cb_t)(const int16_t *pcm_data, size_t pcm_len, void *ctx);

/**
 * @brief Initialize AEC module with ESP-SR AFE
 *
 * @param output_cb Callback for cleaned audio output
 * @param user_ctx User context passed to callback
 * @return true on success, false on failure
 */
bool audio_aec_init(audio_aec_output_cb_t output_cb, void *user_ctx);

/**
 * @brief Process audio through AFE (mic + reference)
 *
 * This function feeds microphone and playback reference to the AFE.
 * Both inputs must be at 16kHz sample rate.
 *
 * @param mic_samples Microphone samples (16-bit PCM, 16kHz)
 * @param ref_samples Playback reference samples (16-bit PCM, 16kHz)
 * @param num_samples Number of samples per channel
 * @return true if processing succeeded, false otherwise
 */
bool audio_aec_process(const int16_t *mic_samples, const int16_t *ref_samples, size_t num_samples);

/**
 * @brief Get the required chunk size for AEC processing
 *
 * @return Number of samples PER CHANNEL required by AFE (typically 128 samples = 8ms @ 16kHz)
 */
size_t audio_aec_get_chunk_size(void);

/**
 * @brief Deinitialize AEC module
 */
void audio_aec_deinit(void);
