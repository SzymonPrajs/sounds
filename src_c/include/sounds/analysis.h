#ifndef SOUNDS_ANALYSIS_H
#define SOUNDS_ANALYSIS_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The range is matched to what the available inputs actually deliver. The
 * built-in microphone's raw recordings collapse below ~20-25 Hz, but a Yeti
 * USB input capture showed measurable 10-20 Hz energy. Keep that one extra
 * octave visible, while leaving sub-10 Hz out as mostly DC/handling rumble with
 * long analysis support.
 *
 * The display ceiling stays at 24 kHz (Nyquist at 48 kHz). The tested Yeti
 * exposes only 44.1/48 kHz input modes, so there is no real content above
 * 24 kHz to draw. Analysis voices still stop at 0.48 of the sample rate so the
 * decimator passband keeps its guard band.
 */
#define SOUND_WAVELET_MIN_HZ 10.0
#define SOUND_WAVELET_MAX_HZ 24000.0
#define SOUND_WAVELET_VOICES_PER_OCTAVE 48U

/* Morlet center-frequency parameter: sets each voice's Q. Matched to the
 * 48-voice grid — at omega0 = 8 a voice's bandwidth (~0.18 octave) spans a
 * handful of neighbors rather than a quarter of the octave. */
#define SOUND_WAVELET_MORLET_OMEGA0 8.0

typedef struct SoundWaveletAnalyzer SoundWaveletAnalyzer;

void sound_compute_levels(
    const float *samples,
    uint64_t sample_count,
    double *rms,
    double *peak
);

bool sound_wavelet_analyzer_create(
    double sample_rate,
    SoundWaveletAnalyzer **analyzer,
    SoundError *error
);

void sound_wavelet_analyzer_destroy(SoundWaveletAnalyzer *analyzer);

double sound_wavelet_analyzer_min_frequency(const SoundWaveletAnalyzer *analyzer);
double sound_wavelet_analyzer_max_frequency(const SoundWaveletAnalyzer *analyzer);
uint64_t sound_wavelet_analyzer_octave_count(const SoundWaveletAnalyzer *analyzer);
uint64_t sound_wavelet_analyzer_voice_count(const SoundWaveletAnalyzer *analyzer);
bool sound_wavelet_analyzer_set_frequency_range(
    SoundWaveletAnalyzer *analyzer,
    double min_hz,
    double max_hz,
    SoundError *error
);

void sound_wavelet_analyzer_set_synchrosqueezed(
    SoundWaveletAnalyzer *analyzer,
    bool enabled
);

bool sound_wavelet_analyzer_synchrosqueezed(const SoundWaveletAnalyzer *analyzer);

bool sound_wavelet_analyzer_push(
    SoundWaveletAnalyzer *analyzer,
    const float *samples,
    uint64_t sample_count,
    SoundError *error
);

bool sound_wavelet_analyzer_snapshot_db(
    SoundWaveletAnalyzer *analyzer,
    float *dbfs_rows,
    uint64_t row_count,
    SoundError *error
);

double sound_wavelet_analyzer_frequency_for_row(
    const SoundWaveletAnalyzer *analyzer,
    uint64_t row,
    uint64_t row_count
);

#endif
