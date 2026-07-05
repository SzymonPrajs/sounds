#include "internal.h"

static uint64_t sample_count_for_seconds(double sample_rate, double seconds) {
    return (uint64_t)(sample_rate * seconds);
}

static void begin_trim_edit(WorkbenchAudio *audio, WorkbenchTrimEdge edge) {
    if (!audio || audio->clip.sample_count == 0) {
        return;
    }

    if (!audio->trim_editing) {
        audio->draft_trim_start = audio->clip.trim_start;
        audio->draft_trim_end = audio->clip.trim_end;
        audio->trim_editing = true;
        audio->active_spectrogram.dirty = true;
    }

    audio->trim_edge = edge;
}

static void move_draft_trim_edge(WorkbenchAudio *audio, int delta_steps) {
    if (!audio || audio->clip.sample_count == 0 || delta_steps == 0) {
        return;
    }

    if (!audio->trim_editing) {
        begin_trim_edit(audio, audio->trim_edge);
    }

    uint64_t step = sample_count_for_seconds(audio->clip.sample_rate, 0.02);
    if (step == 0) {
        step = 1;
    }

    int64_t delta = (int64_t)step * (int64_t)delta_steps;
    if (audio->trim_edge == WORKBENCH_TRIM_START) {
        int64_t next = (int64_t)audio->draft_trim_start + delta;
        int64_t limit = (int64_t)audio->draft_trim_end - 1;

        if (next < 0) {
            next = 0;
        }
        if (next > limit) {
            next = limit;
        }

        audio->draft_trim_start = (uint64_t)next;
    } else {
        int64_t next = (int64_t)audio->draft_trim_end + delta;
        int64_t minimum = (int64_t)audio->draft_trim_start + 1;
        int64_t maximum = (int64_t)audio->clip.sample_count;

        if (next < minimum) {
            next = minimum;
        }
        if (next > maximum) {
            next = maximum;
        }

        audio->draft_trim_end = (uint64_t)next;
    }

    audio->active_spectrogram.dirty = true;
}

void workbench_set_draft_trim_sample(
    WorkbenchAudio *audio,
    bool end_handle,
    uint64_t sample
) {
    if (!audio || audio->clip.sample_count == 0) {
        return;
    }

    begin_trim_edit(
        audio,
        end_handle ? WORKBENCH_TRIM_END : WORKBENCH_TRIM_START
    );

    if (end_handle) {
        uint64_t minimum = audio->draft_trim_start + 1U;
        uint64_t maximum = audio->clip.sample_count;

        if (sample < minimum) {
            sample = minimum;
        }
        if (sample > maximum) {
            sample = maximum;
        }

        audio->draft_trim_end = sample;
    } else {
        uint64_t maximum = audio->draft_trim_end > 0 ?
            audio->draft_trim_end - 1U :
            0;

        if (sample > maximum) {
            sample = maximum;
        }

        audio->draft_trim_start = sample;
    }

    audio->active_spectrogram.dirty = true;
}

void workbench_apply_trim_event(
    WorkbenchAudio *audio,
    const SoundUiEvents *events,
    SoundError *error
) {
    (void)error;

    if (!audio || audio->clip.sample_count == 0) {
        return;
    }

    if (events->trim_select_start) {
        begin_trim_edit(audio, WORKBENCH_TRIM_START);
    }

    if (events->trim_select_end) {
        begin_trim_edit(audio, WORKBENCH_TRIM_END);
    }

    if (events->trim_set_handle) {
        workbench_set_draft_trim_sample(
            audio,
            events->trim_set_handle_end,
            events->trim_set_sample
        );
    }

    if (events->trim_move_delta != 0) {
        move_draft_trim_edge(audio, events->trim_move_delta);
    }

    if (events->trim_commit && audio->trim_editing) {
        audio->clip.trim_start = audio->draft_trim_start;
        audio->clip.trim_end = audio->draft_trim_end;
        audio->trim_editing = false;
        workbench_sync_selected_recording_trim(audio);
        workbench_mark_clip_changed(audio);
    }

    if (events->trim_clear) {
        sound_clip_clear_trim(&audio->clip);
        audio->trim_editing = false;
        workbench_sync_selected_recording_trim(audio);
        workbench_mark_clip_changed(audio);
    }
}
