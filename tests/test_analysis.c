#include "sounds/analysis.h"
#include "sounds/error.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void build_temp_path(char *buffer, size_t size, const char *directory, const char *name) {
    int written = snprintf(buffer, size, "%s/%s", directory, name);
    expect(written > 0 && (size_t)written < size, "temporary path fits");
}

static void test_power_helpers(void) {
    expect(!sound_is_power_of_two(0), "zero is not a power of two");
    expect(sound_is_power_of_two(1), "one is a power of two");
    expect(sound_is_power_of_two(16384), "16384 is a power of two");
    expect(!sound_is_power_of_two(12345), "12345 is not a power of two");
    expect(sound_previous_power_of_two(0) == 0, "previous power for zero");
    expect(sound_previous_power_of_two(1) == 1, "previous power for one");
    expect(sound_previous_power_of_two(5000) == 4096, "previous power below 5000");
}

static void test_raw_and_levels(const char *directory) {
    char raw_path[256];
    char levels_path[256];
    build_temp_path(raw_path, sizeof(raw_path), directory, "raw.f32");
    build_temp_path(levels_path, sizeof(levels_path), directory, "levels.csv");

    float samples[4] = {1.0F, -1.0F, 0.5F, -0.5F};
    SoundError error;
    sound_error_clear(&error);

    expect(sound_write_raw_f32(raw_path, samples, 4, &error), sound_error_message(&error));

    FILE *raw = fopen(raw_path, "rb");
    expect(raw != NULL, "raw output opens");
    if (raw) {
        expect(fseek(raw, 0, SEEK_END) == 0, "raw seek succeeds");
        expect(ftell(raw) == 16L, "raw output has four float samples");
        (void)fclose(raw);
    }

    expect(
        sound_write_levels_csv(levels_path, samples, 4, 4.0, 4, &error),
        sound_error_message(&error)
    );

    FILE *levels = fopen(levels_path, "r");
    expect(levels != NULL, "levels output opens");
    if (levels) {
        char header[64];
        double seconds = 0.0;
        double rms = 0.0;
        double peak = 0.0;

        expect(fgets(header, sizeof(header), levels) != NULL, "levels header reads");
        expect(strcmp(header, "time_seconds,rms,peak\n") == 0, "levels header matches");
        expect(fscanf(levels, "%lf,%lf,%lf", &seconds, &rms, &peak) == 3, "levels row parses");
        expect(fabs(seconds) < 1.0e-9, "levels row starts at zero");
        expect(fabs(rms - sqrt(0.625)) < 1.0e-6, "levels RMS matches");
        expect(fabs(peak - 1.0) < 1.0e-6, "levels peak matches");
        (void)fclose(levels);
    }
}

static void test_spectrum_peak(const char *directory) {
    enum {
        sample_count = 8192,
    };

    const double sample_rate = 48000.0;
    const double tone_hz = 1000.0;
    const double two_pi = 6.28318530717958647692;

    float *samples = malloc(sizeof(float) * sample_count);
    expect(samples != NULL, "spectrum samples allocate");
    if (!samples) {
        return;
    }

    for (int i = 0; i < sample_count; ++i) {
        samples[i] = sinf((float)(two_pi * tone_hz * (double)i / sample_rate));
    }

    char spectrum_path[256];
    build_temp_path(spectrum_path, sizeof(spectrum_path), directory, "spectrum.csv");

    SoundError error;
    sound_error_clear(&error);
    expect(
        sound_write_spectrum_csv(spectrum_path, samples, sample_count, sample_rate, 8192, &error),
        sound_error_message(&error)
    );

    FILE *spectrum = fopen(spectrum_path, "r");
    expect(spectrum != NULL, "spectrum output opens");
    if (spectrum) {
        char header[64];
        double hz = 0.0;
        double db = 0.0;
        double peak_hz = 0.0;
        double peak_db = -1000.0;

        expect(fgets(header, sizeof(header), spectrum) != NULL, "spectrum header reads");
        expect(strcmp(header, "hz,dbfs\n") == 0, "spectrum header matches");

        while (fscanf(spectrum, "%lf,%lf", &hz, &db) == 2) {
            if (db > peak_db) {
                peak_db = db;
                peak_hz = hz;
            }
        }

        expect(fabs(peak_hz - tone_hz) < sample_rate / 8192.0 * 2.0, "spectrum peak near tone");
        (void)fclose(spectrum);
    }

    free(samples);
}

int main(void) {
    char directory[] = "/tmp/sounds-analysis-test-XXXXXX";
    char *created = mkdtemp(directory);

    expect(created != NULL, "temporary directory is created");
    if (!created) {
        return 1;
    }

    test_power_helpers();
    test_raw_and_levels(created);
    test_spectrum_peak(created);

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }

    fprintf(stderr, "analysis tests passed\n");
    return 0;
}
