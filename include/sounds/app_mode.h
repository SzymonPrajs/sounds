#ifndef SOUNDS_APP_MODE_H
#define SOUNDS_APP_MODE_H

#include <stdbool.h>

typedef enum SoundAppMode {
    SOUND_APP_MODE_TRANSIENT,
    SOUND_APP_MODE_TONAL,
    SOUND_APP_MODE_ROOM_DECAY,
} SoundAppMode;

const char *sound_app_mode_banner(SoundAppMode mode, bool tonal_sst_enabled);
const char *sound_app_mode_title(SoundAppMode mode, bool tonal_sst_enabled);
float sound_app_mode_column_smoothing(SoundAppMode mode);

#endif
