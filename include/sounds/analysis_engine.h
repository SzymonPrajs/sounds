#ifndef SOUNDS_ANALYSIS_ENGINE_H
#define SOUNDS_ANALYSIS_ENGINE_H

#include "sounds/app_mode.h"
#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SoundAnalysisEngine SoundAnalysisEngine;

typedef struct SoundAnalysisFrame {
    const float *columns;
    uint64_t column_count;
    uint64_t row_count;
} SoundAnalysisFrame;

bool sound_analysis_engine_create(
    double sample_rate,
    double columns_per_second,
    SoundAnalysisEngine **engine,
    SoundError *error
);

void sound_analysis_engine_destroy(SoundAnalysisEngine *engine);

double sound_analysis_engine_min_frequency(const SoundAnalysisEngine *engine);
double sound_analysis_engine_max_frequency(const SoundAnalysisEngine *engine);

void sound_analysis_engine_set_mode(
    SoundAnalysisEngine *engine,
    SoundAppMode mode
);

void sound_analysis_engine_toggle_sst(SoundAnalysisEngine *engine);
bool sound_analysis_engine_sst_enabled(const SoundAnalysisEngine *engine);

void sound_analysis_engine_reset_timeline(
    SoundAnalysisEngine *engine,
    uint64_t written_samples
);

bool sound_analysis_engine_update(
    SoundAnalysisEngine *engine,
    const SoundRingBuffer *ring,
    uint64_t written_samples,
    uint64_t ring_capacity,
    uint64_t row_count,
    SoundAnalysisFrame *frame,
    SoundError *error
);

#endif
