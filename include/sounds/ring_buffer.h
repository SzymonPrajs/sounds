#ifndef SOUNDS_RING_BUFFER_H
#define SOUNDS_RING_BUFFER_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SoundRingBuffer SoundRingBuffer;

bool sound_ring_buffer_create(
    uint64_t capacity,
    SoundRingBuffer **ring,
    SoundError *error
);

void sound_ring_buffer_destroy(SoundRingBuffer *ring);
uint64_t sound_ring_buffer_capacity(const SoundRingBuffer *ring);
uint64_t sound_ring_buffer_written(const SoundRingBuffer *ring);

void sound_ring_buffer_write(
    SoundRingBuffer *ring,
    const float *samples,
    uint32_t sample_count
);

uint64_t sound_ring_buffer_read_latest(
    const SoundRingBuffer *ring,
    float *samples,
    uint64_t sample_count
);

uint64_t sound_ring_buffer_read_ending_at(
    const SoundRingBuffer *ring,
    uint64_t end_index,
    float *samples,
    uint64_t sample_count
);

#endif
