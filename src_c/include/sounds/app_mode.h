#ifndef SOUNDS_APP_MODE_H
#define SOUNDS_APP_MODE_H

#include <stdbool.h>

typedef enum SoundAppMode {
    SOUND_APP_MODE_TRANSIENT,
    SOUND_APP_MODE_TONAL,
    SOUND_APP_MODE_REASSIGNED,
    SOUND_APP_MODE_SQUEEZED,
    SOUND_APP_MODE_SUPERLET,
    SOUND_APP_MODE_MULTITAPER,
    SOUND_APP_MODE_S_TRANSFORM,
    SOUND_APP_MODE_SPARSE,
    SOUND_APP_MODE_COUNT,
} SoundAppMode;

int sound_app_mode_count(void);
SoundAppMode sound_app_mode_at(int index);
int sound_app_mode_index(SoundAppMode mode);
const char *sound_app_mode_banner(SoundAppMode mode, bool tonal_sst_enabled);
const char *sound_app_mode_title(SoundAppMode mode, bool tonal_sst_enabled);
float sound_app_mode_column_smoothing(SoundAppMode mode);

#endif
