#include "sounds/spectrum.h"

#include "sounds/analysis.h"

#include <Accelerate/Accelerate.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    spectrum_count = 6,
};

static const uint64_t spectrum_lengths[spectrum_count] = {
    512,
    1024,
    2048,
    4096,
    8192,
    16384,
};

static const double pi_value = 3.14159265358979323846264338327950288;
static const double log_two = 0.693147180559945309417232121458176568;
static const double target_cycles_per_window = 10.0;
static const double room_release_seconds = 0.22;
static const double display_db_floor = -120.0;

typedef struct SoundSpectrumWindow {
    uint64_t length;
    uint64_t half_length;
    float *window;
    double window_sum;
    double power_scale;
    vDSP_DFT_Setup setup;
} SoundSpectrumWindow;

typedef struct SpectrumBinMap {
    uint64_t lower;
    uint64_t upper;
    double unit;
} SpectrumBinMap;

typedef struct SpectrumRowMap {
    uint64_t first_spectrum;
    uint64_t second_spectrum;
    double first_weight;
    double second_weight;
    SpectrumBinMap bins[spectrum_count];
} SpectrumRowMap;

struct SoundSpectrumAnalyzer {
    double sample_rate;
    double room_alpha;
    double min_hz;
    double max_hz;
    uint64_t latency_samples;
    SoundSpectrumWindow spectra[spectrum_count];
    float *samples;
    float *input_even;
    float *input_odd;
    float *output_real;
    float *output_imag;
    float *power_rows;
    float *room_rows;
    SpectrumRowMap *row_maps;
    uint64_t row_capacity;
    uint64_t row_map_count;
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

static bool allocate_float_buffer(float **buffer, uint64_t count) {
    if (multiply_overflows_size(count, sizeof(float))) {
        return false;
    }

    *buffer = calloc((size_t)count, sizeof(float));
    return *buffer != NULL;
}

static void free_window(SoundSpectrumWindow *window) {
    if (!window) {
        return;
    }

    vDSP_DFT_DestroySetup(window->setup);
    free(window->window);
    memset(window, 0, sizeof(*window));
}

static bool init_window(
    SoundSpectrumWindow *window,
    uint64_t length,
    vDSP_DFT_Setup previous,
    SoundError *error
) {
    memset(window, 0, sizeof(*window));

    if ((length & 1U) != 0U || length == 0U) {
        sound_error_set(error, "invalid spectrum window length");
        return false;
    }

    window->length = length;
    window->half_length = length / 2U;
    window->setup = vDSP_DFT_zrop_CreateSetup(previous, (vDSP_Length)length, vDSP_DFT_FORWARD);

    if (!window->setup || !allocate_float_buffer(&window->window, length)) {
        sound_error_set(error, "could not allocate spectrum window");
        return false;
    }

    double sum = 0.0;
    for (uint64_t i = 0; i < length; ++i) {
        double unit = (double)i / (double)(length - 1U);
        double value = 0.5 - 0.5 * cos(2.0 * pi_value * unit);

        window->window[i] = (float)value;
        sum += value;
    }

    window->window_sum = fmax(sum, 1.0e-12);
    window->power_scale = 1.0 / (window->window_sum * window->window_sum);
    return true;
}

static bool ensure_row_capacity(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    if (row_count <= analyzer->row_capacity) {
        return true;
    }

    if (multiply_overflows_size(row_count, sizeof(float)) ||
        multiply_overflows_size(row_count, sizeof(SpectrumRowMap))) {
        return false;
    }

    float *power_rows = realloc(analyzer->power_rows, sizeof(float) * (size_t)row_count);
    if (!power_rows) {
        return false;
    }

    analyzer->power_rows = power_rows;

    float *room_rows = realloc(analyzer->room_rows, sizeof(float) * (size_t)row_count);
    if (!room_rows) {
        return false;
    }

    analyzer->room_rows = room_rows;

    SpectrumRowMap *row_maps = realloc(
        analyzer->row_maps,
        sizeof(SpectrumRowMap) * (size_t)row_count
    );

    if (!row_maps) {
        return false;
    }

    analyzer->row_maps = row_maps;
    analyzer->row_capacity = row_count;
    return true;
}

static double frequency_for_row(
    const SoundSpectrumAnalyzer *analyzer,
    uint64_t row,
    uint64_t row_count
) {
    double unit = row_count == 1U ?
        0.5 :
        ((double)row + 0.5) / (double)row_count;
    double log_min = log2_double(analyzer->min_hz);
    double log_max = log2_double(analyzer->max_hz);
    double log_hz = log_max + (log_min - log_max) * clamp_double(unit, 0.0, 1.0);

    return pow(2.0, log_hz);
}

static bool evaluate_window(
    SoundSpectrumAnalyzer *analyzer,
    const SoundRingBuffer *ring,
    uint64_t center_sample,
    uint64_t spectrum_index,
    SoundError *error
) {
    SoundSpectrumWindow *spectrum = &analyzer->spectra[spectrum_index];
    uint64_t end_sample = center_sample + spectrum->half_length;

    if (sound_ring_buffer_read_ending_at(
            ring,
            end_sample,
            analyzer->samples,
            spectrum->length
        ) != spectrum->length) {
        sound_error_set(error, "not enough centered audio for spectrum window");
        return false;
    }

    vDSP_Length half_length = (vDSP_Length)spectrum->half_length;
    vDSP_vmul(analyzer->samples, 2, spectrum->window, 2, analyzer->input_even, 1, half_length);
    vDSP_vmul(
        analyzer->samples + 1,
        2,
        spectrum->window + 1,
        2,
        analyzer->input_odd,
        1,
        half_length
    );

    vDSP_DFT_Execute(
        spectrum->setup,
        analyzer->input_even,
        analyzer->input_odd,
        analyzer->output_real,
        analyzer->output_imag
    );

    return true;
}

static double bin_power(
    const SoundSpectrumAnalyzer *analyzer,
    const SoundSpectrumWindow *spectrum,
    uint64_t bin
) {
    double real;
    double imag;

    if (bin == 0U) {
        real = analyzer->output_real[0];
        imag = 0.0;
    } else if (bin >= spectrum->half_length) {
        real = analyzer->output_imag[0];
        imag = 0.0;
    } else {
        real = analyzer->output_real[bin];
        imag = analyzer->output_imag[bin];
    }

    return (real * real + imag * imag) * spectrum->power_scale;
}

static double mapped_spectrum_power(
    const SoundSpectrumAnalyzer *analyzer,
    uint64_t spectrum_index,
    const SpectrumBinMap *map
) {
    const SoundSpectrumWindow *spectrum = &analyzer->spectra[spectrum_index];
    double low_power = bin_power(analyzer, spectrum, map->lower);
    double high_power = bin_power(analyzer, spectrum, map->upper);

    return low_power + (high_power - low_power) * map->unit;
}

static double desired_window_seconds(const SoundSpectrumAnalyzer *analyzer, double hz) {
    double shortest = (double)spectrum_lengths[0] / analyzer->sample_rate;
    double longest = (double)spectrum_lengths[spectrum_count - 1U] / analyzer->sample_rate;
    double seconds = target_cycles_per_window / fmax(hz, 1.0);

    return clamp_double(seconds, shortest, longest);
}

static void choose_spectra(
    const SoundSpectrumAnalyzer *analyzer,
    double hz,
    uint64_t *first,
    uint64_t *second,
    double *second_amount
) {
    double desired = desired_window_seconds(analyzer, hz);
    uint64_t chosen = 0;

    while (chosen + 1U < spectrum_count &&
        (double)analyzer->spectra[chosen + 1U].length / analyzer->sample_rate < desired) {
        ++chosen;
    }

    if (chosen + 1U >= spectrum_count) {
        *first = spectrum_count - 1U;
        *second = spectrum_count - 1U;
        *second_amount = 0.0;
        return;
    }

    double low = (double)analyzer->spectra[chosen].length / analyzer->sample_rate;
    double high = (double)analyzer->spectra[chosen + 1U].length / analyzer->sample_rate;
    double amount = (log(desired) - log(low)) / (log(high) - log(low));

    *first = chosen;
    *second = chosen + 1U;
    *second_amount = clamp_double(amount, 0.0, 1.0);
}

static SpectrumBinMap spectrum_bin_map(
    const SoundSpectrumAnalyzer *analyzer,
    uint64_t spectrum_index,
    double hz
) {
    const SoundSpectrumWindow *spectrum = &analyzer->spectra[spectrum_index];
    double bin_position = hz * (double)spectrum->length / analyzer->sample_rate;
    double clamped = clamp_double(bin_position, 0.0, (double)spectrum->half_length);
    uint64_t lower = (uint64_t)floor(clamped);
    uint64_t upper = lower + 1U;

    if (upper > spectrum->half_length) {
        upper = spectrum->half_length;
    }

    return (SpectrumBinMap){
        .lower = lower,
        .upper = upper,
        .unit = clamped - (double)lower,
    };
}

static void rebuild_row_maps(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    for (uint64_t row = 0; row < row_count; ++row) {
        double hz = frequency_for_row(analyzer, row, row_count);
        uint64_t first;
        uint64_t second;
        double second_amount;

        choose_spectra(analyzer, hz, &first, &second, &second_amount);

        SpectrumRowMap *map = &analyzer->row_maps[row];
        *map = (SpectrumRowMap){
            .first_spectrum = first,
            .second_spectrum = second,
            .first_weight = 1.0 - second_amount,
            .second_weight = first == second ? 0.0 : second_amount,
        };

        for (uint64_t i = 0; i < spectrum_count; ++i) {
            map->bins[i] = spectrum_bin_map(analyzer, i, hz);
        }

        analyzer->room_rows[row] = (float)display_db_floor;
    }

    analyzer->row_map_count = row_count;
}

static bool ensure_rows(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    if (!ensure_row_capacity(analyzer, row_count)) {
        return false;
    }

    if (analyzer->row_map_count != row_count) {
        rebuild_row_maps(analyzer, row_count);
    }

    return true;
}

bool sound_spectrum_analyzer_create(
    double sample_rate,
    double columns_per_second,
    SoundSpectrumAnalyzer **analyzer,
    SoundError *error
) {
    sound_error_clear(error);

    if (!analyzer || sample_rate <= 0.0 || columns_per_second <= 0.0) {
        sound_error_set(error, "invalid spectrum analyzer request");
        return false;
    }

    *analyzer = NULL;

    SoundSpectrumAnalyzer *created = calloc(1, sizeof(*created));
    if (!created) {
        sound_error_set(error, "could not allocate spectrum analyzer");
        return false;
    }

    created->sample_rate = sample_rate;
    created->room_alpha = 1.0 - exp(-(1.0 / columns_per_second) / room_release_seconds);
    created->min_hz = SOUND_WAVELET_MIN_HZ;
    created->max_hz = fmin(SOUND_WAVELET_MAX_HZ, sample_rate * 0.5);
    created->latency_samples = spectrum_lengths[spectrum_count - 1U] / 2U;

    vDSP_DFT_Setup previous = NULL;
    for (uint64_t i = 0; i < spectrum_count; ++i) {
        if (!init_window(&created->spectra[i], spectrum_lengths[i], previous, error)) {
            sound_spectrum_analyzer_destroy(created);
            return false;
        }

        previous = created->spectra[i].setup;
    }

    uint64_t maximum_length = spectrum_lengths[spectrum_count - 1U];
    uint64_t maximum_half = maximum_length / 2U;

    if (!allocate_float_buffer(&created->samples, maximum_length) ||
        !allocate_float_buffer(&created->input_even, maximum_half) ||
        !allocate_float_buffer(&created->input_odd, maximum_half) ||
        !allocate_float_buffer(&created->output_real, maximum_half) ||
        !allocate_float_buffer(&created->output_imag, maximum_half)) {
        sound_spectrum_analyzer_destroy(created);
        sound_error_set(error, "could not allocate spectrum buffers");
        return false;
    }

    *analyzer = created;
    return true;
}

void sound_spectrum_analyzer_destroy(SoundSpectrumAnalyzer *analyzer) {
    if (!analyzer) {
        return;
    }

    for (uint64_t i = 0; i < spectrum_count; ++i) {
        free_window(&analyzer->spectra[i]);
    }

    free(analyzer->samples);
    free(analyzer->input_even);
    free(analyzer->input_odd);
    free(analyzer->output_real);
    free(analyzer->output_imag);
    free(analyzer->power_rows);
    free(analyzer->room_rows);
    free(analyzer->row_maps);
    free(analyzer);
}

double sound_spectrum_analyzer_min_frequency(const SoundSpectrumAnalyzer *analyzer) {
    return analyzer ? analyzer->min_hz : 0.0;
}

double sound_spectrum_analyzer_max_frequency(const SoundSpectrumAnalyzer *analyzer) {
    return analyzer ? analyzer->max_hz : 0.0;
}

uint64_t sound_spectrum_analyzer_latency_samples(const SoundSpectrumAnalyzer *analyzer) {
    return analyzer ? analyzer->latency_samples : 0;
}

bool sound_spectrum_analyzer_column_db(
    SoundSpectrumAnalyzer *analyzer,
    const SoundRingBuffer *ring,
    uint64_t center_sample,
    SoundSpectrumMode mode,
    float *dbfs_rows,
    uint64_t row_count,
    SoundError *error
) {
    if (!analyzer || !ring || !dbfs_rows || row_count == 0U) {
        sound_error_set(error, "invalid spectrum column request");
        return false;
    }

    if (!ensure_rows(analyzer, row_count)) {
        sound_error_set(error, "could not allocate spectrum rows");
        return false;
    }

    memset(analyzer->power_rows, 0, sizeof(float) * (size_t)row_count);

    for (uint64_t i = 0; i < spectrum_count; ++i) {
        if (!evaluate_window(analyzer, ring, center_sample, i, error)) {
            return false;
        }

        for (uint64_t row = 0; row < row_count; ++row) {
            const SpectrumRowMap *map = &analyzer->row_maps[row];
            double weight = 0.0;

            if (i == map->first_spectrum) {
                weight = map->first_weight;
            } else if (i == map->second_spectrum) {
                weight = map->second_weight;
            }

            if (weight <= 0.0) {
                continue;
            }

            double power = mapped_spectrum_power(analyzer, i, &map->bins[i]);

            analyzer->power_rows[row] += (float)(power * weight);
        }
    }

    for (uint64_t row = 0; row < row_count; ++row) {
        double db = 10.0 * log10(fmax((double)analyzer->power_rows[row], 1.0e-12));

        if (mode == SOUND_SPECTRUM_ROOM_DECAY) {
            double previous = analyzer->room_rows[row];

            if (db >= previous) {
                analyzer->room_rows[row] = (float)db;
            } else {
                analyzer->room_rows[row] =
                    (float)(previous + analyzer->room_alpha * (db - previous));
            }

            db = analyzer->room_rows[row];
        }

        if (db < display_db_floor) {
            db = display_db_floor;
        }

        dbfs_rows[row] = (float)db;
    }

    return true;
}
