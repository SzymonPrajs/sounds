#include "algorithm.h"

#include <stddef.h>

bool sound_analysis_output_append(
    SoundAnalysisOutput *output,
    float **column,
    SoundError *error
) {
    if (output->column_count >= output->column_capacity) {
        sound_error_set(error, "analysis output column buffer is full");
        return false;
    }

    *column = output->columns + output->column_count * output->row_count;
    ++output->column_count;
    return true;
}

void sound_analysis_algorithm_destroy(SoundAnalysisAlgorithm *algorithm) {
    if (!algorithm || !algorithm->ops) {
        return;
    }

    algorithm->ops->destroy(algorithm->state);
    algorithm->ops = NULL;
    algorithm->state = NULL;
}

double sound_analysis_algorithm_min_frequency(const SoundAnalysisAlgorithm *algorithm) {
    if (!algorithm || !algorithm->ops || !algorithm->ops->min_frequency) {
        return 0.0;
    }

    return algorithm->ops->min_frequency(algorithm->state);
}

double sound_analysis_algorithm_max_frequency(const SoundAnalysisAlgorithm *algorithm) {
    if (!algorithm || !algorithm->ops || !algorithm->ops->max_frequency) {
        return 0.0;
    }

    return algorithm->ops->max_frequency(algorithm->state);
}

bool sound_analysis_algorithm_set_frequency_range(
    SoundAnalysisAlgorithm *algorithm,
    double min_hz,
    double max_hz,
    SoundError *error
) {
    if (!algorithm || !algorithm->ops || !algorithm->ops->set_frequency_range) {
        sound_error_set(error, "missing analysis frequency range setter");
        return false;
    }

    return algorithm->ops->set_frequency_range(
        algorithm->state,
        min_hz,
        max_hz,
        error
    );
}

void sound_analysis_algorithm_reset(
    SoundAnalysisAlgorithm *algorithm,
    uint64_t written_samples
) {
    if (algorithm && algorithm->ops && algorithm->ops->reset) {
        algorithm->ops->reset(algorithm->state, written_samples);
    }
}

void sound_analysis_algorithm_toggle_sst(SoundAnalysisAlgorithm *algorithm) {
    if (algorithm && algorithm->ops && algorithm->ops->toggle_sst) {
        algorithm->ops->toggle_sst(algorithm->state);
    }
}

bool sound_analysis_algorithm_sst_enabled(const SoundAnalysisAlgorithm *algorithm) {
    if (!algorithm || !algorithm->ops || !algorithm->ops->sst_enabled) {
        return false;
    }

    return algorithm->ops->sst_enabled(algorithm->state);
}

bool sound_analysis_algorithm_update(
    SoundAnalysisAlgorithm *algorithm,
    const SoundAnalysisInput *input,
    SoundAnalysisOutput *output,
    bool emit,
    SoundError *error
) {
    if (!algorithm || !algorithm->ops || !algorithm->ops->update) {
        sound_error_set(error, "missing analysis algorithm");
        return false;
    }

    return algorithm->ops->update(algorithm->state, input, output, emit, error);
}
