#include "sounds/recording.h"

#include "sounds/defer.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

static const char recording_directory[] = "recordings";
static const char recording_file_prefix[] = "sounds-";
static const char recording_file_suffix[] = "32f";
static const char recording_file_extension[] = ".wav";

static bool ensure_recording_directory(SoundError *error) {
    if (mkdir(recording_directory, 0o755) == 0 || errno == EEXIST) {
        return true;
    }

    sound_error_set(error, "could not create %s", recording_directory);
    return false;
}

static bool write_bytes(FILE *file, const void *bytes, size_t size) {
    return fwrite(bytes, 1, size, file) == size;
}

static bool write_u16_le(FILE *file, uint16_t value) {
    uint8_t bytes[2] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
    };

    return write_bytes(file, bytes, sizeof(bytes));
}

static bool write_u32_le(FILE *file, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
        (uint8_t)((value >> 16) & 0xFFU),
        (uint8_t)((value >> 24) & 0xFFU),
    };

    return write_bytes(file, bytes, sizeof(bytes));
}

static bool write_float32_samples(
    FILE *file,
    const float *samples,
    uint64_t sample_count
) {
    for (uint64_t i = 0; i < sample_count; ++i) {
        union {
            float sample;
            uint32_t bits;
        } value = {.sample = samples[i]};

        if (!write_u32_le(file, value.bits)) {
            return false;
        }
    }

    return true;
}

static bool wav_payload_size(
    uint64_t sample_count,
    uint32_t *data_bytes,
    SoundError *error
) {
    if (sample_count > UINT32_MAX / 4U) {
        sound_error_set(error, "recording is too large for WAV");
        return false;
    }

    uint64_t bytes = sample_count * 4U;
    if (bytes > UINT32_MAX) {
        sound_error_set(error, "recording is too large for WAV");
        return false;
    }

    *data_bytes = (uint32_t)bytes;
    return true;
}

static bool write_wav_samples(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    SoundError *error
) {
    uint32_t bits_per_sample = 32U;
    uint32_t data_bytes = 0;

    if (sample_rate <= 0.0 || sample_rate > (double)UINT32_MAX ||
        !wav_payload_size(sample_count, &data_bytes, error)) {
        if (error && error->message[0] == '\0') {
            sound_error_set(error, "invalid WAV sample rate");
        }
        return false;
    }

    uint16_t audio_format = 3U;
    uint16_t channel_count = 1U;
    uint16_t block_align = (uint16_t)(channel_count * (bits_per_sample / 8U));
    uint32_t rate = (uint32_t)lrint(sample_rate);
    uint64_t byte_rate_64 = (uint64_t)rate * (uint64_t)block_align;
    uint32_t fact_size = 12U;
    uint64_t riff_size_64 =
        4U + (8U + 16U) + fact_size + (8U + (uint64_t)data_bytes);

    if (byte_rate_64 > UINT32_MAX || riff_size_64 > UINT32_MAX) {
        sound_error_set(error, "recording is too large for WAV");
        return false;
    }

    bool ok = false;
    {
        FILE *file = fopen(path, "wb");
        if (!file) {
            sound_error_set(error, "could not open %s", path);
            return false;
        }

        defer {
            ok = fclose(file) == 0 && ok;
        }

        ok =
            write_bytes(file, "RIFF", 4) &&
            write_u32_le(file, (uint32_t)riff_size_64) &&
            write_bytes(file, "WAVE", 4) &&
            write_bytes(file, "fmt ", 4) &&
            write_u32_le(file, 16U) &&
            write_u16_le(file, audio_format) &&
            write_u16_le(file, channel_count) &&
            write_u32_le(file, rate) &&
            write_u32_le(file, (uint32_t)byte_rate_64) &&
            write_u16_le(file, block_align) &&
            write_u16_le(file, (uint16_t)bits_per_sample);

        ok = ok &&
            write_bytes(file, "fact", 4) &&
            write_u32_le(file, 4U) &&
            write_u32_le(file, (uint32_t)sample_count) &&
            write_bytes(file, "data", 4) &&
            write_u32_le(file, data_bytes) &&
            write_float32_samples(file, samples, sample_count);
    }

    if (!ok) {
        sound_error_set(error, "could not write %s", path);
        return false;
    }

    return true;
}

static bool build_recording_path(
    char *path,
    size_t path_size,
    SoundError *error
) {
    time_t now = time(NULL);
    struct tm local_time;
    char timestamp[32];

    if (now == (time_t)-1 || !localtime_r(&now, &local_time)) {
        sound_error_set(error, "could not read current time");
        return false;
    }

    if (strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time) == 0) {
        sound_error_set(error, "could not format recording timestamp");
        return false;
    }

    int written = snprintf(
        path,
        path_size,
        "%s/%s%s-%s%s",
        recording_directory,
        recording_file_prefix,
        timestamp,
        recording_file_suffix,
        recording_file_extension
    );

    if (written <= 0 || (size_t)written >= path_size) {
        sound_error_set(error, "recording path is too long");
        return false;
    }

    return true;
}

bool sound_recording_save_latest(
    const SoundRingBuffer *ring,
    double sample_rate,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
) {
    return sound_recording_save_recent(
        ring,
        sound_ring_buffer_capacity(ring),
        sample_rate,
        sample_count,
        path,
        path_size,
        error
    );
}

bool sound_recording_save_recent(
    const SoundRingBuffer *ring,
    uint64_t requested_samples,
    double sample_rate,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
) {
    *sample_count = 0;
    path[0] = '\0';

    uint64_t capacity = sound_ring_buffer_capacity(ring);
    if (capacity == 0 || requested_samples == 0) {
        return true;
    }

    uint64_t wanted = requested_samples < capacity ? requested_samples : capacity;
    float *samples = malloc(sizeof(float) * (size_t)wanted);
    if (!samples) {
        sound_error_set(error, "could not allocate raw recording buffer");
        return false;
    }
    defer {
        free(samples);
    }

    uint64_t count = sound_ring_buffer_read_latest(ring, samples, wanted);
    bool ok = ensure_recording_directory(error) &&
        build_recording_path(path, path_size, error) &&
        write_wav_samples(path, samples, count, sample_rate, error);

    *sample_count = ok ? count : 0;
    return ok;
}
