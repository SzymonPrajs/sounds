#ifndef SOUNDS_UI_H
#define SOUNDS_UI_H

#include "sounds/app_mode.h"
#include "sounds/error.h"

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
} SoundUiConfig;

typedef struct SoundUiEvents {
    bool quit;
    bool toggle_sst;
    bool mode_changed;
    SoundAppMode mode;
} SoundUiEvents;

typedef struct SoundUiTitle {
    double sample_rate;
    double rms;
    double peak;
    double min_hz;
    double max_hz;
    SoundAppMode mode;
    bool sst_enabled;
} SoundUiTitle;

bool sound_ui_create(
    const SoundUiConfig *config,
    SoundUi **ui,
    SoundError *error
);

void sound_ui_destroy(SoundUi *ui);

void sound_ui_poll_events(
    SoundUi *ui,
    SoundAppMode current_mode,
    SoundUiEvents *events
);

bool sound_ui_sync(SoundUi *ui, SoundError *error);
uint64_t sound_ui_spectrogram_rows(const SoundUi *ui);

void sound_ui_clear_spectrogram(SoundUi *ui);

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
    bool sst_enabled
);

void sound_ui_set_title(SoundUi *ui, const SoundUiTitle *title);
void sound_ui_present(SoundUi *ui);

#endif
