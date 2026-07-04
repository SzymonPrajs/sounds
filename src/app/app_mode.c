#include "sounds/app_mode.h"

static const float transient_column_smoothing = 0.9F;
static const float tonal_column_smoothing = 0.6F;
static const float room_decay_column_smoothing = 0.8F;

const char *sound_app_mode_banner(SoundAppMode mode, bool tonal_sst_enabled) {
    switch (mode) {
        case SOUND_APP_MODE_TRANSIENT:
            return "1 TRANSIENT STFT";
        case SOUND_APP_MODE_TONAL:
            return tonal_sst_enabled ?
                "2 TONAL WAVELET SST" :
                "2 TONAL WAVELET RAW";
        case SOUND_APP_MODE_ROOM_DECAY:
            return "3 ROOM DECAY";
    }

    return "1 TRANSIENT STFT";
}

const char *sound_app_mode_title(SoundAppMode mode, bool tonal_sst_enabled) {
    switch (mode) {
        case SOUND_APP_MODE_TRANSIENT:
            return "transient STFT";
        case SOUND_APP_MODE_TONAL:
            return tonal_sst_enabled ?
                "tonal wavelet SST" :
                "tonal wavelet raw CWT";
        case SOUND_APP_MODE_ROOM_DECAY:
            return "room decay";
    }

    return "transient STFT";
}

float sound_app_mode_column_smoothing(SoundAppMode mode) {
    switch (mode) {
        case SOUND_APP_MODE_TRANSIENT:
            return transient_column_smoothing;
        case SOUND_APP_MODE_TONAL:
            return tonal_column_smoothing;
        case SOUND_APP_MODE_ROOM_DECAY:
            return room_decay_column_smoothing;
    }

    return transient_column_smoothing;
}
