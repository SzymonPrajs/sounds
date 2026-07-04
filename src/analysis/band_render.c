#include "sounds/band_render.h"

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
