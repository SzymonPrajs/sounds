#include "internal.h"

#include "sounds/defer.h"

#include <Accelerate/Accelerate.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct RealDft {
    uint64_t length;
    uint64_t half_length;
    float *even;
    float *odd;
    float *real;
    float *imag;
    vDSP_DFT_Setup forward;
    vDSP_DFT_Setup inverse;
} RealDft;

typedef enum MaskShape {
    MASK_FULL_BAND,
    MASK_MODAL_PEAKS,
    MASK_SPARSE_THRESHOLD,
} MaskShape;

static bool next_power_of_two(uint64_t value, uint64_t *result) {
    uint64_t power = 2;

    while (power < value) {
        if (power > UINT64_MAX / 2U) {
            return false;
        }

        power *= 2U;
    }

    *result = power;
    return true;
}

static void real_dft_destroy(RealDft *dft) {
    if (!dft) {
        return;
    }

    vDSP_DFT_DestroySetup(dft->forward);
    vDSP_DFT_DestroySetup(dft->inverse);
    free(dft->even);
    free(dft->odd);
    free(dft->real);
    free(dft->imag);
    memset(dft, 0, sizeof(*dft));
}

static bool real_dft_create(RealDft *dft, uint64_t length, SoundError *error) {
    memset(dft, 0, sizeof(*dft));
    bool dft_ready = false;
    defer {
        if (!dft_ready) {
            real_dft_destroy(dft);
        }
    }

    if (length < 2U || (length & 1U) != 0U) {
        sound_error_set(error, "invalid band render DFT length");
        return false;
    }

    dft->length = length;
    dft->half_length = length / 2U;
    dft->forward = vDSP_DFT_zrop_CreateSetup(NULL, (vDSP_Length)length, vDSP_DFT_FORWARD);
    dft->inverse = vDSP_DFT_zrop_CreateSetup(NULL, (vDSP_Length)length, vDSP_DFT_INVERSE);

    if (!dft->forward || !dft->inverse ||
        !sound_band_render_allocate_float_buffer(&dft->even, dft->half_length) ||
        !sound_band_render_allocate_float_buffer(&dft->odd, dft->half_length) ||
        !sound_band_render_allocate_float_buffer(&dft->real, dft->half_length) ||
        !sound_band_render_allocate_float_buffer(&dft->imag, dft->half_length)) {
        sound_error_set(error, "could not allocate band render DFT buffers");
        return false;
    }

    dft_ready = true;
    return true;
}

static void real_dft_load(RealDft *dft, const float *input, uint64_t count) {
    memset(dft->even, 0, sizeof(float) * (size_t)dft->half_length);
    memset(dft->odd, 0, sizeof(float) * (size_t)dft->half_length);

    for (uint64_t i = 0; i < count && i < dft->length; ++i) {
        if ((i & 1U) == 0U) {
            dft->even[i / 2U] = input[i];
        } else {
            dft->odd[i / 2U] = input[i];
        }
    }
}

static void real_dft_store(const RealDft *dft, float *output, uint64_t count) {
    double scale = 1.0 / (double)dft->length;

    for (uint64_t i = 0; i < count && i < dft->length; ++i) {
        float value = (i & 1U) == 0U ? dft->even[i / 2U] : dft->odd[i / 2U];
        output[i] = (float)((double)value * scale);
    }
}

static double raised_band_mask(double hz, double low_hz, double high_hz, double softness_hz) {
    if (hz <= 0.0 || hz < low_hz - softness_hz || hz > high_hz + softness_hz) {
        return 0.0;
    }

    if (hz >= low_hz && hz <= high_hz) {
        return 1.0;
    }

    if (softness_hz <= 0.0) {
        return 0.0;
    }

    if (hz < low_hz) {
        double unit = (hz - (low_hz - softness_hz)) / softness_hz;
        return 0.5 - 0.5 * cos(
            sound_band_render_pi * sound_band_render_clamp(unit, 0.0, 1.0)
        );
    }

    double unit = ((high_hz + softness_hz) - hz) / softness_hz;
    return 0.5 - 0.5 * cos(
        sound_band_render_pi * sound_band_render_clamp(unit, 0.0, 1.0)
    );
}

static double bin_power(const RealDft *dft, uint64_t bin) {
    if (bin == 0U) {
        return (double)dft->real[0] * (double)dft->real[0];
    }

    if (bin >= dft->half_length) {
        return (double)dft->imag[0] * (double)dft->imag[0];
    }

    return (double)dft->real[bin] * (double)dft->real[bin] +
        (double)dft->imag[bin] * (double)dft->imag[bin];
}

static void apply_frequency_mask(
    RealDft *dft,
    double sample_rate,
    double low_hz,
    double high_hz,
    MaskShape shape
) {
    double softness = fmax(10.0, (high_hz - low_hz) * 0.04);
    double strongest = 0.0;

    if (shape == MASK_SPARSE_THRESHOLD) {
        for (uint64_t bin = 1; bin <= dft->half_length; ++bin) {
            double hz = (double)bin * sample_rate / (double)dft->length;
            double mask = raised_band_mask(hz, low_hz, high_hz, softness);
            if (mask > 0.0) {
                strongest = fmax(strongest, bin_power(dft, bin));
            }
            if (bin == dft->half_length) {
                break;
            }
        }
    }

    for (uint64_t bin = 0; bin <= dft->half_length; ++bin) {
        double hz = (double)bin * sample_rate / (double)dft->length;
        double mask = raised_band_mask(hz, low_hz, high_hz, softness);

        if (shape == MASK_MODAL_PEAKS && mask > 0.0 &&
            bin > 1U && bin + 1U < dft->half_length) {
            double power = bin_power(dft, bin);
            bool peak = power >= bin_power(dft, bin - 1U) &&
                power >= bin_power(dft, bin + 1U);
            mask = peak ? mask : 0.0;
        } else if (shape == MASK_SPARSE_THRESHOLD && mask > 0.0) {
            mask = bin_power(dft, bin) >= strongest * 0.04 ? mask : 0.0;
        }

        if (bin == 0U) {
            dft->real[0] = (float)((double)dft->real[0] * mask);
        } else if (bin >= dft->half_length) {
            dft->imag[0] = (float)((double)dft->imag[0] * mask);
        } else {
            dft->real[bin] = (float)((double)dft->real[bin] * mask);
            dft->imag[bin] = (float)((double)dft->imag[bin] * mask);
        }

        if (bin == dft->half_length) {
            break;
        }
    }
}

static bool fft_mask_shape(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    MaskShape shape,
    float *output,
    SoundError *error
) {
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

    uint64_t fft_length = 0;
    if (!next_power_of_two(sample_count, &fft_length)) {
        sound_error_set(error, "band render clip is too large");
        return false;
    }

    RealDft dft;
    if (!real_dft_create(&dft, fft_length, error)) {
        return false;
    }
    defer {
        real_dft_destroy(&dft);
    }

    real_dft_load(&dft, input, sample_count);
    vDSP_DFT_Execute(dft.forward, dft.even, dft.odd, dft.real, dft.imag);
    apply_frequency_mask(&dft, sample_rate, low_hz, high_hz, shape);
    vDSP_DFT_Execute(dft.inverse, dft.real, dft.imag, dft.even, dft.odd);
    real_dft_store(&dft, output, sample_count);
    return true;
}

bool sound_band_render_fft_mask(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    return fft_mask_shape(
        input,
        sample_count,
        sample_rate,
        low_hz,
        high_hz,
        MASK_FULL_BAND,
        output,
        error
    );
}

bool sound_band_render_stft_mask(
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

    uint64_t frame_length = 1024;
    uint64_t hop = frame_length / 4U;
    if (sample_count < frame_length) {
        return sound_band_render_fft_mask(
            input,
            sample_count,
            sample_rate,
            low_hz,
            high_hz,
            output,
            error
        );
    }

    RealDft dft;
    if (!real_dft_create(&dft, frame_length, error)) {
        return false;
    }
    defer {
        real_dft_destroy(&dft);
    }

    float *weight = NULL;
    if (!sound_band_render_allocate_float_buffer(&weight, sample_count)) {
        sound_error_set(error, "could not allocate STFT band render weights");
        return false;
    }
    defer {
        free(weight);
    }

    memset(output, 0, sizeof(float) * (size_t)sample_count);

    for (uint64_t start = 0; start < sample_count; start += hop) {
        memset(dft.even, 0, sizeof(float) * (size_t)dft.half_length);
        memset(dft.odd, 0, sizeof(float) * (size_t)dft.half_length);

        for (uint64_t i = 0; i < frame_length; ++i) {
            uint64_t source = start + i;
            double unit = (double)i / (double)(frame_length - 1U);
            float window = (float)(0.5 - 0.5 * cos(2.0 * sound_band_render_pi * unit));
            float value = source < sample_count ? input[source] * window : 0.0F;

            if ((i & 1U) == 0U) {
                dft.even[i / 2U] = value;
            } else {
                dft.odd[i / 2U] = value;
            }
        }

        vDSP_DFT_Execute(dft.forward, dft.even, dft.odd, dft.real, dft.imag);
        apply_frequency_mask(&dft, sample_rate, low_hz, high_hz, MASK_FULL_BAND);
        vDSP_DFT_Execute(dft.inverse, dft.real, dft.imag, dft.even, dft.odd);

        for (uint64_t i = 0; i < frame_length; ++i) {
            uint64_t target = start + i;
            if (target >= sample_count) {
                break;
            }

            double unit = (double)i / (double)(frame_length - 1U);
            float window = (float)(0.5 - 0.5 * cos(2.0 * sound_band_render_pi * unit));
            float value = ((i & 1U) == 0U ? dft.even[i / 2U] : dft.odd[i / 2U]) /
                (float)frame_length;

            output[target] += value * window;
            weight[target] += window * window;
        }

        if (start + frame_length >= sample_count) {
            break;
        }
    }

    for (uint64_t i = 0; i < sample_count; ++i) {
        if (weight[i] > 1.0e-8F) {
            output[i] /= weight[i];
        }
    }

    return true;
}

bool sound_band_render_griffin_lim(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    uint32_t iterations,
    float *output,
    SoundError *error
) {
    if (!sound_band_render_stft_mask(
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

    uint32_t passes = iterations > 0U ? iterations : 2U;
    float *work = NULL;
    if (!sound_band_render_allocate_float_buffer(&work, sample_count)) {
        sound_error_set(error, "could not allocate Griffin-Lim band buffer");
        return false;
    }
    defer {
        free(work);
    }

    for (uint32_t pass = 0; pass < passes; ++pass) {
        memcpy(work, output, sizeof(float) * (size_t)sample_count);
        if (!sound_band_render_stft_mask(
                work,
                sample_count,
                sample_rate,
                low_hz,
                high_hz,
                output,
                error
            )) {
            return false;
        }
    }

    return true;
}

bool sound_band_render_modal_approx(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    return fft_mask_shape(
        input,
        sample_count,
        sample_rate,
        low_hz,
        high_hz,
        MASK_MODAL_PEAKS,
        output,
        error
    );
}

bool sound_band_render_sparse_approx(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    return fft_mask_shape(
        input,
        sample_count,
        sample_rate,
        low_hz,
        high_hz,
        MASK_SPARSE_THRESHOLD,
        output,
        error
    );
}
