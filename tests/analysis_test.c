#include "sounds/analysis.h"
#include "sounds/error.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const double pi_value = 3.14159265358979323846264338327950288;
static const uint64_t row_count = 1024;
static const uint64_t chunk_capacity = 1024;

typedef struct Peak {
    uint64_t row;
    double hz;
    float db;
} Peak;

static double log2_double(double value) {
    return log(value) / log(2.0);
}

static bool create_analyzer(
    double sample_rate,
    SoundWaveletAnalyzer **analyzer,
    SoundError *error
) {
    if (!sound_wavelet_analyzer_create(sample_rate, analyzer, error)) {
        fprintf(stderr, "%s\n", sound_error_message(error));
        return false;
    }

    printf(
        "analyzer %.0f Hz: %.2f-%.0f Hz, %llu octaves, %llu voices\n",
        sample_rate,
        sound_wavelet_analyzer_min_frequency(*analyzer),
        sound_wavelet_analyzer_max_frequency(*analyzer),
        (unsigned long long)sound_wavelet_analyzer_octave_count(*analyzer),
        (unsigned long long)sound_wavelet_analyzer_voice_count(*analyzer)
    );
    return true;
}

static bool snapshot_rows(
    SoundWaveletAnalyzer *analyzer,
    bool synchrosqueezed,
    float *rows,
    SoundError *error
) {
    sound_wavelet_analyzer_set_synchrosqueezed(analyzer, synchrosqueezed);
    if (!sound_wavelet_analyzer_snapshot_db(analyzer, rows, row_count, error)) {
        fprintf(stderr, "%s\n", sound_error_message(error));
        return false;
    }

    return true;
}

static Peak find_peak(
    const SoundWaveletAnalyzer *analyzer,
    const float *rows,
    double low_hz,
    double high_hz
) {
    Peak peak = {
        .row = 0,
        .hz = 0.0,
        .db = -1.0e30F,
    };

    for (uint64_t row = 0; row < row_count; ++row) {
        double hz = sound_wavelet_analyzer_frequency_for_row(analyzer, row, row_count);

        if (hz < low_hz || hz > high_hz) {
            continue;
        }

        if (rows[row] > peak.db) {
            peak.row = row;
            peak.hz = hz;
            peak.db = rows[row];
        }
    }

    return peak;
}

static double peak_width_octaves(
    const SoundWaveletAnalyzer *analyzer,
    const float *rows,
    Peak peak,
    float drop_db
) {
    float threshold = peak.db - drop_db;
    uint64_t first = peak.row;
    uint64_t last = peak.row;

    while (first > 0 && rows[first - 1U] >= threshold) {
        --first;
    }

    while (last + 1U < row_count && rows[last + 1U] >= threshold) {
        ++last;
    }

    double high = sound_wavelet_analyzer_frequency_for_row(analyzer, first, row_count);
    double low = sound_wavelet_analyzer_frequency_for_row(analyzer, last, row_count);

    return fabs(log2_double(high / low));
}

static bool push_tone(
    SoundWaveletAnalyzer *analyzer,
    double sample_rate,
    double hz,
    double seconds,
    double amplitude,
    SoundError *error
) {
    float *chunk = malloc(sizeof(float) * (size_t)chunk_capacity);
    if (!chunk) {
        fprintf(stderr, "could not allocate test tone chunk\n");
        return false;
    }

    uint64_t total = (uint64_t)llround(seconds * sample_rate);
    uint64_t written = 0;
    double phase = 0.0;
    double phase_step = 2.0 * pi_value * hz / sample_rate;

    while (written < total) {
        uint64_t count = total - written;
        if (count > chunk_capacity) {
            count = chunk_capacity;
        }

        for (uint64_t i = 0; i < count; ++i) {
            chunk[i] = (float)(amplitude * sin(phase));
            phase += phase_step;

            if (phase > 2.0 * pi_value) {
                phase -= 2.0 * pi_value;
            }
        }

        if (!sound_wavelet_analyzer_push(analyzer, chunk, count, error)) {
            fprintf(stderr, "%s\n", sound_error_message(error));
            free(chunk);
            return false;
        }

        written += count;
    }

    free(chunk);
    return true;
}

static bool push_two_tone(
    SoundWaveletAnalyzer *analyzer,
    double sample_rate,
    double first_hz,
    double second_hz,
    double seconds,
    SoundError *error
) {
    float *chunk = malloc(sizeof(float) * (size_t)chunk_capacity);
    if (!chunk) {
        fprintf(stderr, "could not allocate two-tone chunk\n");
        return false;
    }

    uint64_t total = (uint64_t)llround(seconds * sample_rate);
    uint64_t written = 0;
    double first_phase = 0.0;
    double second_phase = 0.0;
    double first_step = 2.0 * pi_value * first_hz / sample_rate;
    double second_step = 2.0 * pi_value * second_hz / sample_rate;

    while (written < total) {
        uint64_t count = total - written;
        if (count > chunk_capacity) {
            count = chunk_capacity;
        }

        for (uint64_t i = 0; i < count; ++i) {
            double sample = 0.35 * sin(first_phase) + 0.35 * sin(second_phase);
            chunk[i] = (float)sample;
            first_phase += first_step;
            second_phase += second_step;

            if (first_phase > 2.0 * pi_value) {
                first_phase -= 2.0 * pi_value;
            }

            if (second_phase > 2.0 * pi_value) {
                second_phase -= 2.0 * pi_value;
            }
        }

        if (!sound_wavelet_analyzer_push(analyzer, chunk, count, error)) {
            fprintf(stderr, "%s\n", sound_error_message(error));
            free(chunk);
            return false;
        }

        written += count;
    }

    free(chunk);
    return true;
}

static bool check_peak(
    const char *label,
    double expected_hz,
    double tolerance_fraction,
    Peak peak
) {
    double error_fraction = fabs(peak.hz - expected_hz) / expected_hz;

    printf(
        "%s: peak %.4f Hz at %.1f dBFS-like, expected %.4f Hz, error %.2f%%\n",
        label,
        peak.hz,
        peak.db,
        expected_hz,
        error_fraction * 100.0
    );

    if (error_fraction > tolerance_fraction) {
        fprintf(stderr, "%s failed: peak frequency is outside tolerance\n", label);
        return false;
    }

    if (peak.db < -100.0F) {
        fprintf(stderr, "%s failed: peak was not meaningfully above the floor\n", label);
        return false;
    }

    return true;
}

static bool test_single_tone(
    const char *label,
    double sample_rate,
    double hz,
    double seconds,
    double tolerance_fraction,
    double search_low,
    double search_high
) {
    SoundError error;
    SoundWaveletAnalyzer *analyzer = NULL;
    float *rows = malloc(sizeof(float) * (size_t)row_count);
    bool ok = rows != NULL;

    sound_error_clear(&error);
    ok = ok && create_analyzer(sample_rate, &analyzer, &error);
    ok = ok && push_tone(analyzer, sample_rate, hz, seconds, 0.65, &error);
    ok = ok && snapshot_rows(analyzer, true, rows, &error);

    if (ok) {
        Peak peak = find_peak(analyzer, rows, search_low, search_high);
        ok = check_peak(label, hz, tolerance_fraction, peak);
    }

    sound_wavelet_analyzer_destroy(analyzer);
    free(rows);
    return ok;
}

static bool test_two_tone(void) {
    SoundError error;
    SoundWaveletAnalyzer *analyzer = NULL;
    float *rows = malloc(sizeof(float) * (size_t)row_count);
    bool ok = rows != NULL;

    sound_error_clear(&error);
    ok = ok && create_analyzer(12000.0, &analyzer, &error);
    ok = ok && push_two_tone(analyzer, 12000.0, 30.0, 39.0, 12.0, &error);
    ok = ok && snapshot_rows(analyzer, true, rows, &error);

    if (ok) {
        Peak one = find_peak(analyzer, rows, 27.0, 33.3);
        Peak two = find_peak(analyzer, rows, 35.4, 43.2);
        Peak valley = find_peak(analyzer, rows, 33.3, 35.4);

        ok = check_peak("two-tone 30 Hz ridge", 30.0, 0.06, one);
        ok = check_peak("two-tone 39 Hz ridge", 39.0, 0.06, two) && ok;

        printf(
            "two-tone valley: %.4f Hz at %.1f dBFS-like\n",
            valley.hz,
            valley.db
        );

        if (valley.db > fminf(one.db, two.db) - 3.0F) {
            fprintf(stderr, "two-tone failed: ridges are not cleanly separated\n");
            ok = false;
        }
    }

    sound_wavelet_analyzer_destroy(analyzer);
    free(rows);
    return ok;
}

static bool test_no_low_alias(void) {
    SoundError error;
    SoundWaveletAnalyzer *analyzer = NULL;
    float *rows = malloc(sizeof(float) * (size_t)row_count);
    bool ok = rows != NULL;

    sound_error_clear(&error);
    ok = ok && create_analyzer(48000.0, &analyzer, &error);
    ok = ok && push_tone(analyzer, 48000.0, 20000.0, 2.0, 0.65, &error);
    ok = ok && snapshot_rows(analyzer, false, rows, &error);

    if (ok) {
        Peak high = find_peak(analyzer, rows, 15000.0, 20000.0);
        Peak low = find_peak(analyzer, rows, 20.0, 200.0);

        printf(
            "20 kHz alias check: high %.1f Hz %.1f dB, low ghost %.3f Hz %.1f dB\n",
            high.hz,
            high.db,
            low.hz,
            low.db
        );

        if (high.db < -100.0F || low.db > high.db - 25.0F) {
            fprintf(stderr, "20 kHz alias check failed\n");
            ok = false;
        }
    }

    sound_wavelet_analyzer_destroy(analyzer);
    free(rows);
    return ok;
}

static bool test_chirp(void) {
    SoundError error;
    SoundWaveletAnalyzer *analyzer = NULL;
    float *raw_rows = malloc(sizeof(float) * (size_t)row_count);
    float *squeezed_rows = malloc(sizeof(float) * (size_t)row_count);
    float *chunk = malloc(sizeof(float) * (size_t)chunk_capacity);
    bool ok = raw_rows != NULL && squeezed_rows != NULL && chunk != NULL;

    const double sample_rate = 48000.0;
    const double seconds = 8.0;
    const double start_hz = 250.0;
    const double end_hz = 6000.0;
    const uint64_t total = (uint64_t)llround(seconds * sample_rate);
    const uint64_t snapshot_interval = (uint64_t)llround(sample_rate * 0.25);

    sound_error_clear(&error);
    ok = ok && create_analyzer(sample_rate, &analyzer, &error);

    uint64_t written = 0;
    uint64_t next_snapshot = (uint64_t)llround(sample_rate * 1.0);
    double phase = 0.0;
    double previous_peak_hz = 0.0;
    double raw_width_sum = 0.0;
    double squeezed_width_sum = 0.0;
    uint64_t snapshots = 0;
    uint64_t monotonic_misses = 0;

    while (ok && written < total) {
        uint64_t count = total - written;
        if (count > chunk_capacity) {
            count = chunk_capacity;
        }

        for (uint64_t i = 0; i < count; ++i) {
            double t = (double)(written + i) / sample_rate;
            double unit = t / seconds;
            double hz = start_hz + (end_hz - start_hz) * unit;

            chunk[i] = (float)(0.65 * sin(phase));
            phase += 2.0 * pi_value * hz / sample_rate;

            if (phase > 2.0 * pi_value) {
                phase = fmod(phase, 2.0 * pi_value);
            }
        }

        if (!sound_wavelet_analyzer_push(analyzer, chunk, count, &error)) {
            fprintf(stderr, "%s\n", sound_error_message(&error));
            ok = false;
            break;
        }

        written += count;

        while (ok && written >= next_snapshot && next_snapshot < total) {
            ok = snapshot_rows(analyzer, false, raw_rows, &error);
            ok = ok && snapshot_rows(analyzer, true, squeezed_rows, &error);

            if (!ok) {
                break;
            }

            Peak raw_peak = find_peak(analyzer, raw_rows, 150.0, 7000.0);
            Peak squeezed_peak = find_peak(analyzer, squeezed_rows, 150.0, 7000.0);
            double raw_width = peak_width_octaves(analyzer, raw_rows, raw_peak, 6.0F);
            double squeezed_width =
                peak_width_octaves(analyzer, squeezed_rows, squeezed_peak, 6.0F);

            raw_width_sum += raw_width;
            squeezed_width_sum += squeezed_width;

            if (previous_peak_hz > 0.0 && squeezed_peak.hz < previous_peak_hz * 0.92) {
                ++monotonic_misses;
            }

            previous_peak_hz = squeezed_peak.hz;
            ++snapshots;
            next_snapshot += snapshot_interval;
        }
    }

    if (ok) {
        double raw_average = raw_width_sum / (double)snapshots;
        double squeezed_average = squeezed_width_sum / (double)snapshots;

        printf(
            "chirp: %llu snapshots, raw avg width %.4f oct, SST avg width %.4f oct, final peak %.1f Hz\n",
            (unsigned long long)snapshots,
            raw_average,
            squeezed_average,
            previous_peak_hz
        );

        if (snapshots < 8U ||
            monotonic_misses > 2U ||
            previous_peak_hz < 3500.0 ||
            squeezed_average >= raw_average * 0.85) {
            fprintf(stderr, "chirp failed: ridge was not cleanly sharpened and rising\n");
            ok = false;
        }
    }

    sound_wavelet_analyzer_destroy(analyzer);
    free(raw_rows);
    free(squeezed_rows);
    free(chunk);
    return ok;
}

int main(void) {
    bool ok = true;

    ok = test_single_tone("30 Hz tone", 12000.0, 30.0, 8.0, 0.05, 25.0, 36.0) && ok;
    ok = test_single_tone("10 kHz tone", 48000.0, 10000.0, 2.0, 0.04, 8000.0, 12000.0) && ok;
    ok = test_two_tone() && ok;
    ok = test_chirp() && ok;
    ok = test_no_low_alias() && ok;

    if (!ok) {
        return 1;
    }

    printf("analysis tests passed\n");
    return 0;
}
