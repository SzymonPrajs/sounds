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

static bool file_size(const char *path, long *size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "could not open %s\n", path);
        return false;
    }

    bool ok = fseek(file, 0, SEEK_END) == 0;
    long end = ok ? ftell(file) : -1;
    bool closed = fclose(file) == 0;

    *size = end;
    return expect(ok && end >= 0 && closed, "could not measure recording file");
}

static bool save_test_recording(
    const SoundRingBuffer *ring,
    SoundRecordingFormat format,
    char *path,
    SoundError *error
) {
    uint64_t count = 0;

    if (!sound_recording_save_recent(
            ring,
            4,
            48000.0,
            format,
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

static bool check_wav(
    const char *path,
    uint16_t expected_format,
    uint16_t expected_bits,
    uint32_t expected_data_bytes
) {
    uint8_t header[56];
    bool has_fact = expected_format == 3U;
    size_t data_chunk = has_fact ? 48U : 36U;
    size_t header_size = has_fact ? 56U : 44U;

    if (!read_file_prefix(path, header, header_size)) {
        return false;
    }

    return expect(memcmp(header, "RIFF", 4) == 0, "WAV file is missing RIFF") &&
        expect(memcmp(header + 8, "WAVE", 4) == 0, "WAV file is missing WAVE") &&
        expect(memcmp(header + 12, "fmt ", 4) == 0, "WAV file is missing fmt") &&
        expect(read_u16_le(header + 20) == expected_format, "WAV format tag is wrong") &&
        expect(read_u16_le(header + 22) == 1U, "WAV channel count is wrong") &&
        expect(read_u32_le(header + 24) == 48000U, "WAV sample rate is wrong") &&
        expect(read_u16_le(header + 34) == expected_bits, "WAV bit depth is wrong") &&
        expect(!has_fact || memcmp(header + 36, "fact", 4) == 0, "float WAV is missing fact") &&
        expect(memcmp(header + data_chunk, "data", 4) == 0, "WAV file is missing data") &&
        expect(
            read_u32_le(header + data_chunk + 4U) == expected_data_bytes,
            "WAV data size is wrong"
        );
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
    char pcm_path[SOUND_RECORDING_PATH_CAPACITY] = "";
    char raw_path[SOUND_RECORDING_PATH_CAPACITY] = "";

    if (ok) {
        ok = save_test_recording(ring, SOUND_RECORDING_WAV_FLOAT32, float_path, &error) &&
            expect(strstr(float_path, "-32f.wav") != NULL, "float WAV path is unclear") &&
            check_wav(float_path, 3U, 32U, 16U);
    }

    if (ok) {
        ok = save_test_recording(ring, SOUND_RECORDING_WAV_PCM16, pcm_path, &error) &&
            expect(strstr(pcm_path, "-16.wav") != NULL, "16-bit WAV path is unclear") &&
            check_wav(pcm_path, 1U, 16U, 8U);
    }

    if (ok) {
        long raw_size = 0;
        ok = save_test_recording(ring, SOUND_RECORDING_RAW_FLOAT32, raw_path, &error) &&
            expect(strstr(raw_path, "-raw.f32") != NULL, "raw path is unclear") &&
            file_size(raw_path, &raw_size) &&
            expect(raw_size == 16, "raw recording size is wrong");
    }

    if (!ok) {
        fprintf(stderr, "%s\n", sound_error_message(&error));
    } else {
        printf("recording tests passed\n");
    }

    (void)remove(float_path);
    (void)remove(pcm_path);
    (void)remove(raw_path);
    sound_ring_buffer_destroy(ring);
    return ok ? 0 : 1;
}
