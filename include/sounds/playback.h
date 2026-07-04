#ifndef SOUNDS_PLAYBACK_H
#define SOUNDS_PLAYBACK_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SoundPlayback SoundPlayback;

bool sound_playback_open(SoundPlayback **playback, SoundError *error);

/*
 * Copies mono 32-bit float samples before starting. The reported position is in
 * source frames, after sample-rate conversion to the output device rate.
 */
bool sound_playback_start(
    SoundPlayback *playback,
    const float *samples,
    uint64_t frame_count,
    double sample_rate,
    SoundError *error
);

bool sound_playback_stop(SoundPlayback *playback, SoundError *error);
bool sound_playback_is_playing(const SoundPlayback *playback);
uint64_t sound_playback_position(const SoundPlayback *playback);
void sound_playback_close(SoundPlayback *playback);

#endif
