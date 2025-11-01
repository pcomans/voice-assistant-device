#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    size_t bytes_per_sample;
    size_t capacity_bytes;
} pcm_buffer_config_t;

bool pcm_buffer_init(const pcm_buffer_config_t *cfg);
void pcm_buffer_reset(void);
size_t pcm_buffer_push(const int16_t *samples, size_t sample_count);
size_t pcm_buffer_pop(uint8_t *dst, size_t max_bytes);
size_t pcm_buffer_size(void);
size_t pcm_buffer_capacity(void);
