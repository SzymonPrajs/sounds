#ifndef SOUNDS_ANALYSIS_ALGORITHM_H
#define SOUNDS_ANALYSIS_ALGORITHM_H

#include "sounds/app_mode.h"
#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SoundAnalysisAlgorithm SoundAnalysisAlgorithm;

typedef struct SoundAnalysisInput {
    const SoundRingBuffer *ring;
    uint64_t written_samples;
    uint64_t ring_capacity;
    uint64_t row_count;
    uint64_t column_limit;
} SoundAnalysisInput;

typedef struct SoundAnalysisOutput {
    float *columns;
    uint64_t column_count;
    uint64_t column_capacity;
    uint64_t row_count;
} SoundAnalysisOutput;

typedef struct SoundAnalysisAlgorithmOps {
    void (*destroy)(void *state);
    double (*min_frequency)(const void *state);
    double (*max_frequency)(const void *state);
    bool (*set_frequency_range)(
        void *state,
        double min_hz,
        double max_hz,
        SoundError *error
    );
    void (*reset)(void *state, uint64_t written_samples);
    void (*toggle_sst)(void *state);
    bool (*sst_enabled)(const void *state);
    bool (*update)(
        void *state,
        const SoundAnalysisInput *input,
        SoundAnalysisOutput *output,
        bool emit,
        SoundError *error
    );
} SoundAnalysisAlgorithmOps;

struct SoundAnalysisAlgorithm {
    const SoundAnalysisAlgorithmOps *ops;
    void *state;
    SoundAppMode mode;
};

bool sound_analysis_output_append(
    SoundAnalysisOutput *output,
    float **column,
    SoundError *error
);

void sound_analysis_algorithm_destroy(SoundAnalysisAlgorithm *algorithm);
double sound_analysis_algorithm_min_frequency(const SoundAnalysisAlgorithm *algorithm);
double sound_analysis_algorithm_max_frequency(const SoundAnalysisAlgorithm *algorithm);
bool sound_analysis_algorithm_set_frequency_range(
    SoundAnalysisAlgorithm *algorithm,
    double min_hz,
    double max_hz,
    SoundError *error
);
void sound_analysis_algorithm_reset(
    SoundAnalysisAlgorithm *algorithm,
    uint64_t written_samples
);
void sound_analysis_algorithm_toggle_sst(SoundAnalysisAlgorithm *algorithm);
bool sound_analysis_algorithm_sst_enabled(const SoundAnalysisAlgorithm *algorithm);
bool sound_analysis_algorithm_update(
    SoundAnalysisAlgorithm *algorithm,
    const SoundAnalysisInput *input,
    SoundAnalysisOutput *output,
    bool emit,
    SoundError *error
);

bool sound_transient_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
);

bool sound_tonal_algorithm_create(
    double sample_rate,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
);

bool sound_reassigned_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
);

bool sound_squeezed_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
);

bool sound_superlet_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
);

bool sound_multitaper_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
);

bool sound_s_transform_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
);

bool sound_sparse_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
);

#endif
