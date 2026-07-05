#ifndef SOUNDS_CLIP_H
#define SOUNDS_CLIP_H

#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    SOUND_CLIP_LABEL_CAPACITY = 64,
};

typedef struct SoundClip {
    float *samples;
    uint64_t sample_count;
    uint64_t trim_start;
    uint64_t trim_end;
    double sample_rate;
    char label[SOUND_CLIP_LABEL_CAPACITY];
} SoundClip;

void sound_clip_init(SoundClip *clip);
void sound_clip_free(SoundClip *clip);
bool sound_clip_has_audio(const SoundClip *clip);
const float *sound_clip_samples(const SoundClip *clip);
uint64_t sound_clip_sample_count(const SoundClip *clip);
double sound_clip_duration_seconds(const SoundClip *clip);
double sound_clip_trim_start_seconds(const SoundClip *clip);
double sound_clip_trim_end_seconds(const SoundClip *clip);
bool sound_clip_replace(
    SoundClip *clip,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    const char *label,
    SoundError *error
);
bool sound_clip_replace_from_ring(
    SoundClip *clip,
    const SoundRingBuffer *ring,
    uint64_t end_sample,
    uint64_t requested_samples,
    double sample_rate,
    const char *label,
    SoundError *error
);
void sound_clip_clear_trim(SoundClip *clip);
void sound_clip_trim_start_by(SoundClip *clip, uint64_t samples);
void sound_clip_trim_end_by(SoundClip *clip, uint64_t samples);

#endif
