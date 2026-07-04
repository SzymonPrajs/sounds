#include "algorithm.h"

#include "sounds/spectrum.h"

#include <stdlib.h>

typedef struct TransientAlgorithm {
    SoundSpectrumAnalyzer *spectrum;
    uint64_t column_samples;
    uint64_t last_center;
    uint64_t latency_samples;
} TransientAlgorithm;

static void transient_destroy(void *state) {
    TransientAlgorithm *algorithm = state;

    if (!algorithm) {
        return;
    }

    sound_spectrum_analyzer_destroy(algorithm->spectrum);
    free(algorithm);
}

static double transient_min_frequency(const void *state) {
    const TransientAlgorithm *algorithm = state;
    return sound_spectrum_analyzer_min_frequency(algorithm->spectrum);
}

static double transient_max_frequency(const void *state) {
    const TransientAlgorithm *algorithm = state;
    return sound_spectrum_analyzer_max_frequency(algorithm->spectrum);
}

static void transient_reset(void *state, uint64_t written_samples) {
    TransientAlgorithm *algorithm = state;

    algorithm->last_center = written_samples > algorithm->latency_samples ?
        written_samples - algorithm->latency_samples :
        0;
}

static bool transient_update(
    void *state,
    const SoundAnalysisInput *input,
    SoundAnalysisOutput *output,
    bool emit,
    SoundError *error
) {
    TransientAlgorithm *algorithm = state;

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
                SOUND_SPECTRUM_TRANSIENT,
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

static const SoundAnalysisAlgorithmOps transient_ops = {
    .destroy = transient_destroy,
    .min_frequency = transient_min_frequency,
    .max_frequency = transient_max_frequency,
    .reset = transient_reset,
    .toggle_sst = NULL,
    .sst_enabled = NULL,
    .update = transient_update,
};

bool sound_transient_algorithm_create(
    double sample_rate,
    double columns_per_second,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    *algorithm = (SoundAnalysisAlgorithm){0};

    TransientAlgorithm *state = calloc(1, sizeof(*state));
    if (!state) {
        sound_error_set(error, "could not allocate transient algorithm");
        return false;
    }

    state->column_samples = column_samples;

    if (!sound_spectrum_analyzer_create(
            sample_rate,
            columns_per_second,
            &state->spectrum,
            error
        )) {
        transient_destroy(state);
        return false;
    }

    state->latency_samples = sound_spectrum_analyzer_latency_samples(state->spectrum);

    *algorithm = (SoundAnalysisAlgorithm){
        .ops = &transient_ops,
        .state = state,
        .mode = SOUND_APP_MODE_TRANSIENT,
    };
    return true;
}
