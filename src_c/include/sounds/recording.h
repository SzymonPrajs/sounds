#ifndef SOUNDS_RECORDING_H
#define SOUNDS_RECORDING_H

#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SOUND_RECORDING_PATH_CAPACITY = 512,
};

typedef struct SoundRecordingInfo {
    double sample_rate;
    double duration_seconds;
    uint64_t sample_count;
} SoundRecordingInfo;

const char *sound_recording_directory(void);

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

bool sound_recording_save_samples(
    const float *samples,
    uint64_t requested_samples,
    double sample_rate,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
);

bool sound_recording_read_info(
    const char *path,
    SoundRecordingInfo *info,
    SoundError *error
);

bool sound_recording_load_samples(
    const char *path,
    float **samples,
    uint64_t *sample_count,
    double *sample_rate,
    SoundError *error
);

#endif
