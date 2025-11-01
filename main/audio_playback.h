#pragma once

#include <stddef.h>
#include <stdint.h>

void audio_playback_init(void);
void audio_playback_play_pcm(const uint8_t *data, size_t length_bytes);
