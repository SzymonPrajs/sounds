#include "sounds/analysis_engine.h"

#include "algorithm.h"
#include "sounds/defer.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

enum {
    minimum_column_samples = 32,
    analysis_algorithm_count = SOUND_APP_MODE_COUNT,
};

struct SoundAnalysisEngine {
    SoundAnalysisAlgorithm algorithms[analysis_algorithm_count];
    SoundAppMode mode;
    float *columns;
    uint64_t column_capacity_rows;
    uint64_t column_capacity_columns;
};

static uint64_t column_samples_for_rate(double sample_rate, double columns_per_second) {
    uint64_t samples = (uint64_t)llround(sample_rate / columns_per_second);
    return samples < minimum_column_samples ? minimum_column_samples : samples;
}

static SoundAnalysisAlgorithm *algorithm_for_mode(
    SoundAnalysisEngine *engine,
    SoundAppMode mode
) {
    for (uint64_t i = 0; i < analysis_algorithm_count; ++i) {
        if (engine->algorithms[i].mode == mode) {
            return &engine->algorithms[i];
        }
    }

    return NULL;
}

static const SoundAnalysisAlgorithm *const_algorithm_for_mode(
    const SoundAnalysisEngine *engine,
    SoundAppMode mode
) {
    for (uint64_t i = 0; i < analysis_algorithm_count; ++i) {
        if (engine->algorithms[i].mode == mode) {
            return &engine->algorithms[i];
        }
    }

    return NULL;
}

static bool ensure_column_storage(
    SoundAnalysisEngine *engine,
    uint64_t row_count,
    uint64_t column_count,
    SoundError *error
) {
    if (engine->column_capacity_rows == row_count &&
        engine->column_capacity_columns == column_count) {
        return true;
    }

    if (row_count > (uint64_t)(SIZE_MAX / sizeof(float)) / column_count) {
        sound_error_set(error, "analysis column buffer is too large");
        return false;
    }

    float *columns = realloc(
        engine->columns,
        sizeof(float) * (size_t)row_count * (size_t)column_count
    );

    if (!columns) {
        sound_error_set(error, "could not allocate analysis column buffer");
        return false;
    }

    engine->columns = columns;
    engine->column_capacity_rows = row_count;
    engine->column_capacity_columns = column_count;
    return true;
}

static bool create_algorithms(
    SoundAnalysisEngine *engine,
    double sample_rate,
    double columns_per_second,
    SoundError *error
) {
    uint64_t column_samples =
        column_samples_for_rate(sample_rate, columns_per_second);

    return sound_transient_algorithm_create(
            sample_rate,
            columns_per_second,
            column_samples,
            &engine->algorithms[0],
            error
        ) &&
        sound_tonal_algorithm_create(
            sample_rate,
            column_samples,
            &engine->algorithms[1],
            error
        ) &&
        sound_reassigned_algorithm_create(
            sample_rate,
            columns_per_second,
            column_samples,
            &engine->algorithms[2],
            error
        ) &&
        sound_squeezed_algorithm_create(
            sample_rate,
            columns_per_second,
            column_samples,
            &engine->algorithms[3],
            error
        ) &&
        sound_superlet_algorithm_create(
            sample_rate,
            columns_per_second,
            column_samples,
            &engine->algorithms[4],
            error
        ) &&
        sound_multitaper_algorithm_create(
            sample_rate,
            columns_per_second,
            column_samples,
            &engine->algorithms[5],
            error
        ) &&
        sound_s_transform_algorithm_create(
            sample_rate,
            columns_per_second,
            column_samples,
            &engine->algorithms[6],
            error
        ) &&
        sound_sparse_algorithm_create(
            sample_rate,
            columns_per_second,
            column_samples,
            &engine->algorithms[7],
            error
        );
}

bool sound_analysis_engine_create(
    double sample_rate,
    double columns_per_second,
    SoundAnalysisEngine **engine_out,
    SoundError *error
) {
    *engine_out = NULL;

    SoundAnalysisEngine *engine = calloc(1, sizeof(*engine));
    if (!engine) {
        sound_error_set(error, "could not allocate analysis engine");
        return false;
    }
    defer {
        sound_analysis_engine_destroy(engine);
    }

    engine->mode = SOUND_APP_MODE_TRANSIENT;

    if (!create_algorithms(engine, sample_rate, columns_per_second, error)) {
        return false;
    }

    *engine_out = engine;
    engine = NULL;
    return true;
}

void sound_analysis_engine_destroy(SoundAnalysisEngine *engine) {
    if (!engine) {
        return;
    }

    for (uint64_t i = 0; i < analysis_algorithm_count; ++i) {
        sound_analysis_algorithm_destroy(&engine->algorithms[i]);
    }

    free(engine->columns);
    free(engine);
}

double sound_analysis_engine_min_frequency(const SoundAnalysisEngine *engine) {
    return sound_analysis_algorithm_min_frequency(&engine->algorithms[0]);
}

double sound_analysis_engine_max_frequency(const SoundAnalysisEngine *engine) {
    return sound_analysis_algorithm_max_frequency(&engine->algorithms[0]);
}

void sound_analysis_engine_set_mode(
    SoundAnalysisEngine *engine,
    SoundAppMode mode
) {
    engine->mode = mode;
}

void sound_analysis_engine_toggle_sst(SoundAnalysisEngine *engine) {
    SoundAnalysisAlgorithm *tonal =
        algorithm_for_mode(engine, SOUND_APP_MODE_TONAL);

    sound_analysis_algorithm_toggle_sst(tonal);
}

bool sound_analysis_engine_sst_enabled(const SoundAnalysisEngine *engine) {
    const SoundAnalysisAlgorithm *tonal =
        const_algorithm_for_mode(engine, SOUND_APP_MODE_TONAL);

    return sound_analysis_algorithm_sst_enabled(tonal);
}

void sound_analysis_engine_reset_timeline(
    SoundAnalysisEngine *engine,
    uint64_t written_samples
) {
    for (uint64_t i = 0; i < analysis_algorithm_count; ++i) {
        sound_analysis_algorithm_reset(&engine->algorithms[i], written_samples);
    }
}

void sound_analysis_engine_reset_mode_timeline(
    SoundAnalysisEngine *engine,
    SoundAppMode mode,
    uint64_t written_samples
) {
    SoundAnalysisAlgorithm *algorithm = algorithm_for_mode(engine, mode);
    sound_analysis_algorithm_reset(algorithm, written_samples);
}

bool sound_analysis_engine_update(
    SoundAnalysisEngine *engine,
    const SoundRingBuffer *ring,
    uint64_t written_samples,
    uint64_t ring_capacity,
    uint64_t row_count,
    uint64_t column_limit,
    SoundAnalysisFrame *frame,
    SoundError *error
) {
    *frame = (SoundAnalysisFrame){
        .columns = NULL,
        .column_count = 0,
        .row_count = row_count,
    };

    if (row_count == 0) {
        return true;
    }

    if (column_limit == 0) {
        column_limit = 1;
    }

    if (!ensure_column_storage(engine, row_count, column_limit, error)) {
        return false;
    }

    SoundAnalysisInput input = {
        .ring = ring,
        .written_samples = written_samples,
        .ring_capacity = ring_capacity,
        .row_count = row_count,
        .column_limit = column_limit,
    };
    SoundAnalysisOutput output = {
        .columns = engine->columns,
        .column_count = 0,
        .column_capacity = column_limit,
        .row_count = row_count,
    };

    SoundAnalysisAlgorithm *algorithm = algorithm_for_mode(engine, engine->mode);
    if (!algorithm) {
        sound_error_set(error, "missing analysis mode");
        return false;
    }

    if (!sound_analysis_algorithm_update(
            algorithm,
            &input,
            &output,
            true,
            error
        )) {
        return false;
    }

    frame->columns = engine->columns;
    frame->column_count = output.column_count;
    return true;
}
