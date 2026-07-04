#include "sounds/app_mode.h"

static const float transient_column_smoothing = 0.9F;
static const float tonal_column_smoothing = 0.6F;
static const float reassigned_column_smoothing = 0.85F;
static const float squeezed_column_smoothing = 0.85F;
static const float superlet_column_smoothing = 0.75F;
static const float multitaper_column_smoothing = 0.75F;
static const float s_transform_column_smoothing = 0.85F;
static const float sparse_column_smoothing = 0.80F;

static const SoundAppMode app_modes[] = {
    SOUND_APP_MODE_TRANSIENT,
    SOUND_APP_MODE_TONAL,
    SOUND_APP_MODE_REASSIGNED,
    SOUND_APP_MODE_SQUEEZED,
    SOUND_APP_MODE_SUPERLET,
    SOUND_APP_MODE_MULTITAPER,
    SOUND_APP_MODE_S_TRANSFORM,
    SOUND_APP_MODE_SPARSE,
};

int sound_app_mode_count(void) {
    return (int)(sizeof(app_modes) / sizeof(app_modes[0]));
}

SoundAppMode sound_app_mode_at(int index) {
    if (index < 0 || index >= sound_app_mode_count()) {
        return SOUND_APP_MODE_TRANSIENT;
    }

    return app_modes[index];
}

int sound_app_mode_index(SoundAppMode mode) {
    for (int i = 0; i < sound_app_mode_count(); ++i) {
        if (app_modes[i] == mode) {
            return i;
        }
    }

    return 0;
}

const char *sound_app_mode_banner(SoundAppMode mode, bool tonal_sst_enabled) {
    switch (mode) {
        case SOUND_APP_MODE_TRANSIENT:
            return "1 TRANSIENT STFT";
        case SOUND_APP_MODE_TONAL:
            return tonal_sst_enabled ?
                "2 TONAL WAVELET SST" :
                "2 TONAL WAVELET RAW";
        case SOUND_APP_MODE_REASSIGNED:
            return "3 REASSIGNED STFT";
        case SOUND_APP_MODE_SQUEEZED:
            return "4 SQUEEZED STFT";
        case SOUND_APP_MODE_SUPERLET:
            return "5 SUPERLET";
        case SOUND_APP_MODE_MULTITAPER:
            return "6 MULTITAPER";
        case SOUND_APP_MODE_S_TRANSFORM:
            return "7 S TRANSFORM";
        case SOUND_APP_MODE_SPARSE:
            return "8 SPARSE RIDGES";
        case SOUND_APP_MODE_COUNT:
            break;
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
        case SOUND_APP_MODE_REASSIGNED:
            return "reassigned STFT";
        case SOUND_APP_MODE_SQUEEZED:
            return "squeezed STFT";
        case SOUND_APP_MODE_SUPERLET:
            return "superlet";
        case SOUND_APP_MODE_MULTITAPER:
            return "multitaper";
        case SOUND_APP_MODE_S_TRANSFORM:
            return "S-transform";
        case SOUND_APP_MODE_SPARSE:
            return "sparse ridges";
        case SOUND_APP_MODE_COUNT:
            break;
    }

    return "transient STFT";
}

float sound_app_mode_column_smoothing(SoundAppMode mode) {
    switch (mode) {
        case SOUND_APP_MODE_TRANSIENT:
            return transient_column_smoothing;
        case SOUND_APP_MODE_TONAL:
            return tonal_column_smoothing;
        case SOUND_APP_MODE_REASSIGNED:
            return reassigned_column_smoothing;
        case SOUND_APP_MODE_SQUEEZED:
            return squeezed_column_smoothing;
        case SOUND_APP_MODE_SUPERLET:
            return superlet_column_smoothing;
        case SOUND_APP_MODE_MULTITAPER:
            return multitaper_column_smoothing;
        case SOUND_APP_MODE_S_TRANSFORM:
            return s_transform_column_smoothing;
        case SOUND_APP_MODE_SPARSE:
            return sparse_column_smoothing;
        case SOUND_APP_MODE_COUNT:
            break;
    }

    return transient_column_smoothing;
}
