#ifndef SOUNDS_APP_WORKBENCH_H
#define SOUNDS_APP_WORKBENCH_H

#include "sounds/band_render.h"
#include "sounds/clip.h"
#include "sounds/error.h"
#include "sounds/playback.h"
#include "sounds/recording.h"
#include "sounds/ui.h"
#include "sounds/workspace.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum SoundAuditionTarget {
    SOUND_AUDITION_ORIGINAL,
    SOUND_AUDITION_SELECTED,
    SOUND_AUDITION_REJECTED,
    SOUND_AUDITION_COUNT,
} SoundAuditionTarget;

typedef enum WorkbenchTrimEdge {
    WORKBENCH_TRIM_START,
    WORKBENCH_TRIM_END,
} WorkbenchTrimEdge;

typedef struct WorkbenchRecordingScan WorkbenchRecordingScan;

typedef struct WorkbenchRecording {
    SoundClip clip;
    SoundUiRecordingSummary summary;
    char path[SOUND_RECORDING_PATH_CAPACITY];
    uint64_t created_at;
    bool loaded;
    bool disk_backed;
} WorkbenchRecording;

typedef struct WorkbenchAudio {
    SoundClip clip;
    WorkbenchRecording *recordings;
    SoundUiRecordingSummary *recording_summaries;
    uint64_t recording_count;
    uint64_t recording_capacity;
    uint64_t selected_recording;
    uint64_t active_recording;
    WorkbenchRecordingScan *recording_scan;
    bool recording_scan_complete;
    bool recording_scan_failed;
    float *spectrum_cells;
    uint64_t spectrum_row_count;
    double spectrum_min_hz;
    double spectrum_max_hz;
    float *selected_samples;
    float *rejected_samples;
    uint64_t render_count;
    double low_hz;
    double high_hz;
    uint64_t playback_offset;
    SoundBandRenderMethod band_method;
    SoundAuditionTarget audition;
    WorkbenchTrimEdge trim_edge;
    bool upper_handle_selected;
    bool trim_editing;
    bool has_active_recording;
    bool recording_rename_active;
    bool recording_delete_pending;
    bool spectrum_dirty;
    bool render_dirty;
    uint64_t draft_trim_start;
    uint64_t draft_trim_end;
    uint64_t recording_delete_index;
    char recording_rename_text[SOUND_UI_RECORDING_LABEL_CAPACITY];
} WorkbenchAudio;

void workbench_audio_init(WorkbenchAudio *audio);
void workbench_audio_free(WorkbenchAudio *audio);
void workbench_mark_clip_changed(WorkbenchAudio *audio);
bool workbench_start_recording_scan(WorkbenchAudio *audio, SoundError *error);
bool workbench_poll_recording_scan(WorkbenchAudio *audio, SoundError *error);
void workbench_cycle_audition(WorkbenchAudio *audio);
bool workbench_cycle_recording(
    WorkbenchAudio *audio,
    int delta,
    SoundError *error
);
bool workbench_select_recording(
    WorkbenchAudio *audio,
    SoundError *error
);
bool workbench_delete_selected_recording(
    WorkbenchAudio *audio,
    SoundError *error
);
void workbench_begin_recording_rename(WorkbenchAudio *audio);
void workbench_cancel_recording_rename(WorkbenchAudio *audio);
void workbench_recording_rename_backspace(WorkbenchAudio *audio);
void workbench_append_recording_rename_text(WorkbenchAudio *audio, const char *text);
bool workbench_commit_recording_rename(
    WorkbenchAudio *audio,
    SoundError *error
);
void workbench_cycle_band_method(WorkbenchAudio *audio);
void workbench_cycle_band_handle(WorkbenchAudio *audio);
void workbench_move_selected_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
);
void workbench_move_lower_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
);
void workbench_move_upper_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
);
void workbench_apply_trim_event(
    WorkbenchAudio *audio,
    const SoundUiEvents *events,
    SoundError *error
);

bool workbench_ensure_spectrum(
    WorkbenchAudio *audio,
    uint64_t row_count,
    double sample_rate,
    double min_hz,
    double max_hz,
    SoundError *error
);

bool workbench_ensure_band_render(WorkbenchAudio *audio, SoundError *error);

bool workbench_add_recording_from_samples(
    WorkbenchAudio *audio,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    const char *path,
    SoundError *error
);

bool workbench_ensure_selected_recording_loaded(
    WorkbenchAudio *audio,
    SoundError *error
);

bool workbench_toggle_playback(
    SoundPlayback *playback,
    WorkbenchAudio *audio,
    SoundWorkspace workspace,
    SoundError *error
);

SoundUiWorkbenchState workbench_ui_state(
    const WorkbenchAudio *audio,
    SoundWorkspace workspace,
    bool recording_enabled,
    bool playback_enabled,
    uint64_t playback_position
);

#endif
