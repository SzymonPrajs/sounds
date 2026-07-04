#include "sounds/analysis.h"

#include <Accelerate/Accelerate.h>

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    decimator_tap_count = 127,
};

static const double pi_value = 3.14159265358979323846264338327950288;
static const double log_two = 0.693147180559945309417232121458176568;
static const double decimator_cutoff_cycles = 0.225;
static const double morlet_support_sigmas = 8.0;
static const double display_db_floor = -140.0;
static const double display_linear_gain = 0.25;
static const double squeeze_relative_threshold = 1.0e-4;
static const double squeeze_absolute_threshold = 1.0e-9;

typedef struct SoundDecimator {
    float taps[decimator_tap_count];
    float history[decimator_tap_count];
    float ordered[decimator_tap_count];
    uint64_t samples_seen;
    uint64_t samples_ready;
    uint32_t cursor;
} SoundDecimator;

typedef struct SoundComplexValue {
    double real;
    double imag;
} SoundComplexValue;

typedef struct SoundWaveletVoice {
    double center_hz;
    double scale_samples;
    uint64_t half_support;
    uint64_t extract_index;
    float *kernel_real;
    float *kernel_imag;
    float *derivative_real;
    float *derivative_imag;
    SoundComplexValue coefficient;
    SoundComplexValue derivative;
    double magnitude;
    double instantaneous_hz;
    bool ready;
} SoundWaveletVoice;

typedef struct SoundWaveletLevel {
    double sample_rate;
    double octave_low_hz;
    double octave_high_hz;
    uint64_t voice_count;
    uint64_t max_half_support;
    uint64_t block_size;
    uint64_t fft_size;
    unsigned int log2_fft_size;
    FFTSetup fft_setup;
    SoundWaveletVoice *voices;
    float *history;
    float *block;
    float *fft_real;
    float *fft_imag;
    float *work_real;
    float *work_imag;
    uint64_t history_capacity;
    uint64_t history_written;
    uint64_t history_cursor;
    SoundDecimator decimator;
} SoundWaveletLevel;

struct SoundWaveletAnalyzer {
    double sample_rate;
    double min_hz;
    double max_hz;
    uint64_t octave_count;
    uint64_t total_voice_count;
    SoundWaveletLevel *levels;
    bool synchrosqueezed;
    float *raw_rows;
    float *squeezed_rows;
    uint64_t row_capacity;
};

static double clamp_double(double value, double minimum, double maximum) {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static double log2_double(double value) {
    return log(value) / log_two;
}

static uint64_t next_power_of_two(uint64_t value, unsigned int *log2_value) {
    uint64_t result = 1;
    unsigned int exponent = 0;

    while (result < value) {
        result <<= 1U;
        ++exponent;
    }

    if (log2_value) {
        *log2_value = exponent;
    }

    return result;
}

static bool multiply_overflows_size(uint64_t count, size_t element_size) {
    return count > (uint64_t)(SIZE_MAX / element_size);
}

static void design_decimator_taps(float *taps) {
    double sum = 0.0;
    double middle = (double)(decimator_tap_count - 1) * 0.5;

    for (uint64_t i = 0; i < decimator_tap_count; ++i) {
        double offset = (double)i - middle;
        double sinc;

        if (fabs(offset) < 1.0e-12) {
            sinc = 2.0 * decimator_cutoff_cycles;
        } else {
            double angle = 2.0 * pi_value * decimator_cutoff_cycles * offset;
            sinc = sin(angle) / (pi_value * offset);
        }

        double unit = (double)i / (double)(decimator_tap_count - 1);
        double window =
            0.42 - 0.50 * cos(2.0 * pi_value * unit) + 0.08 * cos(4.0 * pi_value * unit);
        double value = sinc * window;

        taps[i] = (float)value;
        sum += value;
    }

    if (sum == 0.0) {
        return;
    }

    for (uint64_t i = 0; i < decimator_tap_count; ++i) {
        taps[i] = (float)((double)taps[i] / sum);
    }
}

static void sound_decimator_init(SoundDecimator *decimator, const float *taps) {
    memset(decimator, 0, sizeof(*decimator));
    memcpy(decimator->taps, taps, sizeof(decimator->taps));
}

static bool sound_decimator_push(SoundDecimator *decimator, float sample, float *output) {
    decimator->history[decimator->cursor] = sample;
    decimator->cursor = (decimator->cursor + 1U) % decimator_tap_count;
    ++decimator->samples_seen;

    if (decimator->samples_ready < decimator_tap_count) {
        ++decimator->samples_ready;
    }

    if (decimator->samples_ready < decimator_tap_count ||
        (decimator->samples_seen & 1U) != 0U) {
        return false;
    }

    uint32_t latest = (decimator->cursor + decimator_tap_count - 1U) % decimator_tap_count;
    for (uint64_t i = 0; i < decimator_tap_count; ++i) {
        uint32_t index = (latest + decimator_tap_count - (uint32_t)i) % decimator_tap_count;
        decimator->ordered[i] = decimator->history[index];
    }

    vDSP_dotpr(
        decimator->taps,
        1,
        decimator->ordered,
        1,
        output,
        (vDSP_Length)decimator_tap_count
    );
    return true;
}

void sound_compute_levels(
    const float *samples,
    uint64_t sample_count,
    double *rms,
    double *peak
) {
    double sumsq = 0.0;
    double max_absolute = 0.0;

    for (uint64_t i = 0; i < sample_count; ++i) {
        double value = samples[i];
        double absolute = fabs(value);

        if (absolute > max_absolute) {
            max_absolute = absolute;
        }

        sumsq += value * value;
    }

    *rms = sample_count == 0 ? 0.0 : sqrt(sumsq / (double)sample_count);
    *peak = max_absolute;
}

static void wavelet_level_free(SoundWaveletLevel *level) {
    if (!level) {
        return;
    }

    if (level->voices) {
        for (uint64_t i = 0; i < level->voice_count; ++i) {
            free(level->voices[i].kernel_real);
            free(level->voices[i].kernel_imag);
            free(level->voices[i].derivative_real);
            free(level->voices[i].derivative_imag);
        }
    }

    if (level->fft_setup) {
        vDSP_destroy_fftsetup(level->fft_setup);
    }

    free(level->voices);
    free(level->history);
    free(level->block);
    free(level->fft_real);
    free(level->fft_imag);
    free(level->work_real);
    free(level->work_imag);
    memset(level, 0, sizeof(*level));
}

static bool allocate_float_buffer(float **buffer, uint64_t count) {
    if (multiply_overflows_size(count, sizeof(float))) {
        return false;
    }

    *buffer = calloc((size_t)count, sizeof(float));
    return *buffer != NULL;
}

static bool precompute_voice_kernel(
    SoundWaveletLevel *level,
    SoundWaveletVoice *voice,
    SoundError *error
) {
    uint64_t kernel_size = voice->half_support * 2U + 1U;

    if (!allocate_float_buffer(&voice->kernel_real, level->fft_size) ||
        !allocate_float_buffer(&voice->kernel_imag, level->fft_size) ||
        !allocate_float_buffer(&voice->derivative_real, level->fft_size) ||
        !allocate_float_buffer(&voice->derivative_imag, level->fft_size)) {
        sound_error_set(error, "could not allocate Morlet wavelet kernels");
        return false;
    }

    double morlet_norm = pow(pi_value, -0.25) / sqrt(voice->scale_samples);
    double inverse_scale_seconds = level->sample_rate / voice->scale_samples;

    for (uint64_t i = 0; i < kernel_size; ++i) {
        double u = ((double)voice->half_support - (double)i) / voice->scale_samples;
        double envelope = morlet_norm * exp(-0.5 * u * u);
        double phase = -SOUND_WAVELET_MORLET_OMEGA0 * u;
        double real = envelope * cos(phase);
        double imag = envelope * sin(phase);
        double derivative_real = inverse_scale_seconds *
            (u * real - SOUND_WAVELET_MORLET_OMEGA0 * imag);
        double derivative_imag = inverse_scale_seconds *
            (u * imag + SOUND_WAVELET_MORLET_OMEGA0 * real);

        voice->kernel_real[i] = (float)real;
        voice->kernel_imag[i] = (float)imag;
        voice->derivative_real[i] = (float)derivative_real;
        voice->derivative_imag[i] = (float)derivative_imag;
    }

    DSPSplitComplex kernel = {
        .realp = voice->kernel_real,
        .imagp = voice->kernel_imag,
    };
    DSPSplitComplex derivative = {
        .realp = voice->derivative_real,
        .imagp = voice->derivative_imag,
    };

    vDSP_fft_zip(
        level->fft_setup,
        &kernel,
        1,
        (vDSP_Length)level->log2_fft_size,
        FFT_FORWARD
    );
    vDSP_fft_zip(
        level->fft_setup,
        &derivative,
        1,
        (vDSP_Length)level->log2_fft_size,
        FFT_FORWARD
    );
    return true;
}

static uint64_t count_level_voices(
    double octave_low_hz,
    double octave_high_hz,
    double min_hz,
    double max_hz
) {
    uint64_t count = 0;

    for (uint64_t i = 0; i < SOUND_WAVELET_VOICES_PER_OCTAVE; ++i) {
        double unit = ((double)i + 0.5) / (double)SOUND_WAVELET_VOICES_PER_OCTAVE;
        double center = octave_low_hz * pow(2.0, unit);

        if (center >= min_hz && center <= max_hz && center < octave_high_hz) {
            ++count;
        }
    }

    return count;
}

static bool wavelet_level_init(
    SoundWaveletLevel *level,
    uint64_t octave_index,
    double sample_rate,
    double octave_low_hz,
    double octave_high_hz,
    double min_hz,
    double max_hz,
    const float *decimator_taps,
    SoundError *error
) {
    memset(level, 0, sizeof(*level));
    level->sample_rate = sample_rate;
    level->octave_low_hz = octave_low_hz;
    level->octave_high_hz = octave_high_hz;
    sound_decimator_init(&level->decimator, decimator_taps);

    uint64_t voice_count = count_level_voices(octave_low_hz, octave_high_hz, min_hz, max_hz);
    if (voice_count == 0) {
        level->history_capacity = 1;
        return allocate_float_buffer(&level->history, level->history_capacity);
    }

    if (multiply_overflows_size(voice_count, sizeof(*level->voices))) {
        sound_error_set(error, "too many wavelet voices");
        return false;
    }

    level->voices = calloc((size_t)voice_count, sizeof(*level->voices));
    if (!level->voices) {
        sound_error_set(error, "could not allocate wavelet voices");
        return false;
    }

    uint64_t written_voice = 0;
    uint64_t max_half = 0;
    for (uint64_t i = 0; i < SOUND_WAVELET_VOICES_PER_OCTAVE; ++i) {
        double unit = ((double)i + 0.5) / (double)SOUND_WAVELET_VOICES_PER_OCTAVE;
        double center = octave_low_hz * pow(2.0, unit);

        if (center < min_hz || center > max_hz || center >= octave_high_hz) {
            continue;
        }

        double scale_samples =
            SOUND_WAVELET_MORLET_OMEGA0 * sample_rate / (2.0 * pi_value * center);
        uint64_t half = (uint64_t)ceil(morlet_support_sigmas * scale_samples);

        if (half < 4U) {
            half = 4U;
        }

        level->voices[written_voice].center_hz = center;
        level->voices[written_voice].scale_samples = scale_samples;
        level->voices[written_voice].half_support = half;

        if (half > max_half) {
            max_half = half;
        }

        ++written_voice;
    }

    level->voice_count = written_voice;
    level->max_half_support = max_half;
    level->block_size = max_half * 2U + 1U;
    level->history_capacity = level->block_size;

    uint64_t max_kernel_size = max_half * 2U + 1U;
    uint64_t convolution_size = level->block_size + max_kernel_size - 1U;
    level->fft_size = next_power_of_two(convolution_size, &level->log2_fft_size);

    level->fft_setup = vDSP_create_fftsetup((vDSP_Length)level->log2_fft_size, kFFTRadix2);
    if (!level->fft_setup) {
        sound_error_set(error, "vDSP_create_fftsetup failed for wavelet octave %" PRIu64, octave_index);
        return false;
    }

    if (!allocate_float_buffer(&level->history, level->history_capacity) ||
        !allocate_float_buffer(&level->block, level->block_size) ||
        !allocate_float_buffer(&level->fft_real, level->fft_size) ||
        !allocate_float_buffer(&level->fft_imag, level->fft_size) ||
        !allocate_float_buffer(&level->work_real, level->fft_size) ||
        !allocate_float_buffer(&level->work_imag, level->fft_size)) {
        sound_error_set(error, "could not allocate wavelet octave buffers");
        return false;
    }

    for (uint64_t i = 0; i < level->voice_count; ++i) {
        level->voices[i].extract_index = level->max_half_support + level->voices[i].half_support;
        if (!precompute_voice_kernel(level, &level->voices[i], error)) {
            return false;
        }
    }

    return true;
}

static void wavelet_level_write(SoundWaveletLevel *level, float sample) {
    if (level->history_capacity == 0) {
        return;
    }

    level->history[level->history_cursor] = sample;
    level->history_cursor = (level->history_cursor + 1U) % level->history_capacity;
    ++level->history_written;
}

static bool wavelet_level_read_latest(
    const SoundWaveletLevel *level,
    float *samples,
    uint64_t sample_count
) {
    if (!level || !samples || sample_count == 0 || level->history_written < sample_count) {
        return false;
    }

    uint64_t start =
        (level->history_cursor + level->history_capacity - sample_count) %
        level->history_capacity;
    for (uint64_t i = 0; i < sample_count; ++i) {
        samples[i] = level->history[(start + i) % level->history_capacity];
    }

    return true;
}

static SoundComplexValue convolve_current_block(
    SoundWaveletLevel *level,
    const float *kernel_real,
    const float *kernel_imag,
    uint64_t index
) {
    for (uint64_t i = 0; i < level->fft_size; ++i) {
        double xr = level->fft_real[i];
        double xi = level->fft_imag[i];
        double hr = kernel_real[i];
        double hi = kernel_imag[i];

        level->work_real[i] = (float)(xr * hr - xi * hi);
        level->work_imag[i] = (float)(xr * hi + xi * hr);
    }

    DSPSplitComplex work = {
        .realp = level->work_real,
        .imagp = level->work_imag,
    };

    vDSP_fft_zip(
        level->fft_setup,
        &work,
        1,
        (vDSP_Length)level->log2_fft_size,
        FFT_INVERSE
    );

    double scale = 1.0 / (double)level->fft_size;
    SoundComplexValue value = {
        .real = (double)level->work_real[index] * scale,
        .imag = (double)level->work_imag[index] * scale,
    };
    return value;
}

static double instantaneous_frequency_hz(
    SoundComplexValue coefficient,
    SoundComplexValue derivative
) {
    double magnitude_squared =
        coefficient.real * coefficient.real + coefficient.imag * coefficient.imag;

    if (magnitude_squared <= 1.0e-24) {
        return 0.0;
    }

    double imaginary_ratio =
        (derivative.imag * coefficient.real - derivative.real * coefficient.imag) /
        magnitude_squared;

    return imaginary_ratio / (2.0 * pi_value);
}

static double wavelet_level_compute(SoundWaveletLevel *level) {
    if (!level || level->voice_count == 0 ||
        !wavelet_level_read_latest(level, level->block, level->block_size)) {
        return 0.0;
    }

    memset(level->fft_real, 0, sizeof(float) * (size_t)level->fft_size);
    memset(level->fft_imag, 0, sizeof(float) * (size_t)level->fft_size);
    memcpy(level->fft_real, level->block, sizeof(float) * (size_t)level->block_size);

    DSPSplitComplex input = {
        .realp = level->fft_real,
        .imagp = level->fft_imag,
    };

    vDSP_fft_zip(
        level->fft_setup,
        &input,
        1,
        (vDSP_Length)level->log2_fft_size,
        FFT_FORWARD
    );

    double max_magnitude = 0.0;

    for (uint64_t i = 0; i < level->voice_count; ++i) {
        SoundWaveletVoice *voice = &level->voices[i];
        voice->coefficient = convolve_current_block(
            level,
            voice->kernel_real,
            voice->kernel_imag,
            voice->extract_index
        );
        voice->derivative = convolve_current_block(
            level,
            voice->derivative_real,
            voice->derivative_imag,
            voice->extract_index
        );

        voice->magnitude = hypot(voice->coefficient.real, voice->coefficient.imag);
        voice->instantaneous_hz = instantaneous_frequency_hz(
            voice->coefficient,
            voice->derivative
        );
        voice->ready = true;

        if (voice->magnitude > max_magnitude) {
            max_magnitude = voice->magnitude;
        }
    }

    return max_magnitude;
}

static void feed_level_sample(
    SoundWaveletAnalyzer *analyzer,
    uint64_t level_index,
    float sample
) {
    SoundWaveletLevel *level = &analyzer->levels[level_index];
    wavelet_level_write(level, sample);

    if (level_index + 1U >= analyzer->octave_count) {
        return;
    }

    float decimated = 0.0F;
    if (sound_decimator_push(&level->decimator, sample, &decimated)) {
        feed_level_sample(analyzer, level_index + 1U, decimated);
    }
}

static bool ensure_row_buffers(SoundWaveletAnalyzer *analyzer, uint64_t row_count) {
    if (row_count <= analyzer->row_capacity) {
        return true;
    }

    if (multiply_overflows_size(row_count, sizeof(float))) {
        return false;
    }

    float *raw_rows = realloc(analyzer->raw_rows, sizeof(float) * (size_t)row_count);
    if (!raw_rows) {
        return false;
    }

    analyzer->raw_rows = raw_rows;

    float *squeezed_rows =
        realloc(analyzer->squeezed_rows, sizeof(float) * (size_t)row_count);
    if (!squeezed_rows) {
        return false;
    }

    analyzer->squeezed_rows = squeezed_rows;
    analyzer->row_capacity = row_count;
    return true;
}

static void deposit_frequency(
    float *rows,
    uint64_t row_count,
    double min_hz,
    double max_hz,
    double frequency,
    double amplitude,
    double width_octaves
) {
    if (!rows || row_count == 0 || frequency < min_hz || frequency > max_hz || amplitude <= 0.0) {
        return;
    }

    double log_min = log2_double(min_hz);
    double log_max = log2_double(max_hz);
    double log_range = log_max - log_min;

    if (log_range <= 0.0) {
        return;
    }

    double position;
    if (row_count == 1) {
        position = 0.0;
    } else {
        position = (log_max - log2_double(frequency)) / log_range * (double)(row_count - 1U);
    }

    double octaves_per_row = row_count <= 1 ? log_range : log_range / (double)(row_count - 1U);
    double radius = fmax(0.75, width_octaves / octaves_per_row);
    int first = (int)floor(position - radius);
    int last = (int)ceil(position + radius);

    if (first < 0) {
        first = 0;
    }

    if (last >= (int)row_count) {
        last = (int)row_count - 1;
    }

    for (int row = first; row <= last; ++row) {
        double distance = fabs((double)row - position);
        double weight = 1.0 - distance / (radius + 1.0e-12);

        if (weight <= 0.0) {
            continue;
        }

        rows[row] += (float)(amplitude * weight);
    }
}

bool sound_wavelet_analyzer_create(
    double sample_rate,
    SoundWaveletAnalyzer **analyzer,
    SoundError *error
) {
    sound_error_clear(error);

    if (!analyzer) {
        sound_error_set(error, "missing wavelet analyzer output");
        return false;
    }

    *analyzer = NULL;

    if (sample_rate <= 0.0) {
        sound_error_set(error, "invalid wavelet analyzer sample rate");
        return false;
    }

    double min_hz = SOUND_WAVELET_MIN_HZ;
    double max_hz = fmin(SOUND_WAVELET_MAX_HZ, sample_rate * 0.45);

    if (max_hz <= min_hz * 2.0) {
        sound_error_set(error, "sample rate is too low for wavelet analysis");
        return false;
    }

    double octave_span = log2_double(max_hz / min_hz);
    uint64_t octave_count = (uint64_t)ceil(octave_span);

    if (octave_count == 0) {
        sound_error_set(error, "wavelet analyzer has no octaves");
        return false;
    }

    if (multiply_overflows_size(octave_count, sizeof(SoundWaveletLevel))) {
        sound_error_set(error, "too many wavelet octaves");
        return false;
    }

    SoundWaveletAnalyzer *created = calloc(1, sizeof(*created));
    if (!created) {
        sound_error_set(error, "could not allocate wavelet analyzer");
        return false;
    }

    created->levels = calloc((size_t)octave_count, sizeof(*created->levels));
    if (!created->levels) {
        sound_wavelet_analyzer_destroy(created);
        sound_error_set(error, "could not allocate wavelet pyramid");
        return false;
    }

    float decimator_taps[decimator_tap_count];
    design_decimator_taps(decimator_taps);

    created->sample_rate = sample_rate;
    created->min_hz = min_hz;
    created->max_hz = max_hz;
    created->octave_count = octave_count;
    created->synchrosqueezed = true;

    for (uint64_t octave = 0; octave < octave_count; ++octave) {
        double octave_high = max_hz / pow(2.0, (double)octave);
        double octave_low = octave_high * 0.5;
        double level_rate = sample_rate / pow(2.0, (double)octave);

        if (!wavelet_level_init(
                &created->levels[octave],
                octave,
                level_rate,
                octave_low,
                octave_high,
                min_hz,
                max_hz,
                decimator_taps,
                error
            )) {
            sound_wavelet_analyzer_destroy(created);
            return false;
        }

        created->total_voice_count += created->levels[octave].voice_count;
    }

    if (created->total_voice_count == 0) {
        sound_wavelet_analyzer_destroy(created);
        sound_error_set(error, "wavelet analyzer has no voices");
        return false;
    }

    *analyzer = created;
    return true;
}

void sound_wavelet_analyzer_destroy(SoundWaveletAnalyzer *analyzer) {
    if (!analyzer) {
        return;
    }

    if (analyzer->levels) {
        for (uint64_t i = 0; i < analyzer->octave_count; ++i) {
            wavelet_level_free(&analyzer->levels[i]);
        }
    }

    free(analyzer->levels);
    free(analyzer->raw_rows);
    free(analyzer->squeezed_rows);
    free(analyzer);
}

double sound_wavelet_analyzer_min_frequency(const SoundWaveletAnalyzer *analyzer) {
    return analyzer ? analyzer->min_hz : 0.0;
}

double sound_wavelet_analyzer_max_frequency(const SoundWaveletAnalyzer *analyzer) {
    return analyzer ? analyzer->max_hz : 0.0;
}

uint64_t sound_wavelet_analyzer_octave_count(const SoundWaveletAnalyzer *analyzer) {
    return analyzer ? analyzer->octave_count : 0;
}

uint64_t sound_wavelet_analyzer_voice_count(const SoundWaveletAnalyzer *analyzer) {
    return analyzer ? analyzer->total_voice_count : 0;
}

void sound_wavelet_analyzer_set_synchrosqueezed(
    SoundWaveletAnalyzer *analyzer,
    bool enabled
) {
    if (analyzer) {
        analyzer->synchrosqueezed = enabled;
    }
}

bool sound_wavelet_analyzer_synchrosqueezed(const SoundWaveletAnalyzer *analyzer) {
    return analyzer ? analyzer->synchrosqueezed : false;
}

bool sound_wavelet_analyzer_push(
    SoundWaveletAnalyzer *analyzer,
    const float *samples,
    uint64_t sample_count,
    SoundError *error
) {
    if (!analyzer || (!samples && sample_count > 0)) {
        sound_error_set(error, "invalid wavelet input");
        return false;
    }

    for (uint64_t i = 0; i < sample_count; ++i) {
        feed_level_sample(analyzer, 0, samples[i]);
    }

    return true;
}

bool sound_wavelet_analyzer_snapshot_db(
    SoundWaveletAnalyzer *analyzer,
    float *dbfs_rows,
    uint64_t row_count,
    SoundError *error
) {
    if (!analyzer || !dbfs_rows || row_count == 0) {
        sound_error_set(error, "invalid wavelet snapshot request");
        return false;
    }

    if (!ensure_row_buffers(analyzer, row_count)) {
        sound_error_set(error, "could not allocate wavelet display rows");
        return false;
    }

    memset(analyzer->raw_rows, 0, sizeof(float) * (size_t)row_count);
    memset(analyzer->squeezed_rows, 0, sizeof(float) * (size_t)row_count);

    double max_magnitude = 0.0;
    for (uint64_t i = 0; i < analyzer->octave_count; ++i) {
        double level_max = wavelet_level_compute(&analyzer->levels[i]);

        if (level_max > max_magnitude) {
            max_magnitude = level_max;
        }
    }

    double raw_width = 0.85 / (double)SOUND_WAVELET_VOICES_PER_OCTAVE;
    double squeezed_width = 0.24 / (double)SOUND_WAVELET_VOICES_PER_OCTAVE;
    double squeeze_threshold =
        fmax(squeeze_absolute_threshold, max_magnitude * squeeze_relative_threshold);

    for (uint64_t level_index = 0; level_index < analyzer->octave_count; ++level_index) {
        SoundWaveletLevel *level = &analyzer->levels[level_index];

        for (uint64_t voice_index = 0; voice_index < level->voice_count; ++voice_index) {
            const SoundWaveletVoice *voice = &level->voices[voice_index];

            if (!voice->ready) {
                continue;
            }

            deposit_frequency(
                analyzer->raw_rows,
                row_count,
                analyzer->min_hz,
                analyzer->max_hz,
                voice->center_hz,
                voice->magnitude,
                raw_width
            );

            if (voice->magnitude >= squeeze_threshold &&
                voice->instantaneous_hz >= analyzer->min_hz &&
                voice->instantaneous_hz <= analyzer->max_hz) {
                deposit_frequency(
                    analyzer->squeezed_rows,
                    row_count,
                    analyzer->min_hz,
                    analyzer->max_hz,
                    voice->instantaneous_hz,
                    voice->magnitude,
                    squeezed_width
                );
            }
        }
    }

    const float *source = analyzer->synchrosqueezed ?
        analyzer->squeezed_rows :
        analyzer->raw_rows;

    for (uint64_t row = 0; row < row_count; ++row) {
        double db = 20.0 * log10(fmax((double)source[row] * display_linear_gain, 1.0e-12));

        if (db < display_db_floor) {
            db = display_db_floor;
        }

        dbfs_rows[row] = (float)db;
    }

    return true;
}

double sound_wavelet_analyzer_frequency_for_row(
    const SoundWaveletAnalyzer *analyzer,
    uint64_t row,
    uint64_t row_count
) {
    if (!analyzer || row_count == 0) {
        return 0.0;
    }

    double unit = row_count == 1 ?
        0.5 :
        ((double)row + 0.5) / (double)row_count;
    double log_min = log2_double(analyzer->min_hz);
    double log_max = log2_double(analyzer->max_hz);
    double log_hz = log_max + (log_min - log_max) * clamp_double(unit, 0.0, 1.0);

    return pow(2.0, log_hz);
}
