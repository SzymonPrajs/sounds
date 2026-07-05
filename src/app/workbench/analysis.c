#include "internal.h"

#include "sounds/offline_spectrum.h"

#include <Accelerate/Accelerate.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

void workbench_cycle_band_method(WorkbenchAudio *audio) {
    audio->band_method = sound_band_render_method_offset(audio->band_method, 1);
    audio->render_dirty = true;
}

void workbench_cycle_band_handle(WorkbenchAudio *audio) {
    audio->upper_handle_selected = !audio->upper_handle_selected;
}

static void move_band_edge(
    WorkbenchAudio *audio,
    bool upper,
    int semitone_steps,
    double min_hz,
    double max_hz
) {
    if (semitone_steps == 0) {
        return;
    }

    double ratio = pow(2.0, (double)semitone_steps / 12.0);

    if (upper) {
        audio->high_hz = fmin(max_hz, fmax(audio->low_hz * 1.03, audio->high_hz * ratio));
    } else {
        audio->low_hz = fmax(min_hz, fmin(audio->high_hz / 1.03, audio->low_hz * ratio));
    }

    audio->render_dirty = true;
}

void workbench_move_selected_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
) {
    move_band_edge(audio, audio->upper_handle_selected, semitone_steps, min_hz, max_hz);
}

void workbench_move_lower_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
) {
    move_band_edge(audio, false, semitone_steps, min_hz, max_hz);
}

void workbench_move_upper_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
) {
    move_band_edge(audio, true, semitone_steps, min_hz, max_hz);
}

bool workbench_ensure_spectrum(
    WorkbenchAudio *audio,
    uint64_t row_count,
    double sample_rate,
    double min_hz,
    double max_hz,
    SoundError *error
) {
    const float *samples = sound_clip_samples(&audio->clip);
    uint64_t sample_count = sound_clip_sample_count(&audio->clip);

    if (!samples || sample_count == 0 || row_count == 0) {
        return true;
    }

    if (row_count > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "whole-spectrum view is too large");
        return false;
    }

    if (audio->spectrum_row_count != row_count) {
        float *cells = realloc(audio->spectrum_cells, sizeof(float) * (size_t)row_count);
        if (!cells) {
            sound_error_set(error, "could not allocate whole-spectrum cells");
            return false;
        }

        audio->spectrum_cells = cells;
        audio->spectrum_row_count = row_count;
        audio->spectrum_dirty = true;
    }

    if (audio->spectrum_min_hz != min_hz || audio->spectrum_max_hz != max_hz) {
        audio->spectrum_min_hz = min_hz;
        audio->spectrum_max_hz = max_hz;
        audio->spectrum_dirty = true;
    }

    if (!audio->spectrum_dirty) {
        return true;
    }

    if (!sound_offline_spectrum_db(
            samples,
            sample_count,
            sample_rate,
            min_hz,
            max_hz,
            audio->spectrum_cells,
            row_count,
            error
        )) {
        return false;
    }

    audio->spectrum_dirty = false;
    return true;
}

static uint64_t capped_spectrogram_columns(uint64_t column_count) {
    return column_count < 512U ? column_count : 512U;
}

static uint64_t capped_spectrogram_rows(uint64_t row_count) {
    return row_count < 320U ? row_count : 320U;
}

static void active_spectrogram_range(
    const WorkbenchAudio *audio,
    uint64_t *start,
    uint64_t *end
) {
    *start = audio->trim_editing ? audio->draft_trim_start : audio->clip.trim_start;
    *end = audio->trim_editing ? audio->draft_trim_end : audio->clip.trim_end;

    if (*start > audio->clip.sample_count) {
        *start = audio->clip.sample_count;
    }

    if (*end > audio->clip.sample_count) {
        *end = audio->clip.sample_count;
    }

    if (*end < *start) {
        *end = *start;
    }
}

bool workbench_ensure_active_spectrogram(
    WorkbenchAudio *audio,
    uint64_t column_count,
    uint64_t row_count,
    double min_hz,
    double max_hz,
    SoundError *error
) {
    const float *source_samples = audio->clip.samples;
    uint64_t source_sample_count = audio->clip.sample_count;
    uint64_t active_start = 0;
    uint64_t active_end = 0;
    active_spectrogram_range(audio, &active_start, &active_end);

    column_count = capped_spectrogram_columns(column_count);
    row_count = capped_spectrogram_rows(row_count);

    if (!source_samples ||
        active_end <= active_start ||
        column_count == 0U ||
        row_count == 0U) {
        audio->spectrogram_columns = 0;
        audio->spectrogram_rows = 0;
        audio->spectrogram_dirty = true;
        return true;
    }

    if (column_count > UINT64_MAX / row_count ||
        column_count * row_count > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "active spectrogram view is too large");
        return false;
    }

    if (audio->spectrogram_columns != column_count ||
        audio->spectrogram_rows != row_count) {
        float *cells = realloc(
            audio->spectrogram_cells,
            sizeof(float) * (size_t)(column_count * row_count)
        );
        if (!cells) {
            sound_error_set(error, "could not allocate active spectrogram cells");
            return false;
        }

        audio->spectrogram_cells = cells;
        audio->spectrogram_columns = column_count;
        audio->spectrogram_rows = row_count;
        audio->spectrogram_dirty = true;
    }

    if (audio->spectrogram_source_samples != source_samples ||
        audio->spectrogram_source_sample_count != source_sample_count ||
        audio->spectrogram_active_start != active_start ||
        audio->spectrogram_active_end != active_end ||
        audio->spectrogram_sample_rate != audio->clip.sample_rate ||
        audio->spectrogram_min_hz != min_hz ||
        audio->spectrogram_max_hz != max_hz) {
        audio->spectrogram_source_samples = source_samples;
        audio->spectrogram_source_sample_count = source_sample_count;
        audio->spectrogram_active_start = active_start;
        audio->spectrogram_active_end = active_end;
        audio->spectrogram_sample_rate = audio->clip.sample_rate;
        audio->spectrogram_min_hz = min_hz;
        audio->spectrogram_max_hz = max_hz;
        audio->spectrogram_dirty = true;
    }

    if (!audio->spectrogram_dirty) {
        return true;
    }

    if (!sound_offline_spectrogram_db(
            source_samples + active_start,
            active_end - active_start,
            audio->clip.sample_rate,
            min_hz,
            max_hz,
            audio->spectrogram_cells,
            row_count,
            column_count,
            error
        )) {
        return false;
    }

    audio->spectrogram_dirty = false;
    return true;
}

static bool ensure_render_buffers(
    WorkbenchAudio *audio,
    uint64_t sample_count,
    SoundError *error
) {
    if (audio->render_count == sample_count &&
        audio->selected_samples &&
        audio->rejected_samples) {
        return true;
    }

    if (sample_count == 0 || sample_count > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "invalid band render sample count");
        return false;
    }

    float *selected = realloc(audio->selected_samples, sizeof(float) * (size_t)sample_count);
    if (!selected) {
        sound_error_set(error, "could not allocate selected-band samples");
        return false;
    }
    audio->selected_samples = selected;

    float *rejected = realloc(audio->rejected_samples, sizeof(float) * (size_t)sample_count);
    if (!rejected) {
        sound_error_set(error, "could not allocate rejected-band samples");
        return false;
    }
    audio->rejected_samples = rejected;
    audio->render_count = sample_count;
    audio->render_dirty = true;
    return true;
}

bool workbench_ensure_band_render(WorkbenchAudio *audio, SoundError *error) {
    if (!sound_clip_has_audio(&audio->clip)) {
        return true;
    }

    const float *samples = sound_clip_samples(&audio->clip);
    uint64_t sample_count = sound_clip_sample_count(&audio->clip);

    if (!ensure_render_buffers(audio, sample_count, error)) {
        return false;
    }

    if (!audio->render_dirty) {
        return true;
    }

    SoundBandRenderRequest request = {
        .sample_rate = audio->clip.sample_rate,
        .low_hz = audio->low_hz,
        .high_hz = audio->high_hz,
        .iterations = 2,
    };

    if (!sound_band_render(
            samples,
            sample_count,
            &request,
            audio->band_method,
            audio->selected_samples,
            error
        )) {
        return false;
    }

    sound_band_render_sanitize_output(
        audio->selected_samples,
        sample_count,
        audio->clip.sample_rate,
        samples
    );

    vDSP_vsub(
        audio->selected_samples,
        1,
        samples,
        1,
        audio->rejected_samples,
        1,
        (vDSP_Length)sample_count
    );

    sound_band_render_sanitize_output(
        audio->rejected_samples,
        sample_count,
        audio->clip.sample_rate,
        samples
    );

    audio->render_dirty = false;
    return true;
}
