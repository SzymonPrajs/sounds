#include "sounds/band_render.h"
#include "sounds/error.h"
#include "sounds/offline_spectrum.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const double pi_value = 3.14159265358979323846264338327950288;

static bool expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
    }

    return condition;
}

static double tone_projection(
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    double hz
) {
    double sine = 0.0;
    double cosine = 0.0;

    for (uint64_t i = 0; i < sample_count; ++i) {
        double phase = 2.0 * pi_value * hz * (double)i / sample_rate;
        sine += (double)samples[i] * sin(phase);
        cosine += (double)samples[i] * cos(phase);
    }

    return sqrt(sine * sine + cosine * cosine) / (double)sample_count;
}

static double absolute_peak(const float *samples, uint64_t sample_count) {
    double peak = 0.0;

    for (uint64_t i = 0; i < sample_count; ++i) {
        double value = fabs((double)samples[i]);
        if (isfinite(value) && value > peak) {
            peak = value;
        }
    }

    return peak;
}

int main(void) {
    SoundError error;
    sound_error_clear(&error);

    const double sample_rate = 48000.0;
    const uint64_t sample_count = 4096;
    float *input = calloc((size_t)sample_count, sizeof(float));
    float *selected = calloc((size_t)sample_count, sizeof(float));
    float *rows = calloc(96, sizeof(float));
    float *columns = calloc(96 * 24, sizeof(float));

    if (!input || !selected || !rows || !columns) {
        fprintf(stderr, "could not allocate band render test buffers\n");
        free(input);
        free(selected);
        free(rows);
        free(columns);
        return 1;
    }

    for (uint64_t i = 0; i < sample_count; ++i) {
        double t = (double)i / sample_rate;
        input[i] = (float)(0.5 * sin(2.0 * pi_value * 500.0 * t) +
            0.5 * sin(2.0 * pi_value * 4000.0 * t));
    }

    SoundBandRenderRequest request = {
        .sample_rate = sample_rate,
        .low_hz = 400.0,
        .high_hz = 700.0,
        .iterations = 1,
    };

    bool ok = sound_band_render(
        input,
        sample_count,
        &request,
        SOUND_BAND_RENDER_FFT_MASK,
        selected,
        &error
    );

    ok = expect(ok, sound_error_message(&error));
    double low = tone_projection(selected, sample_count, sample_rate, 500.0);
    double high = tone_projection(selected, sample_count, sample_rate, 4000.0);
    ok = expect(low > high * 8.0, "FFT mask did not isolate the low tone") && ok;

    ok = expect(
        sound_band_render_method_count() == 10,
        "unexpected band render method count"
    ) && ok;

    for (uint64_t i = 0; i < sample_count; ++i) {
        selected[i] = 0.0F;
    }
    selected[0] = 12.0F;
    selected[1] = -6.0F;
    selected[32] = NAN;
    selected[64] = 4.0F;
    selected[sample_count - 1U] = -12.0F;

    sound_band_render_sanitize_output(selected, sample_count, sample_rate, input);
    ok = expect(selected[0] == 0.0F, "render guard did not mute the start edge") && ok;
    ok = expect(
        selected[sample_count - 1U] == 0.0F,
        "render guard did not mute the end edge"
    ) && ok;
    ok = expect(isfinite(selected[32]), "render guard left a non-finite sample") && ok;
    ok = expect(
        absolute_peak(selected, sample_count) <= 0.981,
        "render guard did not bound the generated peak"
    ) && ok;

    ok = expect(
        sound_offline_spectrum_db(
            input,
            sample_count,
            sample_rate,
            20.0,
            24000.0,
            rows,
            96,
            &error
        ),
        sound_error_message(&error)
    ) && ok;

    ok = expect(
        sound_offline_spectrogram_db(
            input,
            sample_count,
            sample_rate,
            20.0,
            24000.0,
            columns,
            96,
            24,
            &error
        ),
        sound_error_message(&error)
    ) && ok;

    float maximum = -1.0e30F;
    for (uint64_t i = 0; ok && i < 96U * 24U; ++i) {
        if (!isfinite(columns[i])) {
            ok = expect(false, "offline spectrogram produced a non-finite cell");
            break;
        }

        if (columns[i] > maximum) {
            maximum = columns[i];
        }
    }
    ok = expect(maximum > -119.0F, "offline spectrogram did not show test energy") && ok;

    if (ok) {
        printf("band render tests passed\n");
    }

    free(input);
    free(selected);
    free(rows);
    free(columns);
    return ok ? 0 : 1;
}
