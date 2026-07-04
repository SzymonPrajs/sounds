#ifndef SOUNDS_ANALYSIS_H
#define SOUNDS_ANALYSIS_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SoundSpectrumAnalyzer SoundSpectrumAnalyzer;

bool sound_is_power_of_two(uint64_t value);
uint64_t sound_previous_power_of_two(uint64_t value);

/* Computes RMS and peak absolute amplitude; zero samples yield zeroes. */
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
uint64_t sound_spectrum_analyzer_fft_size(const SoundSpectrumAnalyzer *analyzer);
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

bool sound_write_raw_f32(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    SoundError *error
);

bool sound_write_levels_csv(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    uint64_t block_size,
    SoundError *error
);

bool sound_write_spectrum_csv(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    uint64_t requested_fft_size,
    SoundError *error
);

#endif
