#ifndef SOUNDS_ANALYSIS_H
#define SOUNDS_ANALYSIS_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The range is matched to what the built-in microphone actually delivers,
 * measured from this app's own raw recordings: the input chain's high-pass
 * collapses at ~35-40 dB/decade below ~20-25 Hz, and the ADC's anti-alias
 * brick wall sits at 23 kHz (the floor is flat to 23.0 kHz, then drops
 * ~30 dB within 500 Hz). Trading the unusable sub-20 Hz octaves for double
 * the voice density gives about one voice per display row.
 *
 * The display ceiling runs to 24 kHz (Nyquist at 48 kHz) even though
 * analysis voices stop near 23 kHz: the empty strip above the brick wall
 * is deliberate, showing where the hardware ends the same way the low
 * edge shows the input high-pass.
 */
#define SOUND_WAVELET_MIN_HZ 20.0
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
