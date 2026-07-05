#ifndef SOUNDS_SPECTRUM_H
#define SOUNDS_SPECTRUM_H

#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum SoundSpectrumMode {
    SOUND_SPECTRUM_TRANSIENT,
    SOUND_SPECTRUM_REASSIGNED,
    SOUND_SPECTRUM_SQUEEZED,
    SOUND_SPECTRUM_SUPERLET,
    SOUND_SPECTRUM_MULTITAPER,
    SOUND_SPECTRUM_S_TRANSFORM,
    SOUND_SPECTRUM_SPARSE,
} SoundSpectrumMode;

typedef struct SoundSpectrumAnalyzer SoundSpectrumAnalyzer;

bool sound_spectrum_analyzer_create(
    double sample_rate,
    double columns_per_second,
    SoundSpectrumAnalyzer **analyzer,
    SoundError *error
);

void sound_spectrum_analyzer_destroy(SoundSpectrumAnalyzer *analyzer);

double sound_spectrum_analyzer_min_frequency(const SoundSpectrumAnalyzer *analyzer);
double sound_spectrum_analyzer_max_frequency(const SoundSpectrumAnalyzer *analyzer);
uint64_t sound_spectrum_analyzer_latency_samples(const SoundSpectrumAnalyzer *analyzer);
bool sound_spectrum_analyzer_set_frequency_range(
    SoundSpectrumAnalyzer *analyzer,
    double min_hz,
    double max_hz,
    SoundError *error
);

bool sound_spectrum_analyzer_column_db(
    SoundSpectrumAnalyzer *analyzer,
    const SoundRingBuffer *ring,
    uint64_t center_sample,
    SoundSpectrumMode mode,
    float *dbfs_rows,
    uint64_t row_count,
    SoundError *error
);

#endif
