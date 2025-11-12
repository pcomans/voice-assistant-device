#include "audio_resampler.h"

size_t audio_resample_linear(const int16_t *input, size_t input_len,
                             int input_rate, int16_t *output, int output_rate)
{
    if (!input || !output || input_len == 0 || input_rate == 0 || output_rate == 0) {
        return 0;
    }

    // Calculate output length
    size_t output_len = (input_len * output_rate) / input_rate;

    // Simple linear interpolation resampling
    for (size_t i = 0; i < output_len; i++) {
        // Calculate corresponding input position (fractional)
        float input_pos = ((float)i * input_rate) / output_rate;
        size_t input_idx = (size_t)input_pos;

        // Handle boundary case
        if (input_idx >= input_len - 1) {
            output[i] = input[input_len - 1];
            continue;
        }

        // Linear interpolation between two samples
        float frac = input_pos - input_idx;
        int32_t sample0 = input[input_idx];
        int32_t sample1 = input[input_idx + 1];
        output[i] = (int16_t)(sample0 + (int32_t)(frac * (sample1 - sample0)));
    }

    return output_len;
}
