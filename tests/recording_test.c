#include "sounds/error.h"
#include "sounds/recording.h"
#include "sounds/ring_buffer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
    }

    return condition;
}

static uint16_t read_u16_le(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static bool read_file_prefix(const char *path, uint8_t *bytes, size_t size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "could not open %s\n", path);
        return false;
    }

    bool ok = fread(bytes, 1, size, file) == size;
    bool closed = fclose(file) == 0;

    return expect(ok && closed, "could not read recording file");
}

static bool save_test_recording(
    const SoundRingBuffer *ring,
    char *path,
    SoundError *error
) {
    uint64_t count = 0;

    if (!sound_recording_save_recent(
            ring,
            4,
            48000.0,
            &count,
            path,
            SOUND_RECORDING_PATH_CAPACITY,
            error
        )) {
        fprintf(stderr, "%s\n", sound_error_message(error));
        return false;
    }

    return expect(count == 4, "recording writer saved the wrong sample count");
}

static bool save_test_samples(
    const float *samples,
    char *path,
    SoundError *error
) {
    uint64_t count = 0;

    if (!sound_recording_save_samples(
            samples,
            4,
            48000.0,
            &count,
            path,
            SOUND_RECORDING_PATH_CAPACITY,
            error
        )) {
        fprintf(stderr, "%s\n", sound_error_message(error));
        return false;
    }

    return expect(count == 4, "sample writer saved the wrong sample count");
}

static bool check_wav(
    const char *path,
    uint32_t expected_data_bytes
) {
    uint8_t header[56];

    if (!read_file_prefix(path, header, sizeof(header))) {
        return false;
    }

    return expect(memcmp(header, "RIFF", 4) == 0, "WAV file is missing RIFF") &&
        expect(memcmp(header + 8, "WAVE", 4) == 0, "WAV file is missing WAVE") &&
        expect(memcmp(header + 12, "fmt ", 4) == 0, "WAV file is missing fmt") &&
        expect(read_u16_le(header + 20) == 3U, "WAV format tag is wrong") &&
        expect(read_u16_le(header + 22) == 1U, "WAV channel count is wrong") &&
        expect(read_u32_le(header + 24) == 48000U, "WAV sample rate is wrong") &&
        expect(read_u16_le(header + 34) == 32U, "WAV bit depth is wrong") &&
        expect(memcmp(header + 36, "fact", 4) == 0, "float WAV is missing fact") &&
        expect(read_u32_le(header + 40) == 4U, "WAV fact size is wrong") &&
        expect(read_u32_le(header + 44) == 4U, "WAV fact sample count is wrong") &&
        expect(memcmp(header + 48, "data", 4) == 0, "WAV file is missing data") &&
        expect(
            read_u32_le(header + 52) == expected_data_bytes,
            "WAV data size is wrong"
        );
}

static bool check_stale_recording_stop_read(SoundError *error) {
    SoundRingBuffer *ring = NULL;
    float initial[] = {-1.0F, -0.25F, 0.25F, 1.0F};
    float replacement[] = {5.0F};
    float stopped[4] = {0.0F};
    bool ok = sound_ring_buffer_create(4, &ring, error);

    if (!ok) {
        return false;
    }

    sound_ring_buffer_write(ring, initial, 4);

    uint64_t stopped_at = sound_ring_buffer_written(ring);
    sound_ring_buffer_write(ring, replacement, 1);

    uint64_t count =
        sound_ring_buffer_read_available_ending_at(ring, stopped_at, stopped, 4);

    ok = expect(count == 3, "stale stop read should return remaining audio") &&
        expect(stopped[0] == -0.25F, "stale stop read has wrong first sample") &&
        expect(stopped[1] == 0.25F, "stale stop read has wrong middle sample") &&
        expect(stopped[2] == 1.0F, "stale stop read has wrong last sample");

    sound_ring_buffer_destroy(ring);
    return ok;
}

int main(void) {
    SoundError error;
    SoundRingBuffer *ring = NULL;
    bool ok = true;

    sound_error_clear(&error);
    ok = sound_ring_buffer_create(16, &ring, &error);

    float samples[] = {-1.0F, -0.25F, 0.25F, 1.0F};
    if (ok) {
        sound_ring_buffer_write(ring, samples, 4);
    }

    char float_path[SOUND_RECORDING_PATH_CAPACITY] = "";
    char sample_path[SOUND_RECORDING_PATH_CAPACITY] = "";

    if (ok) {
        ok = save_test_recording(ring, float_path, &error) &&
            expect(strstr(float_path, "-32f.wav") != NULL, "float WAV path is unclear") &&
            check_wav(float_path, 16U);
    }

    if (ok) {
        ok = save_test_samples(samples, sample_path, &error) &&
            expect(strstr(sample_path, "-32f.wav") != NULL, "sample WAV path is unclear") &&
            check_wav(sample_path, 16U);
    }

    if (ok) {
        ok = check_stale_recording_stop_read(&error);
    }

    if (!ok) {
        fprintf(stderr, "%s\n", sound_error_message(&error));
    } else {
        printf("recording tests passed\n");
    }

    (void)remove(float_path);
    (void)remove(sample_path);
    sound_ring_buffer_destroy(ring);
    return ok ? 0 : 1;
}
