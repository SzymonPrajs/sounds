#ifndef SOUNDS_FREQUENCY_BAND_H
#define SOUNDS_FREQUENCY_BAND_H

#include <stdbool.h>

#define SOUND_FREQUENCY_LOW_MAX_HZ 120.0
#define SOUND_FREQUENCY_MID_MAX_HZ 1000.0

typedef enum SoundFrequencyBand {
    SOUND_FREQUENCY_BAND_WHOLE,
    SOUND_FREQUENCY_BAND_LOW,
    SOUND_FREQUENCY_BAND_MID,
    SOUND_FREQUENCY_BAND_HIGH,
    SOUND_FREQUENCY_BAND_CUSTOM,
    SOUND_FREQUENCY_BAND_BANDS,
    SOUND_FREQUENCY_BAND_COUNT,
} SoundFrequencyBand;

int sound_frequency_band_count(void);
SoundFrequencyBand sound_frequency_band_at(int index);
int sound_frequency_band_index(SoundFrequencyBand band);
bool sound_frequency_band_is_valid(SoundFrequencyBand band);
const char *sound_frequency_band_name(SoundFrequencyBand band);
const char *sound_frequency_band_title(SoundFrequencyBand band);
const char *sound_frequency_band_range_label(SoundFrequencyBand band);
bool sound_frequency_band_shows_all_bands(SoundFrequencyBand band);

void sound_frequency_band_limits(
    SoundFrequencyBand band,
    double full_min_hz,
    double full_max_hz,
    double custom_min_hz,
    double custom_max_hz,
    double *min_hz,
    double *max_hz
);

bool sound_frequency_custom_range_is_valid(
    double full_min_hz,
    double full_max_hz,
    double custom_min_hz,
    double custom_max_hz
);

#endif
