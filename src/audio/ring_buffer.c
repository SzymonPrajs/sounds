#include "sounds/ring_buffer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct SoundRingBuffer {
    float *samples;
    uint64_t capacity;
    atomic_uint_fast64_t write_index;
};

bool sound_ring_buffer_create(
    uint64_t capacity,
    SoundRingBuffer **ring,
    SoundError *error
) {
    sound_error_clear(error);

    if (!ring || capacity == 0) {
        sound_error_set(error, "invalid ring buffer capacity");
        return false;
    }

    if (capacity > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "ring buffer capacity is too large");
        return false;
    }

    SoundRingBuffer *created = calloc(1, sizeof(*created));
    if (!created) {
        sound_error_set(error, "could not allocate ring buffer");
        return false;
    }

    created->samples = calloc((size_t)capacity, sizeof(float));
    if (!created->samples) {
        free(created);
        sound_error_set(error, "could not allocate ring buffer samples");
        return false;
    }

    created->capacity = capacity;
    atomic_init(&created->write_index, 0);
    *ring = created;
    return true;
}

void sound_ring_buffer_destroy(SoundRingBuffer *ring) {
    if (!ring) {
        return;
    }

    free(ring->samples);
    free(ring);
}

uint64_t sound_ring_buffer_capacity(const SoundRingBuffer *ring) {
    return ring ? ring->capacity : 0;
}

uint64_t sound_ring_buffer_written(const SoundRingBuffer *ring) {
    if (!ring) {
        return 0;
    }

    return atomic_load_explicit(&ring->write_index, memory_order_acquire);
}

void sound_ring_buffer_write(
    SoundRingBuffer *ring,
    const float *samples,
    uint32_t sample_count
) {
    if (!ring || !samples || sample_count == 0) {
        return;
    }

    uint64_t count = sample_count;
    if (count > ring->capacity) {
        samples += count - ring->capacity;
        count = ring->capacity;
    }

    uint64_t write_index = atomic_load_explicit(&ring->write_index, memory_order_relaxed);

    for (uint64_t i = 0; i < count; ++i) {
        ring->samples[(write_index + i) % ring->capacity] = samples[i];
    }

    atomic_store_explicit(&ring->write_index, write_index + count, memory_order_release);
}

uint64_t sound_ring_buffer_read_latest(
    const SoundRingBuffer *ring,
    float *samples,
    uint64_t sample_count
) {
    if (!ring || !samples || sample_count == 0) {
        return 0;
    }

    uint64_t written = atomic_load_explicit(&ring->write_index, memory_order_acquire);
    uint64_t available = written < ring->capacity ? written : ring->capacity;
    uint64_t to_read = sample_count < available ? sample_count : available;
    uint64_t start = written - to_read;

    for (uint64_t i = 0; i < to_read; ++i) {
        samples[i] = ring->samples[(start + i) % ring->capacity];
    }

    return to_read;
}

uint64_t sound_ring_buffer_read_ending_at(
    const SoundRingBuffer *ring,
    uint64_t end_index,
    float *samples,
    uint64_t sample_count
) {
    if (!ring || !samples || sample_count == 0) {
        return 0;
    }

    uint64_t written = atomic_load_explicit(&ring->write_index, memory_order_acquire);
    uint64_t available = written < ring->capacity ? written : ring->capacity;
    uint64_t oldest = written - available;

    if (end_index > written) {
        return 0;
    }

    if (end_index < sample_count || end_index - sample_count < oldest) {
        return 0;
    }

    uint64_t start = end_index - sample_count;

    for (uint64_t i = 0; i < sample_count; ++i) {
        samples[i] = ring->samples[(start + i) % ring->capacity];
    }

    return sample_count;
}
