#include "sounds/band_render.h"

#include <Accelerate/Accelerate.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const double pi_value = 3.14159265358979323846264338327950288;

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

static const SoundBandRenderMethod methods[] = {
    SOUND_BAND_RENDER_FFT_MASK,
    SOUND_BAND_RENDER_FIR_LINEAR,
    SOUND_BAND_RENDER_IIR_BIQUAD,
    SOUND_BAND_RENDER_ZERO_PHASE_IIR,
    SOUND_BAND_RENDER_STFT_MASK,
    SOUND_BAND_RENDER_GRIFFIN_LIM,
    SOUND_BAND_RENDER_CQT_APPROX,
    SOUND_BAND_RENDER_AUDITORY_APPROX,
    SOUND_BAND_RENDER_MODAL_APPROX,
    SOUND_BAND_RENDER_SPARSE_APPROX,
};

int sound_band_render_method_count(void) {
    return (int)(sizeof(methods) / sizeof(methods[0]));
}

SoundBandRenderMethod sound_band_render_method_at(int index) {
    if (index < 0 || index >= sound_band_render_method_count()) {
        return SOUND_BAND_RENDER_FFT_MASK;
    }

    return methods[index];
}

int sound_band_render_method_index(SoundBandRenderMethod method) {
    for (int i = 0; i < sound_band_render_method_count(); ++i) {
        if (methods[i] == method) {
            return i;
        }
    }

    return 0;
}

SoundBandRenderMethod sound_band_render_method_offset(
    SoundBandRenderMethod method,
    int offset
) {
    int count = sound_band_render_method_count();
    int index = sound_band_render_method_index(method) + offset;

    while (index < 0) {
        index += count;
    }

    return sound_band_render_method_at(index % count);
}

const char *sound_band_render_method_name(SoundBandRenderMethod method) {
    switch (method) {
        case SOUND_BAND_RENDER_FFT_MASK:
            return "FFT MASK IFFT";
        case SOUND_BAND_RENDER_FIR_LINEAR:
            return "FIR LINEAR";
        case SOUND_BAND_RENDER_IIR_BIQUAD:
            return "IIR BIQUAD";
        case SOUND_BAND_RENDER_ZERO_PHASE_IIR:
            return "IIR ZERO PHASE";
        case SOUND_BAND_RENDER_STFT_MASK:
            return "STFT MASK ISTFT";
        case SOUND_BAND_RENDER_GRIFFIN_LIM:
            return "GRIFFIN LIM";
        case SOUND_BAND_RENDER_CQT_APPROX:
            return "CQT APPROX";
        case SOUND_BAND_RENDER_AUDITORY_APPROX:
            return "ERB AUDITORY";
        case SOUND_BAND_RENDER_MODAL_APPROX:
            return "MODAL";
        case SOUND_BAND_RENDER_SPARSE_APPROX:
            return "SPARSE";
        case SOUND_BAND_RENDER_COUNT:
            break;
    }

    return "FFT MASK IFFT";
}

const char *sound_band_render_method_short_name(SoundBandRenderMethod method) {
    return sound_band_render_method_name(method);
}

static bool multiply_overflows_size(uint64_t count, size_t element_size) {
    return count > (uint64_t)(SIZE_MAX / element_size);
}

static bool allocate_float_buffer(float **buffer, uint64_t count) {
    if (multiply_overflows_size(count, sizeof(float))) {
        return false;
    }

    *buffer = calloc((size_t)count, sizeof(float));
    return *buffer != NULL;
}

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

static double clamp_double(double value, double minimum, double maximum) {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static bool valid_request(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
) {
    if (!input || !output || sample_count == 0U || sample_rate <= 0.0) {
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

    if (length < 2U || (length & 1U) != 0U) {
        sound_error_set(error, "invalid band render DFT length");
        return false;
    }

    dft->length = length;
    dft->half_length = length / 2U;
    dft->forward = vDSP_DFT_zrop_CreateSetup(NULL, (vDSP_Length)length, vDSP_DFT_FORWARD);
    dft->inverse = vDSP_DFT_zrop_CreateSetup(NULL, (vDSP_Length)length, vDSP_DFT_INVERSE);

    if (!dft->forward || !dft->inverse ||
        !allocate_float_buffer(&dft->even, dft->half_length) ||
        !allocate_float_buffer(&dft->odd, dft->half_length) ||
        !allocate_float_buffer(&dft->real, dft->half_length) ||
        !allocate_float_buffer(&dft->imag, dft->half_length)) {
        real_dft_destroy(dft);
        sound_error_set(error, "could not allocate band render DFT buffers");
        return false;
    }

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
        return 0.5 - 0.5 * cos(pi_value * clamp_double(unit, 0.0, 1.0));
    }

    double unit = ((high_hz + softness_hz) - hz) / softness_hz;
    return 0.5 - 0.5 * cos(pi_value * clamp_double(unit, 0.0, 1.0));
}

typedef enum MaskShape {
    MASK_FULL_BAND,
    MASK_MODAL_PEAKS,
    MASK_SPARSE_THRESHOLD,
} MaskShape;

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

        if (shape == MASK_MODAL_PEAKS && mask > 0.0 && bin > 1U && bin + 1U < dft->half_length) {
            double power = bin_power(dft, bin);
            bool peak = power >= bin_power(dft, bin - 1U) && power >= bin_power(dft, bin + 1U);
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
    if (!valid_request(input, sample_count, sample_rate, low_hz, high_hz, output, error)) {
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

    real_dft_load(&dft, input, sample_count);
    vDSP_DFT_Execute(dft.forward, dft.even, dft.odd, dft.real, dft.imag);
    apply_frequency_mask(&dft, sample_rate, low_hz, high_hz, shape);
    vDSP_DFT_Execute(dft.inverse, dft.real, dft.imag, dft.even, dft.odd);
    real_dft_store(&dft, output, sample_count);
    real_dft_destroy(&dft);
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

static double sinc(double value) {
    if (fabs(value) < 1.0e-12) {
        return 1.0;
    }

    return sin(pi_value * value) / (pi_value * value);
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

    if (!valid_request(input, sample_count, sample_rate, low_hz, high_hz, output, error)) {
        return false;
    }

    uint64_t taps = fir_tap_count(sample_rate, low_hz, high_hz);
    float *kernel = NULL;
    if (!allocate_float_buffer(&kernel, taps)) {
        sound_error_set(error, "could not allocate FIR band render kernel");
        return false;
    }

    uint64_t center = taps / 2U;
    double low = low_hz / sample_rate;
    double high = high_hz / sample_rate;
    double sum = 0.0;

    for (uint64_t i = 0; i < taps; ++i) {
        double n = (double)i - (double)center;
        double ideal = 2.0 * high * sinc(2.0 * high * n) -
            2.0 * low * sinc(2.0 * low * n);
        double unit = (double)i / (double)(taps - 1U);
        double window = 0.5 - 0.5 * cos(2.0 * pi_value * unit);

        kernel[i] = (float)(ideal * window);
        sum += kernel[i];
    }

    (void)sum;
    memset(output, 0, sizeof(float) * (size_t)sample_count);
    for (uint64_t n = 0; n < sample_count; ++n) {
        double value = 0.0;

        for (uint64_t k = 0; k < taps; ++k) {
            uint64_t source;
            if (n + center < k) {
                continue;
            }

            source = n + center - k;
            if (source < sample_count) {
                value += (double)input[source] * (double)kernel[k];
            }
        }

        output[n] = (float)value;
    }

    free(kernel);
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
    double q = clamp_double(center / width, 0.05, 80.0);
    double w0 = 2.0 * pi_value * center / sample_rate;
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

    if (!valid_request(input, sample_count, sample_rate, low_hz, high_hz, output, error)) {
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

    if (!valid_request(input, sample_count, sample_rate, low_hz, high_hz, output, error)) {
        return false;
    }

    float *work = NULL;
    if (!allocate_float_buffer(&work, sample_count)) {
        sound_error_set(error, "could not allocate zero-phase band buffer");
        return false;
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
        free(work);
        return false;
    }

    Biquad biquad;
    biquad_init(&biquad, sample_rate, low_hz, high_hz);
    for (uint64_t i = 0; i < sample_count; ++i) {
        uint64_t reverse = sample_count - 1U - i;
        output[reverse] = biquad_process(&biquad, work[reverse]);
    }

    free(work);
    return true;
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

    if (!valid_request(input, sample_count, sample_rate, low_hz, high_hz, output, error)) {
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

    float *weight = NULL;
    if (!allocate_float_buffer(&weight, sample_count)) {
        real_dft_destroy(&dft);
        sound_error_set(error, "could not allocate STFT band render weights");
        return false;
    }

    memset(output, 0, sizeof(float) * (size_t)sample_count);

    for (uint64_t start = 0; start < sample_count; start += hop) {
        memset(dft.even, 0, sizeof(float) * (size_t)dft.half_length);
        memset(dft.odd, 0, sizeof(float) * (size_t)dft.half_length);

        for (uint64_t i = 0; i < frame_length; ++i) {
            uint64_t source = start + i;
            double unit = (double)i / (double)(frame_length - 1U);
            float window = (float)(0.5 - 0.5 * cos(2.0 * pi_value * unit));
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
            float window = (float)(0.5 - 0.5 * cos(2.0 * pi_value * unit));
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

    free(weight);
    real_dft_destroy(&dft);
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
    if (!allocate_float_buffer(&work, sample_count)) {
        sound_error_set(error, "could not allocate Griffin-Lim band buffer");
        return false;
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
            free(work);
            return false;
        }
    }

    free(work);
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

bool sound_band_render(
    const float *input,
    uint64_t sample_count,
    const SoundBandRenderRequest *request,
    SoundBandRenderMethod method,
    float *output,
    SoundError *error
) {
    if (!request) {
        sound_error_set(error, "missing band render request");
        return false;
    }

    switch (method) {
        case SOUND_BAND_RENDER_FFT_MASK:
            return sound_band_render_fft_mask(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_FIR_LINEAR:
            return sound_band_render_fir_linear(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_IIR_BIQUAD:
            return sound_band_render_iir_biquad(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_ZERO_PHASE_IIR:
            return sound_band_render_zero_phase_iir(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_STFT_MASK:
            return sound_band_render_stft_mask(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_GRIFFIN_LIM:
            return sound_band_render_griffin_lim(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                request->iterations,
                output,
                error
            );
        case SOUND_BAND_RENDER_CQT_APPROX:
            return sound_band_render_cqt_approx(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_AUDITORY_APPROX:
            return sound_band_render_auditory_approx(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_MODAL_APPROX:
            return sound_band_render_modal_approx(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_SPARSE_APPROX:
            return sound_band_render_sparse_approx(
                input,
                sample_count,
                request->sample_rate,
                request->low_hz,
                request->high_hz,
                output,
                error
            );
        case SOUND_BAND_RENDER_COUNT:
            break;
    }

    sound_error_set(error, "unknown band render method");
    return false;
}
