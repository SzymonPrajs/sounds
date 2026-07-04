#include "sounds/frequency_band.h"

#include <stddef.h>

typedef struct SoundFrequencyBandData {
    const char *name;
    const char *title;
    const char *range_label;
} SoundFrequencyBandData;

static const SoundFrequencyBandData frequency_band_data[SOUND_FREQUENCY_BAND_COUNT] = {
    [SOUND_FREQUENCY_BAND_WHOLE] = {"whole", "whole spectrogram", "10 Hz-24 kHz"},
    [SOUND_FREQUENCY_BAND_LOW] = {"low", "low frequencies", "10-120 Hz"},
    [SOUND_FREQUENCY_BAND_MID] = {"mid", "mid frequencies", "120 Hz-1 kHz"},
    [SOUND_FREQUENCY_BAND_HIGH] = {"high", "high frequencies", "1-24 kHz"},
    [SOUND_FREQUENCY_BAND_CUSTOM] = {"custom", "custom range", "typed low / high"},
    [SOUND_FREQUENCY_BAND_BANDS] = {"bands", "banded spectrogram", "low / mid / high"},
};

static const SoundFrequencyBand frequency_band_order[] = {
    SOUND_FREQUENCY_BAND_WHOLE,
    SOUND_FREQUENCY_BAND_LOW,
    SOUND_FREQUENCY_BAND_MID,
    SOUND_FREQUENCY_BAND_HIGH,
    SOUND_FREQUENCY_BAND_CUSTOM,
    SOUND_FREQUENCY_BAND_BANDS,
};

static double larger_double(double a, double b) {
    return a > b ? a : b;
}

static double smaller_double(double a, double b) {
    return a < b ? a : b;
}

int sound_frequency_band_count(void) {
    return (int)(sizeof(frequency_band_order) / sizeof(frequency_band_order[0]));
}

SoundFrequencyBand sound_frequency_band_at(int index) {
    if (index < 0 || index >= sound_frequency_band_count()) {
        return SOUND_FREQUENCY_BAND_WHOLE;
    }

    return frequency_band_order[index];
}

int sound_frequency_band_index(SoundFrequencyBand band) {
    for (int i = 0; i < sound_frequency_band_count(); ++i) {
        if (frequency_band_order[i] == band) {
            return i;
        }
    }

    return 0;
}

bool sound_frequency_band_is_valid(SoundFrequencyBand band) {
    return sound_frequency_band_index(band) >= 0 &&
        sound_frequency_band_at(sound_frequency_band_index(band)) == band;
}

const char *sound_frequency_band_name(SoundFrequencyBand band) {
    if (!sound_frequency_band_is_valid(band)) {
        band = SOUND_FREQUENCY_BAND_WHOLE;
    }

    return frequency_band_data[band].name;
}

const char *sound_frequency_band_title(SoundFrequencyBand band) {
    if (!sound_frequency_band_is_valid(band)) {
        band = SOUND_FREQUENCY_BAND_WHOLE;
    }

    return frequency_band_data[band].title;
}

const char *sound_frequency_band_range_label(SoundFrequencyBand band) {
    if (!sound_frequency_band_is_valid(band)) {
        band = SOUND_FREQUENCY_BAND_WHOLE;
    }

    return frequency_band_data[band].range_label;
}

bool sound_frequency_band_shows_all_bands(SoundFrequencyBand band) {
    return band == SOUND_FREQUENCY_BAND_BANDS;
}

void sound_frequency_band_limits(
    SoundFrequencyBand band,
    double full_min_hz,
    double full_max_hz,
    double custom_min_hz,
    double custom_max_hz,
    double *min_hz,
    double *max_hz
) {
    double low = full_min_hz;
    double high = full_max_hz;

    if (full_min_hz <= 0.0 || full_max_hz <= full_min_hz) {
        if (min_hz) {
            *min_hz = 0.0;
        }

        if (max_hz) {
            *max_hz = 0.0;
        }

        return;
    }

    switch (band) {
        case SOUND_FREQUENCY_BAND_LOW:
            high = smaller_double(full_max_hz, SOUND_FREQUENCY_LOW_MAX_HZ);
            break;
        case SOUND_FREQUENCY_BAND_MID:
            low = larger_double(full_min_hz, SOUND_FREQUENCY_LOW_MAX_HZ);
            high = smaller_double(full_max_hz, SOUND_FREQUENCY_MID_MAX_HZ);
            break;
        case SOUND_FREQUENCY_BAND_HIGH:
            low = larger_double(full_min_hz, SOUND_FREQUENCY_MID_MAX_HZ);
            break;
        case SOUND_FREQUENCY_BAND_CUSTOM:
            if (sound_frequency_custom_range_is_valid(
                    full_min_hz,
                    full_max_hz,
                    custom_min_hz,
                    custom_max_hz
                )) {
                low = larger_double(full_min_hz, custom_min_hz);
                high = smaller_double(full_max_hz, custom_max_hz);
            }
            break;
        case SOUND_FREQUENCY_BAND_WHOLE:
        case SOUND_FREQUENCY_BAND_BANDS:
        case SOUND_FREQUENCY_BAND_COUNT:
            break;
    }

    if (high <= low) {
        low = full_min_hz;
        high = full_max_hz;
    }

    if (min_hz) {
        *min_hz = low;
    }

    if (max_hz) {
        *max_hz = high;
    }
}

bool sound_frequency_custom_range_is_valid(
    double full_min_hz,
    double full_max_hz,
    double custom_min_hz,
    double custom_max_hz
) {
    return full_min_hz > 0.0 &&
        full_max_hz > full_min_hz &&
        custom_min_hz >= full_min_hz &&
        custom_max_hz <= full_max_hz &&
        custom_max_hz > custom_min_hz * 1.01;
}
