#include "sounds/ring_buffer.h"

#include "sounds/defer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct SoundRingBuffer {
    float *samples;
    uint64_t capacity;
    atomic_uint_fast64_t write_index;
};

static void copy_into_ring(
    SoundRingBuffer *ring,
    uint64_t index,
    const float *samples,
    uint64_t count
) {
    uint64_t offset = index % ring->capacity;
    uint64_t first = ring->capacity - offset;

    if (first > count) {
        first = count;
    }

    memcpy(ring->samples + offset, samples, sizeof(float) * (size_t)first);

    uint64_t remaining = count - first;
    if (remaining > 0) {
        memcpy(ring->samples, samples + first, sizeof(float) * (size_t)remaining);
    }
}

static void copy_from_ring(
    const SoundRingBuffer *ring,
    uint64_t index,
    float *samples,
    uint64_t count
) {
    uint64_t offset = index % ring->capacity;
    uint64_t first = ring->capacity - offset;

    if (first > count) {
        first = count;
    }

    memcpy(samples, ring->samples + offset, sizeof(float) * (size_t)first);

    uint64_t remaining = count - first;
    if (remaining > 0) {
        memcpy(samples + first, ring->samples, sizeof(float) * (size_t)remaining);
    }
}

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
    defer {
        sound_ring_buffer_destroy(created);
    }

    created->samples = calloc((size_t)capacity, sizeof(float));
    if (!created->samples) {
        sound_error_set(error, "could not allocate ring buffer samples");
        return false;
    }

    created->capacity = capacity;
    atomic_init(&created->write_index, 0);
    *ring = created;
    created = NULL;
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

    copy_into_ring(ring, write_index, samples, count);
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

    copy_from_ring(ring, start, samples, to_read);
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

    copy_from_ring(ring, start, samples, sample_count);
    return sample_count;
}

uint64_t sound_ring_buffer_read_available_ending_at(
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

    if (end_index > written || end_index <= oldest) {
        return 0;
    }

    uint64_t requested_start = end_index > sample_count ? end_index - sample_count : 0;
    uint64_t start = requested_start > oldest ? requested_start : oldest;
    uint64_t to_read = end_index - start;

    copy_from_ring(ring, start, samples, to_read);
    return to_read;
}
