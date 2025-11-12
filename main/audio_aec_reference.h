#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize AEC reference buffer module
 *
 * This module receives 24kHz playback audio, downsamples it to 16kHz,
 * and buffers it for consumption by the AEC module.
 *
 * @param buffer_ms Size of reference buffer in milliseconds
 * @return true on success, false on failure
 */
bool audio_aec_reference_init(int buffer_ms);

/**
 * @brief Feed playback reference audio (24kHz) for echo cancellation
 *
 * This is called by the playback module with audio being sent to the speaker.
 * The module will downsample to 16kHz and buffer it.
 *
 * @param pcm_24khz Playback audio at 24kHz (16-bit PCM)
 * @param samples_24khz Number of samples at 24kHz
 */
void audio_aec_reference_feed(const int16_t *pcm_24khz, size_t samples_24khz);

/**
 * @brief Get reference samples (16kHz) for AEC processing
 *
 * Retrieves reference samples from the buffer, or returns silence if no
 * playback is active.
 *
 * @param output Output buffer for reference samples (16kHz)
 * @param num_samples Number of samples to retrieve
 * @return true if samples were retrieved, false if buffer underrun (will fill with silence)
 */
bool audio_aec_reference_get(int16_t *output, size_t num_samples);

/**
 * @brief Deinitialize reference buffer module
 */
void audio_aec_reference_deinit(void);
