#ifndef SOUNDS_BAND_RENDER_INTERNAL_H
#define SOUNDS_BAND_RENDER_INTERNAL_H

#include "sounds/band_render.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static const double sound_band_render_pi = 3.14159265358979323846264338327950288;
static const double sound_band_render_maximum_sample_rate = 512000.0;

static inline bool sound_band_render_multiply_overflows_size(
    uint64_t count,
    size_t element_size
) {
    return count > (uint64_t)(SIZE_MAX / element_size);
}

static inline bool sound_band_render_allocate_float_buffer(
    float **buffer,
    uint64_t count
) {
    if (sound_band_render_multiply_overflows_size(count, sizeof(float))) {
        return false;
    }

    *buffer = calloc((size_t)count, sizeof(float));
    return *buffer != NULL;
}

static inline double sound_band_render_clamp(
    double value,
    double minimum,
    double maximum
) {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static inline bool sound_band_render_valid_request(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    if (!input ||
        !output ||
        sample_count == 0U ||
        sound_band_render_multiply_overflows_size(sample_count, sizeof(float)) ||
        !isfinite(sample_rate) ||
        sample_rate <= 0.0 ||
        sample_rate > sound_band_render_maximum_sample_rate) {
        sound_error_set(error, "invalid band render buffer");
        return false;
    }

    double nyquist = sample_rate * 0.5;
    if (low_hz < 0.0) {
        low_hz = 0.0;
    }

    if (high_hz <= low_hz || high_hz > nyquist) {
        sound_error_set(error, "invalid band render frequency range");
        return false;
    }

    return true;
}

#endif
