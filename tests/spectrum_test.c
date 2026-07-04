#include "sounds/spectrum.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const double sample_rate = 48000.0;
static const double columns_per_second = 240.0;
static const uint64_t row_count = 512;
static const double min_hz = 10.0;
static const double max_hz = 24000.0;

typedef struct Target {
    double hz;
    uint64_t row;
    float peak_db;
    int64_t peak_offset;
} Target;

typedef struct ModeCase {
    const char *label;
    SoundSpectrumMode mode;
} ModeCase;

static double log2_double(double value) {
    return log(value) / log(2.0);
}

static uint64_t row_for_frequency(double hz) {
    double unit = (log2_double(max_hz) - log2_double(hz)) /
        (log2_double(max_hz) - log2_double(min_hz));
    int64_t row = (int64_t)llround(unit * (double)row_count - 0.5);

    if (row < 0) {
        return 0;
    }

    if (row >= (int64_t)row_count) {
        return row_count - 1U;
    }

    return (uint64_t)row;
}

int main(void) {
    SoundError error;
    SoundRingBuffer *ring = NULL;
    SoundSpectrumAnalyzer *analyzer = NULL;
    float *samples = NULL;
    float *rows = NULL;
    bool ok = true;

    sound_error_clear(&error);

    uint64_t sample_count = (uint64_t)(sample_rate * 3.0);
    uint64_t impulse_sample = (uint64_t)sample_rate;
    uint64_t column_samples = (uint64_t)llround(sample_rate / columns_per_second);

    samples = calloc((size_t)sample_count, sizeof(float));
    rows = malloc(sizeof(float) * (size_t)row_count);
    ok = samples != NULL && rows != NULL;

    if (ok) {
        samples[impulse_sample] = 1.0F;
        ok = sound_ring_buffer_create(sample_count, &ring, &error);
    }

    if (ok) {
        sound_ring_buffer_write(ring, samples, (uint32_t)sample_count);
        ok = sound_spectrum_analyzer_create(
            sample_rate,
            columns_per_second,
            &analyzer,
            &error
        );
    }

    Target targets[] = {
        {.hz = 20000.0, .peak_db = -1.0e30F},
        {.hz = 10000.0, .peak_db = -1.0e30F},
        {.hz = 1000.0, .peak_db = -1.0e30F},
        {.hz = 100.0, .peak_db = -1.0e30F},
        {.hz = 20.0, .peak_db = -1.0e30F},
        {.hz = 10.0, .peak_db = -1.0e30F},
    };
    uint64_t target_count = sizeof(targets) / sizeof(targets[0]);

    for (uint64_t i = 0; i < target_count; ++i) {
        targets[i].row = row_for_frequency(targets[i].hz);
    }

    int64_t first_center = (int64_t)impulse_sample - (int64_t)(sample_rate * 0.5);
    int64_t last_center = (int64_t)impulse_sample + (int64_t)(sample_rate * 0.5);

    for (int64_t center = first_center; ok && center <= last_center;
         center += (int64_t)column_samples) {
        ok = sound_spectrum_analyzer_column_db(
            analyzer,
            ring,
            (uint64_t)center,
            SOUND_SPECTRUM_TRANSIENT,
            rows,
            row_count,
            &error
        );

        for (uint64_t i = 0; ok && i < target_count; ++i) {
            float db = rows[targets[i].row];

            if (db > targets[i].peak_db) {
                targets[i].peak_db = db;
                targets[i].peak_offset = center - (int64_t)impulse_sample;
            }
        }
    }

    for (uint64_t i = 0; ok && i < target_count; ++i) {
        double offset_ms = (double)targets[i].peak_offset / sample_rate * 1000.0;

        printf(
            "transient impulse %.0f Hz peak offset %.1f ms at %.1f dB\n",
            targets[i].hz,
            offset_ms,
            targets[i].peak_db
        );

        if (llabs(targets[i].peak_offset) > (int64_t)column_samples) {
            fprintf(stderr, "transient impulse peak is not time-aligned\n");
            ok = false;
        }
    }

    static const ModeCase modes[] = {
        {.label = "transient", .mode = SOUND_SPECTRUM_TRANSIENT},
        {.label = "reassigned", .mode = SOUND_SPECTRUM_REASSIGNED},
        {.label = "squeezed", .mode = SOUND_SPECTRUM_SQUEEZED},
        {.label = "superlet", .mode = SOUND_SPECTRUM_SUPERLET},
        {.label = "multitaper", .mode = SOUND_SPECTRUM_MULTITAPER},
        {.label = "S-transform", .mode = SOUND_SPECTRUM_S_TRANSFORM},
        {.label = "sparse", .mode = SOUND_SPECTRUM_SPARSE},
    };
    uint64_t mode_count = sizeof(modes) / sizeof(modes[0]);

    for (uint64_t mode_index = 0; ok && mode_index < mode_count; ++mode_index) {
        ok = sound_spectrum_analyzer_column_db(
            analyzer,
            ring,
            impulse_sample,
            modes[mode_index].mode,
            rows,
            row_count,
            &error
        );

        float maximum = -1.0e30F;
        for (uint64_t row = 0; ok && row < row_count; ++row) {
            if (!isfinite(rows[row])) {
                fprintf(stderr, "%s spectrum produced a non-finite row\n", modes[mode_index].label);
                ok = false;
            }

            if (rows[row] > maximum) {
                maximum = rows[row];
            }
        }

        if (ok && maximum <= -119.0F) {
            fprintf(stderr, "%s spectrum did not show impulse energy\n", modes[mode_index].label);
            ok = false;
        }
    }

    if (!ok) {
        fprintf(stderr, "%s\n", sound_error_message(&error));
    } else {
        printf("spectrum tests passed\n");
    }

    sound_spectrum_analyzer_destroy(analyzer);
    sound_ring_buffer_destroy(ring);
    free(samples);
    free(rows);
    return ok ? 0 : 1;
}
