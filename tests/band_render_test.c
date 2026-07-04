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

int main(void) {
    SoundError error;
    sound_error_clear(&error);

    const double sample_rate = 48000.0;
    const uint64_t sample_count = 4096;
    float *input = calloc((size_t)sample_count, sizeof(float));
    float *selected = calloc((size_t)sample_count, sizeof(float));
    float *rows = calloc(96, sizeof(float));

    if (!input || !selected || !rows) {
        fprintf(stderr, "could not allocate band render test buffers\n");
        free(input);
        free(selected);
        free(rows);
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

    if (ok) {
        printf("band render tests passed\n");
    }

    free(input);
    free(selected);
    free(rows);
    return ok ? 0 : 1;
}
