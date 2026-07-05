#ifndef SOUNDS_SETTINGS_H
#define SOUNDS_SETTINGS_H

#include "sounds/app_mode.h"
#include "sounds/colormap.h"
#include "sounds/error.h"
#include "sounds/frequency_band.h"

#include <stdbool.h>

typedef struct SoundSettings {
    SoundAppMode mode;
    SoundColormap colormap;
    SoundFrequencyBand frequency_band;
    double custom_min_hz;
    double custom_max_hz;
} SoundSettings;

void sound_settings_defaults(SoundSettings *settings);
bool sound_settings_load(SoundSettings *settings, SoundError *error);
bool sound_settings_save(const SoundSettings *settings, SoundError *error);

#endif
