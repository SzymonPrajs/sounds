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

/*
 * Streaming evaluation: each octave level re-evaluates all of its voices
 * after `hop` new samples at that level's own rate, so high octaves are
 * sampled thousands of times per second while deep octaves follow their
 * own slower timescale. Per-voice power is smoothed over a window matched
 * to the wavelet's length, which is what turns single-shot speckle into a
 * stable time-integrated spectrogram column.
 */
enum {
    level_hop_divisor = 4,
    level_minimum_hop = 2,
};

static const double pi_value = 3.14159265358979323846264338327950288;
static const double log_two = 0.693147180559945309417232121458176568;
static const double decimator_cutoff_cycles = 0.225;
static const double morlet_support_sigmas = 8.0;

static const double power_smoothing_floor_seconds = 0.060;
static const double power_smoothing_scales = 6.0;
static const double frequency_smoothing_floor_seconds = 0.040;
static const double frequency_smoothing_scales = 4.0;

/*
 * Display shaping. Raw CWT ridges are deposited at the voice's center with
 * a width matched to the voice spacing; synchrosqueezed deposits land at
 * the smoothed instantaneous frequency with a width driven by how stable
 * that estimate is, so coherent tones sharpen while noise stays a smooth
 * continuum instead of scattering into speckle.
 */
static const double raw_ridge_width_octaves =
    0.55 / (double)SOUND_WAVELET_VOICES_PER_OCTAVE;
static const double squeezed_width_gain = 0.8;
static const double squeezed_width_min_octaves = 0.018;
static const double squeezed_width_max_octaves = 0.22;
static const double minimum_deposit_sigma_rows = 0.9;
static const double display_db_floor = -120.0;
static const double if_clamp_octaves = 0.75;
static const double if_power_gate = 1.0e-13;
static const double initial_if_jitter = 0.25;
static const double initial_power_jitter = 0.5;

/*
 * A genuine ridge keeps a steady envelope; a voice caught between two
 * close tones beats deeply, and reassigning its energy would paint a
 * phantom ridge at the mean frequency. Two coherence measures scale the
 * synchrosqueezed deposit down by 1 + gain * jitter^2: the relative
 * envelope modulation between evaluations, and the instantaneous
 * bandwidth (the real part of dW/W, whose imaginary part is the
 * instantaneous frequency) relative to the voice's own bandwidth. Both
 * sit near zero on coherent tones and blow up on interference.
 */
static const double am_incoherence_gain = 8.0;
static const double bandwidth_incoherence_gain = 4.0;
static const double bandwidth_ratio_clamp = 3.0;

/*
 * Snapshot columns sum overlapping voices, so each display mode carries
 * its own normalization, calibrated so a full-scale sine at a voice
 * center reads close to 0 dBFS.
 */
static const double raw_deposit_normalization = 0.5;
static const double squeezed_deposit_normalization = 0.1;

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
    double center_log2;
    double scale_samples;
    uint64_t half_support;
    uint64_t kernel_size;
    uint64_t block_offset;
    float *kernel_real;
    float *kernel_imag;
    float *derivative_real;
    float *derivative_imag;
    double gain; /* magnitude response to a full-scale sine at center_hz */
    double power_alpha;
    double frequency_alpha;
    double ema_power;
    double ema_if_log2;
    double if_jitter;
    double last_if_log2;
    double power_jitter; /* relative envelope modulation between evals */
    double previous_power;
    double bandwidth_jitter; /* instantaneous bandwidth / voice bandwidth */
    bool has_output;
} SoundWaveletVoice;

typedef struct SoundWaveletLevel {
    double sample_rate;
    double octave_low_hz;
    double octave_high_hz;
    uint64_t voice_count;
    uint64_t max_half_support;
    uint64_t block_size;
    uint64_t hop;
    uint64_t samples_since_eval;
    SoundWaveletVoice *voices;
    float *history;
    float *block;
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
    float *power_rows;
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

    free(level->voices);
    free(level->history);
    free(level->block);
    memset(level, 0, sizeof(*level));
}

static bool allocate_float_buffer(float **buffer, uint64_t count) {
    if (multiply_overflows_size(count, sizeof(float))) {
        return false;
    }

    *buffer = calloc((size_t)count, sizeof(float));
    return *buffer != NULL;
}

/*
 * Build the analytic Morlet correlation kernel and its time-derivative
 * kernel for one voice, and calibrate the voice's magnitude response so
 * that a full-scale sine at the center frequency reads as power 1.0
 * (0 dBFS) regardless of scale or level rate.
 */
static bool precompute_voice_kernel(
    SoundWaveletLevel *level,
    SoundWaveletVoice *voice,
    SoundError *error
) {
    uint64_t kernel_size = voice->half_support * 2U + 1U;

    voice->kernel_size = kernel_size;

    if (!allocate_float_buffer(&voice->kernel_real, kernel_size) ||
        !allocate_float_buffer(&voice->kernel_imag, kernel_size) ||
        !allocate_float_buffer(&voice->derivative_real, kernel_size) ||
        !allocate_float_buffer(&voice->derivative_imag, kernel_size)) {
        sound_error_set(error, "could not allocate Morlet wavelet kernels");
        return false;
    }

    double morlet_norm = pow(pi_value, -0.25) / sqrt(voice->scale_samples);
    double inverse_scale_seconds = level->sample_rate / voice->scale_samples;
    double theta = 2.0 * pi_value * voice->center_hz / level->sample_rate;
    double positive_real = 0.0;
    double positive_imag = 0.0;
    double negative_real = 0.0;
    double negative_imag = 0.0;

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

        double m = (double)i - (double)voice->half_support;
        double c = cos(theta * m);
        double s = sin(theta * m);

        positive_real += real * c - imag * s;
        positive_imag += real * s + imag * c;
        negative_real += real * c + imag * s;
        negative_imag += imag * c - real * s;
    }

    double positive = hypot(positive_real, positive_imag);
    double negative = hypot(negative_real, negative_imag);

    voice->gain = 0.5 * fmax(positive, negative);
    if (voice->gain < 1.0e-9) {
        voice->gain = 1.0e-9;
    }

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

static double smoothing_alpha(double hop_seconds, double floor_seconds, double timescale_seconds) {
    double time_constant = fmax(floor_seconds, timescale_seconds);
    double alpha = 1.0 - exp(-hop_seconds / time_constant);

    return clamp_double(alpha, 1.0e-4, 1.0);
}

static bool wavelet_level_init(
    SoundWaveletLevel *level,
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
        level->voices[written_voice].center_log2 = log2_double(center);
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
    level->hop = max_half / level_hop_divisor;

    if (level->hop < level_minimum_hop) {
        level->hop = level_minimum_hop;
    }

    if (!allocate_float_buffer(&level->history, level->history_capacity) ||
        !allocate_float_buffer(&level->block, level->block_size)) {
        sound_error_set(error, "could not allocate wavelet octave buffers");
        return false;
    }

    double hop_seconds = (double)level->hop / sample_rate;

    for (uint64_t i = 0; i < level->voice_count; ++i) {
        SoundWaveletVoice *voice = &level->voices[i];
        double scale_seconds = voice->scale_samples / sample_rate;

        voice->block_offset = max_half - voice->half_support;
        voice->power_alpha = smoothing_alpha(
            hop_seconds,
            power_smoothing_floor_seconds,
            power_smoothing_scales * scale_seconds
        );
        voice->frequency_alpha = smoothing_alpha(
            hop_seconds,
            frequency_smoothing_floor_seconds,
            frequency_smoothing_scales * scale_seconds
        );

        if (!precompute_voice_kernel(level, voice, error)) {
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

/*
 * Evaluate every voice of one level at the center of its history block via
 * direct correlation (four short real dot products per voice), then fold
 * the result into the voice's smoothed power, instantaneous frequency, and
 * frequency-stability estimates.
 */
static void wavelet_level_evaluate(SoundWaveletLevel *level) {
    if (!wavelet_level_read_latest(level, level->block, level->block_size)) {
        return;
    }

    for (uint64_t i = 0; i < level->voice_count; ++i) {
        SoundWaveletVoice *voice = &level->voices[i];
        const float *segment = level->block + voice->block_offset;
        float coefficient_real = 0.0F;
        float coefficient_imag = 0.0F;
        float derivative_real = 0.0F;
        float derivative_imag = 0.0F;
        vDSP_Length length = (vDSP_Length)voice->kernel_size;

        vDSP_dotpr(segment, 1, voice->kernel_real, 1, &coefficient_real, length);
        vDSP_dotpr(segment, 1, voice->kernel_imag, 1, &coefficient_imag, length);
        vDSP_dotpr(segment, 1, voice->derivative_real, 1, &derivative_real, length);
        vDSP_dotpr(segment, 1, voice->derivative_imag, 1, &derivative_imag, length);

        SoundComplexValue coefficient = {
            .real = coefficient_real,
            .imag = coefficient_imag,
        };
        SoundComplexValue derivative = {
            .real = derivative_real,
            .imag = derivative_imag,
        };

        double power =
            (coefficient.real * coefficient.real + coefficient.imag * coefficient.imag) /
            (voice->gain * voice->gain);

        if (!voice->has_output) {
            voice->ema_power = power;
            voice->ema_if_log2 = voice->center_log2;
            voice->last_if_log2 = voice->center_log2;
            voice->if_jitter = initial_if_jitter;
            voice->power_jitter = initial_power_jitter;
            voice->previous_power = power;
            voice->bandwidth_jitter = initial_power_jitter;
            voice->has_output = true;
        } else {
            voice->ema_power += voice->power_alpha * (power - voice->ema_power);
        }

        if (power <= if_power_gate) {
            continue;
        }

        double modulation =
            fabs(power - voice->previous_power) /
            (power + voice->previous_power + 1.0e-15);

        voice->power_jitter += voice->frequency_alpha * (modulation - voice->power_jitter);
        voice->previous_power = power;

        double magnitude_squared =
            coefficient.real * coefficient.real + coefficient.imag * coefficient.imag;
        double instantaneous_bandwidth_hz = fabs(
            (derivative.real * coefficient.real + derivative.imag * coefficient.imag) /
            (magnitude_squared * 2.0 * pi_value)
        );
        double voice_bandwidth_hz = voice->center_hz / SOUND_WAVELET_MORLET_OMEGA0;
        double bandwidth_ratio = clamp_double(
            instantaneous_bandwidth_hz / voice_bandwidth_hz,
            0.0,
            bandwidth_ratio_clamp
        );

        voice->bandwidth_jitter +=
            voice->frequency_alpha * (bandwidth_ratio - voice->bandwidth_jitter);

        double frequency = instantaneous_frequency_hz(coefficient, derivative);

        if (frequency <= 0.0) {
            frequency = voice->center_hz;
        }

        double if_log2 = clamp_double(
            log2_double(frequency),
            voice->center_log2 - if_clamp_octaves,
            voice->center_log2 + if_clamp_octaves
        );
        double delta = fabs(if_log2 - voice->last_if_log2);

        voice->if_jitter += voice->frequency_alpha * (delta - voice->if_jitter);
        voice->ema_if_log2 += voice->frequency_alpha * (if_log2 - voice->ema_if_log2);
        voice->last_if_log2 = if_log2;
    }
}

static void wavelet_level_feed(SoundWaveletLevel *level, float sample) {
    wavelet_level_write(level, sample);

    if (level->voice_count == 0) {
        return;
    }

    ++level->samples_since_eval;

    if (level->history_written >= level->history_capacity &&
        level->samples_since_eval >= level->hop) {
        level->samples_since_eval = 0;
        wavelet_level_evaluate(level);
    }
}

static void feed_level_sample(
    SoundWaveletAnalyzer *analyzer,
    uint64_t level_index,
    float sample
) {
    SoundWaveletLevel *level = &analyzer->levels[level_index];
    wavelet_level_feed(level, sample);

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

    float *power_rows = realloc(analyzer->power_rows, sizeof(float) * (size_t)row_count);
    if (!power_rows) {
        return false;
    }

    analyzer->power_rows = power_rows;
    analyzer->row_capacity = row_count;
    return true;
}

/*
 * Paint one voice's smoothed power onto the log-frequency display rows as
 * a truncated Gaussian, never narrower than about one pixel so ridges stay
 * continuous at any window size.
 */
static void deposit_gaussian(
    float *rows,
    uint64_t row_count,
    double min_hz,
    double max_hz,
    double frequency,
    double power,
    double width_octaves
) {
    if (!rows || row_count == 0 || power <= 0.0) {
        return;
    }

    double log_min = log2_double(min_hz);
    double log_max = log2_double(max_hz);
    double log_range = log_max - log_min;

    if (log_range <= 0.0) {
        return;
    }

    frequency = clamp_double(frequency, min_hz, max_hz);

    double position;
    double octaves_per_row;
    if (row_count == 1) {
        position = 0.0;
        octaves_per_row = log_range;
    } else {
        position = (log_max - log2_double(frequency)) / log_range * (double)(row_count - 1U);
        octaves_per_row = log_range / (double)(row_count - 1U);
    }

    double sigma_rows = fmax(minimum_deposit_sigma_rows, width_octaves / octaves_per_row);
    int first = (int)floor(position - 3.0 * sigma_rows);
    int last = (int)ceil(position + 3.0 * sigma_rows);

    if (first < 0) {
        first = 0;
    }

    if (last >= (int)row_count) {
        last = (int)row_count - 1;
    }

    for (int row = first; row <= last; ++row) {
        double distance = ((double)row - position) / sigma_rows;
        rows[row] += (float)(power * exp(-0.5 * distance * distance));
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
    free(analyzer->power_rows);
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

    memset(analyzer->power_rows, 0, sizeof(float) * (size_t)row_count);

    for (uint64_t level_index = 0; level_index < analyzer->octave_count; ++level_index) {
        SoundWaveletLevel *level = &analyzer->levels[level_index];

        for (uint64_t voice_index = 0; voice_index < level->voice_count; ++voice_index) {
            const SoundWaveletVoice *voice = &level->voices[voice_index];

            if (!voice->has_output || voice->ema_power <= 0.0) {
                continue;
            }

            double frequency;
            double width_octaves;
            double power = voice->ema_power;

            if (analyzer->synchrosqueezed) {
                frequency = pow(2.0, voice->ema_if_log2);
                width_octaves = clamp_double(
                    squeezed_width_gain * voice->if_jitter,
                    squeezed_width_min_octaves,
                    squeezed_width_max_octaves
                );
                power /= 1.0 +
                    am_incoherence_gain * voice->power_jitter * voice->power_jitter +
                    bandwidth_incoherence_gain *
                        voice->bandwidth_jitter * voice->bandwidth_jitter;
            } else {
                frequency = voice->center_hz;
                width_octaves = raw_ridge_width_octaves;
            }

            deposit_gaussian(
                analyzer->power_rows,
                row_count,
                analyzer->min_hz,
                analyzer->max_hz,
                frequency,
                power,
                width_octaves
            );
        }
    }

    double normalization = analyzer->synchrosqueezed ?
        squeezed_deposit_normalization :
        raw_deposit_normalization;

    for (uint64_t row = 0; row < row_count; ++row) {
        double power = (double)analyzer->power_rows[row] * normalization;
        double db = 10.0 * log10(fmax(power, 1.0e-12));

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
