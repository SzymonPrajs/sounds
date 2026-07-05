#ifndef SOUNDS_OFFLINE_SPECTRUM_H
#define SOUNDS_OFFLINE_SPECTRUM_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

double sound_offline_spectrum_frequency_for_row(
    double min_hz,
    double max_hz,
    uint64_t row,
    uint64_t row_count
);

bool sound_offline_spectrum_db(
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    double min_hz,
    double max_hz,
    float *dbfs_rows,
    uint64_t row_count,
    SoundError *error
);

bool sound_offline_spectrogram_db(
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    double min_hz,
    double max_hz,
    float *dbfs_columns,
    uint64_t row_count,
    uint64_t column_count,
    SoundError *error
);

#endif
