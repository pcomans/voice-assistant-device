#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Simple linear interpolation resampler for audio
 *
 * Resamples 16-bit PCM audio from one sample rate to another.
 *
 * @param input Input PCM samples
 * @param input_len Number of input samples
 * @param input_rate Input sample rate (Hz)
 * @param output Output PCM samples (must be pre-allocated)
 * @param output_rate Output sample rate (Hz)
 * @return Number of output samples generated
 */
size_t audio_resample_linear(const int16_t *input, size_t input_len,
                             int input_rate, int16_t *output, int output_rate);

/**
 * @brief Calculate required output buffer size for resampling
 *
 * @param input_len Number of input samples
 * @param input_rate Input sample rate (Hz)
 * @param output_rate Output sample rate (Hz)
 * @return Number of output samples that will be generated
 */
static inline size_t audio_resample_calc_output_size(size_t input_len, int input_rate, int output_rate)
{
    return (input_len * output_rate) / input_rate;
}
