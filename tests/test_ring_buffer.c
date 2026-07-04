#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <stdbool.h>
#include <stdio.h>

static int failures = 0;

static void expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void test_ordered_read(void) {
    SoundError error;
    SoundRingBuffer *ring = NULL;
    float input[] = {1.0F, 2.0F, 3.0F, 4.0F};
    float output[] = {0.0F, 0.0F};

    sound_error_clear(&error);
    expect(sound_ring_buffer_create(8, &ring, &error), sound_error_message(&error));
    sound_ring_buffer_write(ring, input, 4);

    expect(sound_ring_buffer_written(ring) == 4, "write count tracks samples");
    expect(sound_ring_buffer_read_latest(ring, output, 2) == 2, "latest read returns requested count");
    expect(output[0] == 3.0F && output[1] == 4.0F, "latest read preserves order");

    sound_ring_buffer_destroy(ring);
}

static void test_wraparound_read(void) {
    SoundError error;
    SoundRingBuffer *ring = NULL;
    float first[] = {1.0F, 2.0F, 3.0F, 4.0F};
    float second[] = {5.0F, 6.0F, 7.0F};
    float output[] = {0.0F, 0.0F, 0.0F, 0.0F};

    sound_error_clear(&error);
    expect(sound_ring_buffer_create(4, &ring, &error), sound_error_message(&error));
    sound_ring_buffer_write(ring, first, 4);
    sound_ring_buffer_write(ring, second, 3);

    expect(sound_ring_buffer_written(ring) == 7, "write count survives wraparound");
    expect(sound_ring_buffer_read_latest(ring, output, 4) == 4, "wrapped read returns capacity");
    expect(
        output[0] == 4.0F &&
        output[1] == 5.0F &&
        output[2] == 6.0F &&
        output[3] == 7.0F,
        "wrapped read returns oldest-to-newest latest samples"
    );

    sound_ring_buffer_destroy(ring);
}

static void test_large_write_keeps_tail(void) {
    SoundError error;
    SoundRingBuffer *ring = NULL;
    float input[] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    float output[] = {0.0F, 0.0F, 0.0F};

    sound_error_clear(&error);
    expect(sound_ring_buffer_create(3, &ring, &error), sound_error_message(&error));
    sound_ring_buffer_write(ring, input, 5);

    expect(sound_ring_buffer_read_latest(ring, output, 3) == 3, "large write returns full tail");
    expect(output[0] == 3.0F && output[1] == 4.0F && output[2] == 5.0F, "large write keeps tail");

    sound_ring_buffer_destroy(ring);
}

static void test_absolute_read(void) {
    SoundError error;
    SoundRingBuffer *ring = NULL;
    float first[] = {1.0F, 2.0F, 3.0F, 4.0F};
    float second[] = {5.0F, 6.0F, 7.0F, 8.0F};
    float output[] = {0.0F, 0.0F, 0.0F};

    sound_error_clear(&error);
    expect(sound_ring_buffer_create(6, &ring, &error), sound_error_message(&error));
    sound_ring_buffer_write(ring, first, 4);
    sound_ring_buffer_write(ring, second, 4);

    expect(
        sound_ring_buffer_read_ending_at(ring, 6, output, 3) == 3,
        "absolute read returns requested count"
    );
    expect(output[0] == 4.0F && output[1] == 5.0F && output[2] == 6.0F, "absolute read is ordered");

    expect(
        sound_ring_buffer_read_ending_at(ring, 3, output, 3) == 0,
        "absolute read rejects overwritten samples"
    );

    sound_ring_buffer_destroy(ring);
}

int main(void) {
    test_ordered_read();
    test_wraparound_read();
    test_large_write_keeps_tail();
    test_absolute_read();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }

    fprintf(stderr, "ring buffer tests passed\n");
    return 0;
}
