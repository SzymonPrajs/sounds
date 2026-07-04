#include "algorithm.h"

#include "sounds/analysis.h"

#include <stdlib.h>

typedef struct TonalAlgorithm {
    SoundWaveletAnalyzer *wavelet;
    float *samples;
    uint64_t column_samples;
    uint64_t analyzed_samples;
} TonalAlgorithm;

static void tonal_destroy(void *state) {
    TonalAlgorithm *algorithm = state;

    if (!algorithm) {
        return;
    }

    sound_wavelet_analyzer_destroy(algorithm->wavelet);
    free(algorithm->samples);
    free(algorithm);
}

static double tonal_min_frequency(const void *state) {
    const TonalAlgorithm *algorithm = state;
    return sound_wavelet_analyzer_min_frequency(algorithm->wavelet);
}

static double tonal_max_frequency(const void *state) {
    const TonalAlgorithm *algorithm = state;
    return sound_wavelet_analyzer_max_frequency(algorithm->wavelet);
}

static void tonal_toggle_sst(void *state) {
    TonalAlgorithm *algorithm = state;
    bool enabled = !sound_wavelet_analyzer_synchrosqueezed(algorithm->wavelet);

    sound_wavelet_analyzer_set_synchrosqueezed(algorithm->wavelet, enabled);
}

static bool tonal_sst_enabled(const void *state) {
    const TonalAlgorithm *algorithm = state;
    return sound_wavelet_analyzer_synchrosqueezed(algorithm->wavelet);
}

static bool tonal_push_column(
    TonalAlgorithm *algorithm,
    const SoundAnalysisInput *input,
    bool *pushed,
    SoundError *error
) {
    *pushed = false;

    uint64_t read_count = sound_ring_buffer_read_ending_at(
        input->ring,
        algorithm->analyzed_samples + algorithm->column_samples,
        algorithm->samples,
        algorithm->column_samples
    );

    if (read_count != algorithm->column_samples) {
        algorithm->analyzed_samples = input->written_samples;
        return true;
    }

    if (!sound_wavelet_analyzer_push(
            algorithm->wavelet,
            algorithm->samples,
            read_count,
            error
        )) {
        return false;
    }

    algorithm->analyzed_samples += read_count;
    *pushed = true;
    return true;
}

static bool tonal_update(
    void *state,
    const SoundAnalysisInput *input,
    SoundAnalysisOutput *output,
    bool emit,
    SoundError *error
) {
    TonalAlgorithm *algorithm = state;

    if (input->written_samples > algorithm->analyzed_samples + input->ring_capacity) {
        algorithm->analyzed_samples = input->written_samples - input->ring_capacity;
    }

    uint64_t pending =
        (input->written_samples - algorithm->analyzed_samples) / algorithm->column_samples;
    if (pending > input->column_limit) {
        pending = input->column_limit;
    }

    for (uint64_t column = 0; column < pending; ++column) {
        bool pushed = false;
        if (!tonal_push_column(algorithm, input, &pushed, error)) {
            return false;
        }

        if (!pushed) {
            break;
        }

        if (!emit) {
            continue;
        }

        float *dbfs_rows = NULL;
        if (!sound_analysis_output_append(output, &dbfs_rows, error)) {
            return false;
        }

        if (!sound_wavelet_analyzer_snapshot_db(
                algorithm->wavelet,
                dbfs_rows,
                input->row_count,
                error
            )) {
            return false;
        }
    }

    return true;
}

static const SoundAnalysisAlgorithmOps tonal_ops = {
    .destroy = tonal_destroy,
    .min_frequency = tonal_min_frequency,
    .max_frequency = tonal_max_frequency,
    .reset = NULL,
    .toggle_sst = tonal_toggle_sst,
    .sst_enabled = tonal_sst_enabled,
    .update = tonal_update,
};

bool sound_tonal_algorithm_create(
    double sample_rate,
    uint64_t column_samples,
    SoundAnalysisAlgorithm *algorithm,
    SoundError *error
) {
    *algorithm = (SoundAnalysisAlgorithm){0};

    TonalAlgorithm *state = calloc(1, sizeof(*state));
    if (!state) {
        sound_error_set(error, "could not allocate tonal algorithm");
        return false;
    }

    state->column_samples = column_samples;
    state->samples = malloc(sizeof(float) * (size_t)column_samples);

    if (!state->samples) {
        tonal_destroy(state);
        sound_error_set(error, "could not allocate tonal sample buffer");
        return false;
    }

    if (!sound_wavelet_analyzer_create(sample_rate, &state->wavelet, error)) {
        tonal_destroy(state);
        return false;
    }

    *algorithm = (SoundAnalysisAlgorithm){
        .ops = &tonal_ops,
        .state = state,
        .mode = SOUND_APP_MODE_TONAL,
    };
    return true;
}
