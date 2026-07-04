#ifndef SOUNDS_ANALYSIS_H
#define SOUNDS_ANALYSIS_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SoundSpectrumAnalyzer SoundSpectrumAnalyzer;

void sound_compute_levels(
    const float *samples,
    uint64_t sample_count,
    double *rms,
    double *peak
);

bool sound_spectrum_analyzer_create(
    uint64_t fft_size,
    SoundSpectrumAnalyzer **analyzer,
    SoundError *error
);

void sound_spectrum_analyzer_destroy(SoundSpectrumAnalyzer *analyzer);
uint64_t sound_spectrum_analyzer_bin_count(const SoundSpectrumAnalyzer *analyzer);

bool sound_spectrum_analyzer_compute(
    SoundSpectrumAnalyzer *analyzer,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    float *dbfs,
    uint64_t dbfs_count,
    SoundError *error
);

#endif
