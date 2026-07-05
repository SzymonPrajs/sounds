#include "internal.h"

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

SoundUiWorkbenchState workbench_ui_state(
    const WorkbenchAudio *audio,
    SoundWorkspace workspace,
    bool recording_enabled,
    bool playback_enabled,
    uint64_t playback_position
) {
    uint64_t playback_sample = audio->playback_offset + playback_position;
    if (playback_sample > audio->clip.sample_count) {
        playback_sample = audio->clip.sample_count;
    }

    double clip_seconds = 0.0;
    double active_seconds = 0.0;
    double trim_start_seconds = 0.0;
    double trim_end_seconds = 0.0;
    if (audio->clip.sample_rate > 0.0) {
        uint64_t active_start = audio->trim_editing ?
            audio->draft_trim_start :
            audio->clip.trim_start;
        uint64_t active_end = audio->trim_editing ?
            audio->draft_trim_end :
            audio->clip.trim_end;

        if (active_start > audio->clip.sample_count) {
            active_start = audio->clip.sample_count;
        }
        if (active_end > audio->clip.sample_count) {
            active_end = audio->clip.sample_count;
        }
        if (active_end < active_start) {
            active_end = active_start;
        }

        clip_seconds = (double)audio->clip.sample_count / audio->clip.sample_rate;
        active_seconds = (double)(active_end - active_start) / audio->clip.sample_rate;
        trim_start_seconds = (double)active_start / audio->clip.sample_rate;
        trim_end_seconds = (double)active_end / audio->clip.sample_rate;
    }

    bool band_render_ready =
        !audio->render_dirty &&
        audio->selected_samples &&
        audio->render_count > 0U;
    bool band_spectrogram_ready =
        band_render_ready &&
        !audio->band_spectrogram.dirty &&
        audio->band_spectrogram.cells &&
        audio->band_spectrogram.columns > 0U &&
        audio->band_spectrogram.rows > 0U;

    return (SoundUiWorkbenchState){
        .workspace = workspace,
        .clip_label = audio->clip.label,
        .method_label = sound_band_render_method_short_name(audio->band_method),
        .audition_label = audition_name(audio->audition),
        .recordings = audio->recording_summaries,
        .band_spectrogram_cells =
            band_spectrogram_ready ? audio->band_spectrogram.cells : NULL,
        .filtered_samples = band_render_ready ? audio->selected_samples : NULL,
        .clip_seconds = clip_seconds,
        .active_seconds = active_seconds,
        .trim_start_seconds = trim_start_seconds,
        .trim_end_seconds = trim_end_seconds,
        .low_hz = audio->low_hz,
        .high_hz = audio->high_hz,
        .source_sample_count = audio->clip.sample_count,
        .trim_start_sample = audio->clip.trim_start,
        .trim_end_sample = audio->clip.trim_end,
        .draft_trim_start_sample = audio->trim_editing ?
            audio->draft_trim_start :
            audio->clip.trim_start,
        .draft_trim_end_sample = audio->trim_editing ?
            audio->draft_trim_end :
            audio->clip.trim_end,
        .playback_sample = playback_sample,
        .band_spectrogram_columns =
            band_spectrogram_ready ? audio->band_spectrogram.columns : 0U,
        .band_spectrogram_rows =
            band_spectrogram_ready ? audio->band_spectrogram.rows : 0U,
        .filtered_sample_count = band_render_ready ? audio->render_count : 0U,
        .recording_count = audio->recording_count,
        .recording_index = audio->selected_recording,
        .active_recording_index = audio->active_recording,
        .recording_scan_complete = audio->recording_scan_complete,
        .recording_scan_failed = audio->recording_scan_failed,
        .recording_enabled = recording_enabled,
        .playback_enabled = playback_enabled,
        .playback_cursor_visible =
            playback_enabled && audio->clip.sample_count > 0,
        .has_clip = sound_clip_has_audio(&audio->clip),
        .has_active_recording = audio->has_active_recording,
        .upper_band_selected = audio->upper_handle_selected,
        .trim_editing = audio->trim_editing,
        .trim_end_selected = audio->trim_edge == WORKBENCH_TRIM_END,
        .recording_rename_active = audio->recording_rename_active,
        .recording_delete_pending =
            audio->recording_delete_pending &&
            audio->recording_delete_index == audio->selected_recording,
    };
}
