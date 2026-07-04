#ifndef SOUNDS_APP_WORKBENCH_H
#define SOUNDS_APP_WORKBENCH_H

#include "sounds/band_render.h"
#include "sounds/clip.h"
#include "sounds/error.h"
#include "sounds/playback.h"
#include "sounds/ring_buffer.h"
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

typedef struct WorkbenchAudio {
    SoundClip clip;
    float *spectrum_rows;
    uint64_t spectrum_row_count;
    float *selected_samples;
    float *rejected_samples;
    uint64_t render_count;
    double low_hz;
    double high_hz;
    SoundBandRenderMethod band_method;
    SoundAuditionTarget audition;
    bool upper_handle_selected;
    bool spectrum_dirty;
    bool render_dirty;
} WorkbenchAudio;

void workbench_audio_init(WorkbenchAudio *audio);
void workbench_audio_free(WorkbenchAudio *audio);
void workbench_mark_clip_changed(WorkbenchAudio *audio);
void workbench_cycle_audition(WorkbenchAudio *audio);
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

bool workbench_ensure_spectrum_rows(
    WorkbenchAudio *audio,
    uint64_t row_count,
    double sample_rate,
    double min_hz,
    double max_hz,
    SoundError *error
);

bool workbench_ensure_band_render(WorkbenchAudio *audio, SoundError *error);

bool workbench_freeze_recent_clip(
    WorkbenchAudio *audio,
    const SoundRingBuffer *ring,
    uint64_t written_samples,
    double sample_rate,
    const char *label,
    SoundError *error
);

bool workbench_replace_clip_from_recording(
    WorkbenchAudio *audio,
    const SoundRingBuffer *ring,
    uint64_t started_at,
    uint64_t ended_at,
    double sample_rate,
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
    bool playback_enabled
);

#endif
