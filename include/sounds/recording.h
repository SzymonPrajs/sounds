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

typedef enum SoundRecordingFormat {
    SOUND_RECORDING_WAV_FLOAT32,
    SOUND_RECORDING_WAV_PCM16,
    SOUND_RECORDING_RAW_FLOAT32,
    SOUND_RECORDING_FORMAT_COUNT,
} SoundRecordingFormat;

int sound_recording_format_count(void);
SoundRecordingFormat sound_recording_format_at(int index);
int sound_recording_format_index(SoundRecordingFormat format);
const char *sound_recording_format_name(SoundRecordingFormat format);

bool sound_recording_save_latest(
    const SoundRingBuffer *ring,
    double sample_rate,
    SoundRecordingFormat format,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
);

bool sound_recording_save_recent(
    const SoundRingBuffer *ring,
    uint64_t requested_samples,
    double sample_rate,
    SoundRecordingFormat format,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
);

#endif
