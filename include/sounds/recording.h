#ifndef SOUNDS_RECORDING_H
#define SOUNDS_RECORDING_H

#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SOUND_RECORDING_PATH_CAPACITY = 128,
};

bool sound_recording_save_latest(
    const SoundRingBuffer *ring,
    double sample_rate,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
);

bool sound_recording_save_recent(
    const SoundRingBuffer *ring,
    uint64_t requested_samples,
    double sample_rate,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
);

#endif
