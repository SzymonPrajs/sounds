#ifndef SOUNDS_UI_H
#define SOUNDS_UI_H

#include "sounds/app_mode.h"
#include "sounds/colormap.h"
#include "sounds/error.h"
#include "sounds/workspace.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SoundUi SoundUi;

typedef struct SoundUiConfig {
    const char *app_name;
    const char *app_identifier;
    const char *app_version;
    int initial_width;
    int initial_height;
    int minimum_width;
    int minimum_height;
    double min_hz;
    double max_hz;
    SoundColormap colormap;
} SoundUiConfig;

typedef struct SoundUiEvents {
    bool quit;
    bool toggle_sst;
    bool toggle_recording;
    bool toggle_playback;
    bool cycle_audition;
    bool cycle_band_method;
    bool cycle_band_handle;
    bool trim_clear;
    bool mode_changed;
    bool colormap_changed;
    bool workspace_changed;
    int selected_band_delta;
    int lower_band_delta;
    int upper_band_delta;
    int trim_start_delta;
    int trim_end_delta;
    int recording_delta;
    SoundAppMode mode;
    SoundColormap colormap;
    SoundWorkspace workspace;
} SoundUiEvents;

typedef struct SoundUiTitle {
    double sample_rate;
    double rms;
    double peak;
    double min_hz;
    double max_hz;
    SoundAppMode mode;
    SoundWorkspace workspace;
    SoundColormap colormap;
    bool sst_enabled;
    bool recording_enabled;
    bool playback_enabled;
} SoundUiTitle;

enum {
    SOUND_UI_RECORDING_LABEL_CAPACITY = 64,
    SOUND_UI_RECORDING_CREATED_CAPACITY = 32,
};

typedef struct SoundUiRecordingSummary {
    char label[SOUND_UI_RECORDING_LABEL_CAPACITY];
    char created[SOUND_UI_RECORDING_CREATED_CAPACITY];
    double seconds;
    bool loaded;
} SoundUiRecordingSummary;

typedef struct SoundUiWorkbenchState {
    SoundWorkspace workspace;
    const char *clip_label;
    const char *method_label;
    const char *audition_label;
    const SoundUiRecordingSummary *recordings;
    double clip_seconds;
    double active_seconds;
    double trim_start_seconds;
    double trim_end_seconds;
    double low_hz;
    double high_hz;
    uint64_t source_sample_count;
    uint64_t trim_start_sample;
    uint64_t trim_end_sample;
    uint64_t playback_sample;
    uint64_t recording_count;
    uint64_t recording_index;
    bool recording_scan_complete;
    bool recording_scan_failed;
    bool recording_enabled;
    bool playback_enabled;
    bool playback_cursor_visible;
    bool has_clip;
    bool upper_band_selected;
} SoundUiWorkbenchState;

bool sound_ui_create(
    const SoundUiConfig *config,
    SoundUi **ui,
    SoundError *error
);

void sound_ui_destroy(SoundUi *ui);

void sound_ui_poll_events(
    SoundUi *ui,
    SoundAppMode current_mode,
    SoundWorkspace current_workspace,
    SoundUiEvents *events
);

bool sound_ui_sync(SoundUi *ui, SoundError *error);
uint64_t sound_ui_spectrogram_rows(const SoundUi *ui);
uint64_t sound_ui_spectrogram_columns(const SoundUi *ui);
SoundColormap sound_ui_colormap(const SoundUi *ui);
bool sound_ui_menu_open(const SoundUi *ui);

void sound_ui_clear_spectrogram(SoundUi *ui);
void sound_ui_recolor_spectrogram(SoundUi *ui);
void sound_ui_mark_spectrogram_transition(SoundUi *ui);

void sound_ui_draw_spectrogram_columns(
    SoundUi *ui,
    const float *columns,
    uint64_t column_count,
    uint64_t row_count,
    SoundAppMode mode
);

void sound_ui_draw_waveform(
    SoundUi *ui,
    const float *samples,
    uint64_t sample_count,
    double peak
);

void sound_ui_draw_banner(
    SoundUi *ui,
    SoundAppMode mode,
    SoundWorkspace workspace,
    bool sst_enabled,
    bool recording_enabled,
    double recording_seconds,
    bool playback_enabled
);

void sound_ui_draw_menu(
    SoundUi *ui,
    SoundAppMode mode,
    SoundWorkspace workspace,
    bool sst_enabled,
    bool recording_enabled,
    bool playback_enabled
);

void sound_ui_draw_workbench_tabs(SoundUi *ui, const SoundUiWorkbenchState *state);
void sound_ui_draw_empty_workspace(SoundUi *ui, const SoundUiWorkbenchState *state);
void sound_ui_draw_clip_workspace(
    SoundUi *ui,
    const float *samples,
    uint64_t sample_count,
    const float *db_columns,
    uint64_t row_count,
    uint64_t column_count,
    const SoundUiWorkbenchState *state
);
void sound_ui_draw_spectrum_workspace(
    SoundUi *ui,
    const float *db_columns,
    uint64_t row_count,
    uint64_t column_count,
    const SoundUiWorkbenchState *state
);
void sound_ui_draw_compare_workspace(
    SoundUi *ui,
    const float *source,
    uint64_t source_count,
    const float *rendered,
    uint64_t rendered_count,
    const SoundUiWorkbenchState *state
);

void sound_ui_set_title(SoundUi *ui, const SoundUiTitle *title);
void sound_ui_present(SoundUi *ui);

#endif
