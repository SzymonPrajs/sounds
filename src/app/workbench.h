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
    WorkbenchRecordingScan *recording_scan;
    bool recording_scan_complete;
    bool recording_scan_failed;
    float *spectrum_cells;
    uint64_t spectrum_row_count;
    uint64_t spectrum_column_count;
    float *selected_samples;
    float *rejected_samples;
    uint64_t render_count;
    double low_hz;
    double high_hz;
    uint64_t playback_offset;
    SoundBandRenderMethod band_method;
    SoundAuditionTarget audition;
    bool upper_handle_selected;
    bool spectrum_dirty;
    bool render_dirty;
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
void workbench_apply_trim_event(WorkbenchAudio *audio, const SoundUiEvents *events);

bool workbench_ensure_spectrogram(
    WorkbenchAudio *audio,
    uint64_t row_count,
    uint64_t column_count,
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
