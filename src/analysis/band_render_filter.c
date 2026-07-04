#include "band_render_internal.h"

#include "sounds/defer.h"

#include <Accelerate/Accelerate.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

static double sinc(double value) {
    if (fabs(value) < 1.0e-12) {
        return 1.0;
    }

    return sin(sound_band_render_pi * value) / (sound_band_render_pi * value);
}

static uint64_t fir_tap_count(double sample_rate, double low_hz, double high_hz) {
    double transition = fmax(30.0, fmin(low_hz, sample_rate * 0.5 - high_hz));
    uint64_t taps = (uint64_t)ceil(sample_rate * 3.0 / transition);

    if (taps < 65U) {
        taps = 65U;
    }

    if (taps > 513U) {
        taps = 513U;
    }

    if ((taps & 1U) == 0U) {
        ++taps;
    }

    return taps;
}

static float fir_sample(
    const float *input,
    const float *kernel,
    uint64_t sample_count,
    uint64_t taps,
    uint64_t center,
    uint64_t sample
) {
    uint64_t source_center = sample + center;
    uint64_t k_min = source_center >= sample_count ?
        source_center - sample_count + 1U :
        0U;
    uint64_t k_max = source_center < taps ? source_center : taps - 1U;

    if (k_min > k_max) {
        return 0.0F;
    }

    uint64_t count = k_max - k_min + 1U;
    uint64_t source_index = source_center - k_min;
    float value = 0.0F;

    vDSP_dotpr(
        kernel + k_min,
        1,
        input + source_index,
        -1,
        &value,
        (vDSP_Length)count
    );
    return value;
}

bool sound_band_render_fir_linear(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    sound_error_clear(error);

    if (!sound_band_render_valid_request(
            input,
            sample_count,
            sample_rate,
            low_hz,
            high_hz,
            output,
            error
        )) {
        return false;
    }

    uint64_t taps = fir_tap_count(sample_rate, low_hz, high_hz);
    float *kernel = NULL;
    if (!sound_band_render_allocate_float_buffer(&kernel, taps)) {
        sound_error_set(error, "could not allocate FIR band render kernel");
        return false;
    }
    defer {
        free(kernel);
    }

    uint64_t center = taps / 2U;
    double low = low_hz / sample_rate;
    double high = high_hz / sample_rate;

    for (uint64_t i = 0; i < taps; ++i) {
        double n = (double)i - (double)center;
        double ideal = 2.0 * high * sinc(2.0 * high * n) -
            2.0 * low * sinc(2.0 * low * n);
        double unit = (double)i / (double)(taps - 1U);
        double window = 0.5 - 0.5 * cos(2.0 * sound_band_render_pi * unit);

        kernel[i] = (float)(ideal * window);
    }

    for (uint64_t n = 0; n < sample_count; ++n) {
        output[n] = fir_sample(input, kernel, sample_count, taps, center, n);
    }

    return true;
}

typedef struct Biquad {
    double b0;
    double b1;
    double b2;
    double a1;
    double a2;
    double x1;
    double x2;
    double y1;
    double y2;
} Biquad;

static void biquad_init(
    Biquad *biquad,
    double sample_rate,
    double low_hz,
    double high_hz
) {
    double center = sqrt(low_hz * high_hz);
    double width = fmax(high_hz - low_hz, 1.0);
    double q = sound_band_render_clamp(center / width, 0.05, 80.0);
    double w0 = 2.0 * sound_band_render_pi * center / sample_rate;
    double alpha = sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;

    *biquad = (Biquad){
        .b0 = alpha / a0,
        .b1 = 0.0,
        .b2 = -alpha / a0,
        .a1 = -2.0 * cos(w0) / a0,
        .a2 = (1.0 - alpha) / a0,
    };
}

static float biquad_process(Biquad *biquad, float input) {
    double output = biquad->b0 * input +
        biquad->b1 * biquad->x1 +
        biquad->b2 * biquad->x2 -
        biquad->a1 * biquad->y1 -
        biquad->a2 * biquad->y2;

    biquad->x2 = biquad->x1;
    biquad->x1 = input;
    biquad->y2 = biquad->y1;
    biquad->y1 = output;
    return (float)output;
}

bool sound_band_render_iir_biquad(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    sound_error_clear(error);

    if (!sound_band_render_valid_request(
            input,
            sample_count,
            sample_rate,
            low_hz,
            high_hz,
            output,
            error
        )) {
        return false;
    }

    Biquad biquad;
    biquad_init(&biquad, sample_rate, low_hz, high_hz);

    for (uint64_t i = 0; i < sample_count; ++i) {
        output[i] = biquad_process(&biquad, input[i]);
    }

    return true;
}

bool sound_band_render_zero_phase_iir(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    sound_error_clear(error);

    if (!sound_band_render_valid_request(
            input,
            sample_count,
            sample_rate,
            low_hz,
            high_hz,
            output,
            error
        )) {
        return false;
    }

    float *work = NULL;
    if (!sound_band_render_allocate_float_buffer(&work, sample_count)) {
        sound_error_set(error, "could not allocate zero-phase band buffer");
        return false;
    }
    defer {
        free(work);
    }

    if (!sound_band_render_iir_biquad(
            input,
            sample_count,
            sample_rate,
            low_hz,
            high_hz,
            work,
            error
        )) {
        return false;
    }

    Biquad biquad;
    biquad_init(&biquad, sample_rate, low_hz, high_hz);
    for (uint64_t i = 0; i < sample_count; ++i) {
        uint64_t reverse = sample_count - 1U - i;
        output[reverse] = biquad_process(&biquad, work[reverse]);
    }

    return true;
}

bool sound_band_render_cqt_approx(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    double grow = pow(2.0, 1.0 / 24.0);
    return sound_band_render_fir_linear(
        input,
        sample_count,
        sample_rate,
        low_hz / grow,
        high_hz * grow,
        output,
        error
    );
}

static double erb_width(double hz) {
    return 24.7 * (4.37 * hz / 1000.0 + 1.0);
}

bool sound_band_render_auditory_approx(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    double center = sqrt(low_hz * high_hz);
    double width = erb_width(center);
    double low = fmax(1.0, low_hz - width);
    double high = fmin(sample_rate * 0.5, high_hz + width);

    return sound_band_render_iir_biquad(
        input,
        sample_count,
        sample_rate,
        low,
        high,
        output,
        error
    );
}
