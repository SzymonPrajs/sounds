#include "algorithm.h"

#include "sounds/spectrum.h"

#include <stdlib.h>

typedef struct SpectrumModeAlgorithm {
    SoundSpectrumAnalyzer *spectrum;
    SoundSpectrumMode spectrum_mode;
    uint64_t column_samples;
    uint64_t last_center;
    uint64_t latency_samples;
} SpectrumModeAlgorithm;

static void spectrum_mode_destroy(void *state) {
    SpectrumModeAlgorithm *algorithm = state;

    if (!algorithm) {
        return;
    }

    sound_spectrum_analyzer_destroy(algorithm->spectrum);
    free(algorithm);
}

static double spectrum_mode_min_frequency(const void *state) {
    const SpectrumModeAlgorithm *algorithm = state;
    return sound_spectrum_analyzer_min_frequency(algorithm->spectrum);
}

static double spectrum_mode_max_frequency(const void *state) {
    const SpectrumModeAlgorithm *algorithm = state;
    return sound_spectrum_analyzer_max_frequency(algorithm->spectrum);
}

static void spectrum_mode_reset(void *state, uint64_t written_samples) {
    SpectrumModeAlgorithm *algorithm = state;

    algorithm->last_center = written_samples > algorithm->latency_samples ?
        written_samples - algorithm->latency_samples :
        0;
}

static bool spectrum_mode_update(
    void *state,
    const SoundAnalysisInput *input,
    SoundAnalysisOutput *output,
    bool emit,
    SoundError *error
) {
    SpectrumModeAlgorithm *algorithm = state;

    if (!emit) {
        return true;
    }

    uint64_t latest_center = input->written_samples > algorithm->latency_samples ?
        input->written_samples - algorithm->latency_samples :
        0;
    uint64_t first_center = algorithm->last_center + algorithm->column_samples;

    if (first_center < algorithm->latency_samples) {
        first_center = algorithm->latency_samples;
    }

    if (latest_center < first_center) {
        return true;
    }

    uint64_t pending = 1U + (latest_center - first_center) / algorithm->column_samples;
    if (pending > input->column_limit) {
        pending = input->column_limit;
    }

    for (uint64_t column = 0; column < pending; ++column) {
        uint64_t center_sample = first_center + column * algorithm->column_samples;
        float *dbfs_rows = NULL;

        if (!sound_analysis_output_append(output, &dbfs_rows, error)) {
            return false;
        }

        if (!sound_spectrum_analyzer_column_db(
                algorithm->spectrum,
                input->ring,
                center_sample,
                algorithm->spectrum_mode,
                dbfs_rows,
                input->row_count,
                error
            )) {
            return false;
        }

        algorithm->last_center = center_sample;
    }

    return true;
}

static const SoundAnalysisAlgorithmOps spectrum_mode_ops = {
    .destroy = spectrum_mode_destroy,
    .min_frequency = spectrum_mode_min_frequency,
    .max_frequency = spectrum_mode_max_frequency,
    .reset = spectrum_mode_reset,
    .toggle_sst = NULL,
    .sst_enabled = NULL,
    .update = spectrum_mode_update,
};

static bool spectrum_mode_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAppMode app_mode,
    SoundSpectrumMode spectrum_mode,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    *algorithm = (SoundAnalysisAlgorithm){0};

    SpectrumModeAlgorithm *state = calloc(1, sizeof(*state));
    if (!state) {
        sound_error_set(error, "could not allocate spectrum mode algorithm");
        return false;
    }

    state->column_samples = column_samples;
    state->spectrum_mode = spectrum_mode;

    if (!sound_spectrum_analyzer_create(
            sample_rate,
            columns_per_second,
            &state->spectrum,
            error
        )) {
        spectrum_mode_destroy(state);
        return false;
    }

    state->latency_samples = sound_spectrum_analyzer_latency_samples(state->spectrum);

    *algorithm = (SoundAnalysisAlgorithm){
        .ops = &spectrum_mode_ops,
        .state = state,
        .mode = app_mode,
    };
    return true;
}

bool sound_reassigned_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    return spectrum_mode_algorithm_create(
        sample_rate,
        columns_per_second,
        column_samples,
        SOUND_APP_MODE_REASSIGNED,
        SOUND_SPECTRUM_REASSIGNED,
        algorithm,
        error
    );
}

bool sound_squeezed_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    return spectrum_mode_algorithm_create(
        sample_rate,
        columns_per_second,
        column_samples,
        SOUND_APP_MODE_SQUEEZED,
        SOUND_SPECTRUM_SQUEEZED,
        algorithm,
        error
    );
}

bool sound_superlet_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    return spectrum_mode_algorithm_create(
        sample_rate,
        columns_per_second,
        column_samples,
        SOUND_APP_MODE_SUPERLET,
        SOUND_SPECTRUM_SUPERLET,
        algorithm,
        error
    );
}

bool sound_multitaper_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    return spectrum_mode_algorithm_create(
        sample_rate,
        columns_per_second,
        column_samples,
        SOUND_APP_MODE_MULTITAPER,
        SOUND_SPECTRUM_MULTITAPER,
        algorithm,
        error
    );
}

bool sound_s_transform_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    return spectrum_mode_algorithm_create(
        sample_rate,
        columns_per_second,
        column_samples,
        SOUND_APP_MODE_S_TRANSFORM,
        SOUND_SPECTRUM_S_TRANSFORM,
        algorithm,
        error
    );
}

bool sound_sparse_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    return spectrum_mode_algorithm_create(
        sample_rate,
        columns_per_second,
        column_samples,
        SOUND_APP_MODE_SPARSE,
        SOUND_SPECTRUM_SPARSE,
        algorithm,
        error
    );
}
