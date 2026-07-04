#ifndef SOUNDS_BAND_RENDER_H
#define SOUNDS_BAND_RENDER_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum SoundBandRenderMethod {
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
    SOUND_BAND_RENDER_COUNT,
} SoundBandRenderMethod;

typedef struct SoundBandRenderRequest {
    double sample_rate;
    double low_hz;
    double high_hz;
    uint32_t iterations;
} SoundBandRenderRequest;

int sound_band_render_method_count(void);
SoundBandRenderMethod sound_band_render_method_at(int index);
int sound_band_render_method_index(SoundBandRenderMethod method);
SoundBandRenderMethod sound_band_render_method_offset(
    SoundBandRenderMethod method,
    int offset
);
const char *sound_band_render_method_name(SoundBandRenderMethod method);
const char *sound_band_render_method_short_name(SoundBandRenderMethod method);

bool sound_band_render(
    const float *input,
    uint64_t sample_count,
    const SoundBandRenderRequest *request,
    SoundBandRenderMethod method,
    float *output,
    SoundError *error
);

void sound_band_render_sanitize_output(
    float *output,
    uint64_t sample_count,
    double sample_rate,
    const float *reference
);

bool sound_band_render_fft_mask(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

bool sound_band_render_fir_linear(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

bool sound_band_render_iir_biquad(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

bool sound_band_render_zero_phase_iir(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

bool sound_band_render_stft_mask(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

bool sound_band_render_griffin_lim(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    uint32_t iterations,
    float *output,
    SoundError *error
);

bool sound_band_render_cqt_approx(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

bool sound_band_render_auditory_approx(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

bool sound_band_render_modal_approx(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

bool sound_band_render_sparse_approx(
    const float *input,
    uint64_t sample_count,
    double sample_rate,
    double low_hz,
    double high_hz,
    float *output,
    SoundError *error
);

#endif
