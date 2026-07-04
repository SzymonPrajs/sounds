#include "sounds/recording.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

static const char recording_directory[] = "recordings";
static const char recording_file_prefix[] = "sounds-";
static const char recording_file_extension[] = ".f32";

static bool ensure_recording_directory(SoundError *error) {
    if (mkdir(recording_directory, 0755) == 0 || errno == EEXIST) {
        return true;
    }

    sound_error_set(error, "could not create %s", recording_directory);
    return false;
}

static bool write_raw_samples(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    SoundError *error
) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        sound_error_set(error, "could not open %s", path);
        return false;
    }

    size_t written = fwrite(samples, sizeof(float), (size_t)sample_count, file);
    bool closed = fclose(file) == 0;

    if (written != (size_t)sample_count || !closed) {
        sound_error_set(error, "could not write %s", path);
        return false;
    }

    return true;
}

static bool build_recording_path(char *path, size_t path_size, SoundError *error) {
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
        "%s/%s%s%s",
        recording_directory,
        recording_file_prefix,
        timestamp,
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
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
) {
    *sample_count = 0;
    path[0] = '\0';

    uint64_t capacity = sound_ring_buffer_capacity(ring);
    if (capacity == 0) {
        return true;
    }

    float *samples = malloc(sizeof(float) * (size_t)capacity);
    if (!samples) {
        sound_error_set(error, "could not allocate raw recording buffer");
        return false;
    }

    uint64_t count = sound_ring_buffer_read_latest(ring, samples, capacity);
    bool ok = ensure_recording_directory(error) &&
        build_recording_path(path, path_size, error) &&
        write_raw_samples(path, samples, count, error);

    free(samples);
    *sample_count = ok ? count : 0;
    return ok;
}
