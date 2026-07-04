#include "sounds/band_render.h"

#include <math.h>

static const double pi_value = 3.14159265358979323846264338327950288;
static const double render_edge_fade_seconds = 0.010;
static const uint64_t render_edge_fade_max_samples = 2048;
static const double render_peak_headroom = 1.10;
static const double render_peak_floor = 0.02;
static const double render_peak_ceiling = 0.98;

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

static double render_peak_limit(double reference_peak) {
    if (!isfinite(reference_peak) || reference_peak < 0.0) {
        reference_peak = 0.0;
    }

    double limit = reference_peak * render_peak_headroom;
    if (limit < render_peak_floor) {
        limit = render_peak_floor;
    }
    if (limit > render_peak_ceiling) {
        limit = render_peak_ceiling;
    }

    return limit;
}

static uint64_t render_fade_sample_count(
    uint64_t sample_count,
    double sample_rate
) {
    if (sample_count < 2 || !isfinite(sample_rate) || sample_rate <= 0.0) {
        return 0;
    }

    uint64_t fade = (uint64_t)ceil(sample_rate * render_edge_fade_seconds);
    if (fade > render_edge_fade_max_samples) {
        fade = render_edge_fade_max_samples;
    }
    if (fade > sample_count / 2U) {
        fade = sample_count / 2U;
    }

    return fade;
}

static void apply_edge_fade(
    float *samples,
    uint64_t sample_count,
    uint64_t fade_count
) {
    if (fade_count == 0) {
        return;
    }

    for (uint64_t i = 0; i < fade_count; ++i) {
        double position = fade_count == 1U ?
            0.0 :
            (double)i / (double)(fade_count - 1U);
        float gain = (float)(0.5 - 0.5 * cos(pi_value * position));
        samples[i] *= gain;
        samples[sample_count - 1U - i] *= gain;
    }
}

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

void sound_band_render_sanitize_output(
    float *output,
    uint64_t sample_count,
    double sample_rate,
    const float *reference
) {
    if (!output || sample_count == 0) {
        return;
    }

    for (uint64_t i = 0; i < sample_count; ++i) {
        if (!isfinite(output[i])) {
            output[i] = 0.0F;
        }
    }

    uint64_t fade_count = render_fade_sample_count(sample_count, sample_rate);
    apply_edge_fade(output, sample_count, fade_count);

    double peak = absolute_peak(output, sample_count);
    double reference_peak = reference ?
        absolute_peak(reference, sample_count) :
        peak;
    double limit = render_peak_limit(reference_peak);

    if (peak > limit && peak > 0.0) {
        float scale = (float)(limit / peak);
        for (uint64_t i = 0; i < sample_count; ++i) {
            output[i] *= scale;
        }
    }
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
