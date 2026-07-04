#include "workbench.h"

#include "sounds/offline_spectrum.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

static const double default_freeze_seconds = 5.0;
static const double trim_step_seconds = 0.25;

static uint64_t sample_count_for_seconds(double sample_rate, double seconds) {
    return (uint64_t)(sample_rate * seconds);
}

static const char *audition_name(SoundAuditionTarget target) {
    switch (target) {
        case SOUND_AUDITION_ORIGINAL:
            return "ORIGINAL";
        case SOUND_AUDITION_SELECTED:
            return "SELECTED BAND";
        case SOUND_AUDITION_REJECTED:
            return "REJECTED BAND";
        case SOUND_AUDITION_COUNT:
            break;
    }

    return "ORIGINAL";
}

void workbench_audio_init(WorkbenchAudio *audio) {
    sound_clip_init(&audio->clip);
    audio->spectrum_rows = NULL;
    audio->spectrum_row_count = 0;
    audio->selected_samples = NULL;
    audio->rejected_samples = NULL;
    audio->render_count = 0;
    audio->low_hz = 120.0;
    audio->high_hz = 1000.0;
    audio->band_method = SOUND_BAND_RENDER_FFT_MASK;
    audio->audition = SOUND_AUDITION_ORIGINAL;
    audio->upper_handle_selected = false;
    audio->spectrum_dirty = true;
    audio->render_dirty = true;
}

void workbench_audio_free(WorkbenchAudio *audio) {
    sound_clip_free(&audio->clip);
    free(audio->spectrum_rows);
    free(audio->selected_samples);
    free(audio->rejected_samples);
}

void workbench_mark_clip_changed(WorkbenchAudio *audio) {
    audio->spectrum_dirty = true;
    audio->render_dirty = true;
    audio->render_count = 0;
}

void workbench_cycle_audition(WorkbenchAudio *audio) {
    audio->audition =
        (SoundAuditionTarget)(((int)audio->audition + 1) % (int)SOUND_AUDITION_COUNT);
}

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

bool workbench_ensure_spectrum_rows(
    WorkbenchAudio *audio,
    uint64_t row_count,
    double sample_rate,
    double min_hz,
    double max_hz,
    SoundError *error
) {
    if (!sound_clip_has_audio(&audio->clip) || row_count == 0) {
        return true;
    }

    if (audio->spectrum_row_count != row_count) {
        float *rows = realloc(audio->spectrum_rows, sizeof(float) * (size_t)row_count);
        if (!rows) {
            sound_error_set(error, "could not allocate whole-spectrum rows");
            return false;
        }

        audio->spectrum_rows = rows;
        audio->spectrum_row_count = row_count;
        audio->spectrum_dirty = true;
    }

    if (!audio->spectrum_dirty) {
        return true;
    }

    if (!sound_offline_spectrum_db(
            sound_clip_samples(&audio->clip),
            sound_clip_sample_count(&audio->clip),
            sample_rate,
            min_hz,
            max_hz,
            audio->spectrum_rows,
            row_count,
            error
        )) {
        return false;
    }

    audio->spectrum_dirty = false;
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

    for (uint64_t i = 0; i < sample_count; ++i) {
        audio->rejected_samples[i] = samples[i] - audio->selected_samples[i];
    }

    audio->render_dirty = false;
    return true;
}

bool workbench_freeze_recent_clip(
    WorkbenchAudio *audio,
    const SoundRingBuffer *ring,
    uint64_t written_samples,
    double sample_rate,
    const char *label,
    SoundError *error
) {
    uint64_t requested = sample_count_for_seconds(sample_rate, default_freeze_seconds);
    uint64_t available = written_samples < sound_ring_buffer_capacity(ring) ?
        written_samples :
        sound_ring_buffer_capacity(ring);

    if (available == 0) {
        return true;
    }

    if (requested > available) {
        requested = available;
    }

    if (!sound_clip_replace_from_ring(
            &audio->clip,
            ring,
            written_samples,
            requested,
            sample_rate,
            label,
            error
        )) {
        return false;
    }

    workbench_mark_clip_changed(audio);
    return true;
}

bool workbench_replace_clip_from_recording(
    WorkbenchAudio *audio,
    const SoundRingBuffer *ring,
    uint64_t started_at,
    uint64_t ended_at,
    double sample_rate,
    SoundError *error
) {
    if (ended_at <= started_at) {
        return true;
    }

    if (!sound_clip_replace_from_ring(
            &audio->clip,
            ring,
            ended_at,
            ended_at - started_at,
            sample_rate,
            "recording",
            error
        )) {
        return false;
    }

    workbench_mark_clip_changed(audio);
    return true;
}

void workbench_apply_trim_event(WorkbenchAudio *audio, const SoundUiEvents *events) {
    if (!sound_clip_has_audio(&audio->clip)) {
        return;
    }

    uint64_t step = sample_count_for_seconds(audio->clip.sample_rate, trim_step_seconds);
    if (step == 0) {
        step = 1;
    }

    if (events->trim_start_delta > 0) {
        sound_clip_trim_start_by(&audio->clip, step * (uint64_t)events->trim_start_delta);
        workbench_mark_clip_changed(audio);
    }

    if (events->trim_end_delta > 0) {
        sound_clip_trim_end_by(&audio->clip, step * (uint64_t)events->trim_end_delta);
        workbench_mark_clip_changed(audio);
    }

    if (events->trim_clear) {
        sound_clip_clear_trim(&audio->clip);
        workbench_mark_clip_changed(audio);
    }
}

bool workbench_toggle_playback(
    SoundPlayback *playback,
    WorkbenchAudio *audio,
    SoundWorkspace workspace,
    SoundError *error
) {
    if (sound_playback_is_playing(playback)) {
        return sound_playback_stop(playback, error);
    }

    if (!sound_clip_has_audio(&audio->clip)) {
        return true;
    }

    const float *samples = sound_clip_samples(&audio->clip);
    uint64_t sample_count = sound_clip_sample_count(&audio->clip);

    if ((workspace == SOUND_WORKSPACE_BAND || workspace == SOUND_WORKSPACE_COMPARE) &&
        audio->audition != SOUND_AUDITION_ORIGINAL) {
        if (!workbench_ensure_band_render(audio, error)) {
            return false;
        }

        samples = audio->audition == SOUND_AUDITION_SELECTED ?
            audio->selected_samples :
            audio->rejected_samples;
        sample_count = audio->render_count;
    }

    return sound_playback_start(
        playback,
        samples,
        sample_count,
        audio->clip.sample_rate,
        error
    );
}

SoundUiWorkbenchState workbench_ui_state(
    const WorkbenchAudio *audio,
    SoundWorkspace workspace,
    bool recording_enabled,
    bool playback_enabled
) {
    return (SoundUiWorkbenchState){
        .workspace = workspace,
        .clip_label = audio->clip.label,
        .method_label = sound_band_render_method_name(audio->band_method),
        .audition_label = audition_name(audio->audition),
        .clip_seconds = sound_clip_duration_seconds(&audio->clip),
        .trim_start_seconds = sound_clip_trim_start_seconds(&audio->clip),
        .trim_end_seconds = sound_clip_trim_end_seconds(&audio->clip),
        .low_hz = audio->low_hz,
        .high_hz = audio->high_hz,
        .recording_enabled = recording_enabled,
        .playback_enabled = playback_enabled,
        .has_clip = sound_clip_has_audio(&audio->clip),
    };
}
