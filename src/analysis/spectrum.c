#include "sounds/spectrum.h"

#include "sounds/analysis.h"
#include "sounds/defer.h"

#include <Accelerate/Accelerate.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    spectrum_count = 6,
    multitaper_count = 3,
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
static const double transient_cycles_per_window = 10.0;
static const double s_transform_cycles_per_window = 5.0;
static const double focused_transient_cycles_limit = 18.0;
static const double focused_s_transform_cycles_limit = 9.0;
static const double display_db_floor = -120.0;
static const double minimum_linear_power = 1.0e-12;

typedef struct SoundSpectrumWindow {
    uint64_t length;
    uint64_t half_length;
    float *window;
    float *tapers[multitaper_count];
    double window_sum;
    double power_scale;
    double taper_power_scales[multitaper_count];
    vDSP_DFT_Setup setup;
} SoundSpectrumWindow;

typedef struct SpectrumBinMap {
    uint64_t lower;
    uint64_t upper;
    uint64_t first_bin;
    uint64_t last_bin;
    double unit;
    bool integrate;
} SpectrumBinMap;

typedef struct SpectrumRowMap {
    double hz;
    uint64_t first_spectrum;
    uint64_t second_spectrum;
    double first_weight;
    double second_weight;
    SpectrumBinMap bins[spectrum_count];
} SpectrumRowMap;

struct SoundSpectrumAnalyzer {
    double sample_rate;
    double full_min_hz;
    double full_max_hz;
    double min_hz;
    double max_hz;
    double transient_target_cycles;
    double s_transform_target_cycles;
    uint64_t latency_samples;
    SoundSpectrumWindow spectra[spectrum_count];
    float *samples;
    float *input_even;
    float *input_odd;
    float *output_real;
    float *output_imag;
    float *power_rows;
    float *work_rows;
    float *window_rows;
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

static void update_range_tuning(SoundSpectrumAnalyzer *analyzer) {
    double full_octaves = log2_double(analyzer->full_max_hz / analyzer->full_min_hz);
    double range_octaves = log2_double(analyzer->max_hz / analyzer->min_hz);
    double focus = range_octaves > 0.0 ?
        clamp_double(full_octaves / range_octaves, 1.0, 8.0) :
        1.0;

    analyzer->transient_target_cycles = clamp_double(
        transient_cycles_per_window * (1.0 + 0.16 * (focus - 1.0)),
        transient_cycles_per_window,
        focused_transient_cycles_limit
    );
    analyzer->s_transform_target_cycles = clamp_double(
        s_transform_cycles_per_window * (1.0 + 0.14 * (focus - 1.0)),
        s_transform_cycles_per_window,
        focused_s_transform_cycles_limit
    );
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
    for (uint64_t i = 0; i < multitaper_count; ++i) {
        free(window->tapers[i]);
    }
    memset(window, 0, sizeof(*window));
}

static bool init_window(
    SoundSpectrumWindow *window,
    uint64_t length,
    vDSP_DFT_Setup previous,
    SoundError *error
) {
    memset(window, 0, sizeof(*window));
    bool window_ready = false;
    defer {
        if (!window_ready) {
            free_window(window);
        }
    }

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

    for (uint64_t taper = 0; taper < multitaper_count; ++taper) {
        if (!allocate_float_buffer(&window->tapers[taper], length)) {
            sound_error_set(error, "could not allocate spectrum tapers");
            return false;
        }
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

    for (uint64_t taper = 0; taper < multitaper_count; ++taper) {
        uint64_t order = taper * 2U + 1U;
        double taper_sum = 0.0;

        for (uint64_t i = 0; i < length; ++i) {
            double angle =
                pi_value * (double)order * (double)(i + 1U) / (double)(length + 1U);
            double value = sin(angle);

            window->tapers[taper][i] = (float)value;
            taper_sum += value;
        }

        taper_sum = fmax(fabs(taper_sum), 1.0e-12);
        window->taper_power_scales[taper] = 1.0 / (taper_sum * taper_sum);
    }

    window_ready = true;
    return true;
}

static bool ensure_row_capacity(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    if (row_count <= analyzer->row_capacity) {
        return true;
    }

    if (multiply_overflows_size(row_count, sizeof(float)) ||
        row_count > UINT64_MAX / spectrum_count ||
        multiply_overflows_size(row_count * spectrum_count, sizeof(float)) ||
        multiply_overflows_size(row_count, sizeof(SpectrumRowMap))) {
        return false;
    }

    float *power_rows = realloc(analyzer->power_rows, sizeof(float) * (size_t)row_count);
    if (!power_rows) {
        return false;
    }

    analyzer->power_rows = power_rows;

    float *work_rows = realloc(analyzer->work_rows, sizeof(float) * (size_t)row_count);
    if (!work_rows) {
        return false;
    }

    analyzer->work_rows = work_rows;

    float *window_rows = realloc(
        analyzer->window_rows,
        sizeof(float) * (size_t)row_count * spectrum_count
    );

    if (!window_rows) {
        return false;
    }

    analyzer->window_rows = window_rows;

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

static double frequency_for_row_edge(
    const SoundSpectrumAnalyzer *analyzer,
    uint64_t edge,
    uint64_t row_count
) {
    double unit = row_count == 0U ? 0.0 : (double)edge / (double)row_count;
    double log_min = log2_double(analyzer->min_hz);
    double log_max = log2_double(analyzer->max_hz);
    double log_hz = log_max + (log_min - log_max) * clamp_double(unit, 0.0, 1.0);

    return pow(2.0, log_hz);
}

static bool evaluate_window_with_taper(
    SoundSpectrumAnalyzer *analyzer,
    const SoundRingBuffer *ring,
    uint64_t center_sample,
    uint64_t spectrum_index,
    const float *window,
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
    vDSP_vmul(analyzer->samples, 2, window, 2, analyzer->input_even, 1, half_length);
    vDSP_vmul(
        analyzer->samples + 1,
        2,
        window + 1,
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

static bool evaluate_window(
    SoundSpectrumAnalyzer *analyzer,
    const SoundRingBuffer *ring,
    uint64_t center_sample,
    uint64_t spectrum_index,
    SoundError *error
) {
    return evaluate_window_with_taper(
        analyzer,
        ring,
        center_sample,
        spectrum_index,
        analyzer->spectra[spectrum_index].window,
        error
    );
}

static double bin_power(
    const SoundSpectrumAnalyzer *analyzer,
    const SoundSpectrumWindow *spectrum,
    uint64_t bin,
    double power_scale
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

    return (real * real + imag * imag) * power_scale;
}

static double mapped_spectrum_power(
    const SoundSpectrumAnalyzer *analyzer,
    uint64_t spectrum_index,
    const SpectrumBinMap *map,
    double power_scale
) {
    const SoundSpectrumWindow *spectrum = &analyzer->spectra[spectrum_index];

    if (map->integrate) {
        double total = 0.0;
        uint64_t count = map->last_bin - map->first_bin + 1U;

        for (uint64_t bin = map->first_bin; bin <= map->last_bin; ++bin) {
            total += bin_power(analyzer, spectrum, bin, power_scale);
        }

        return total / (double)count;
    }

    double low_power = bin_power(analyzer, spectrum, map->lower, power_scale);
    double high_power = bin_power(analyzer, spectrum, map->upper, power_scale);

    return low_power + (high_power - low_power) * map->unit;
}

static double desired_window_seconds(
    const SoundSpectrumAnalyzer *analyzer,
    double hz,
    double target_cycles
) {
    double shortest = (double)spectrum_lengths[0] / analyzer->sample_rate;
    double longest = (double)spectrum_lengths[spectrum_count - 1U] / analyzer->sample_rate;
    double seconds = target_cycles / fmax(hz, 1.0);

    return clamp_double(seconds, shortest, longest);
}

static void choose_spectra(
    const SoundSpectrumAnalyzer *analyzer,
    double hz,
    double target_cycles,
    uint64_t *first,
    uint64_t *second,
    double *second_amount
) {
    double desired = desired_window_seconds(analyzer, hz, target_cycles);
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
    double low_hz,
    double hz,
    double high_hz
) {
    const SoundSpectrumWindow *spectrum = &analyzer->spectra[spectrum_index];
    double bin_position = hz * (double)spectrum->length / analyzer->sample_rate;
    double clamped = clamp_double(bin_position, 0.0, (double)spectrum->half_length);
    uint64_t lower = (uint64_t)floor(clamped);
    uint64_t upper = lower + 1U;
    double low_position = low_hz * (double)spectrum->length / analyzer->sample_rate;
    double high_position = high_hz * (double)spectrum->length / analyzer->sample_rate;
    int64_t first = (int64_t)ceil(fmin(low_position, high_position));
    int64_t last = (int64_t)floor(fmax(low_position, high_position));

    if (upper > spectrum->half_length) {
        upper = spectrum->half_length;
    }

    if (first < 0) {
        first = 0;
    }

    if (last < 0) {
        last = 0;
    }

    if (first > (int64_t)spectrum->half_length) {
        first = (int64_t)spectrum->half_length;
    }

    if (last > (int64_t)spectrum->half_length) {
        last = (int64_t)spectrum->half_length;
    }

    return (SpectrumBinMap){
        .lower = lower,
        .upper = upper,
        .first_bin = (uint64_t)first,
        .last_bin = (uint64_t)last,
        .unit = clamped - (double)lower,
        .integrate = first <= last,
    };
}

static void rebuild_row_maps(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    for (uint64_t row = 0; row < row_count; ++row) {
        double hz = frequency_for_row(analyzer, row, row_count);
        double high_hz = frequency_for_row_edge(analyzer, row, row_count);
        double low_hz = frequency_for_row_edge(analyzer, row + 1U, row_count);
        uint64_t first;
        uint64_t second;
        double second_amount;

        choose_spectra(
            analyzer,
            hz,
            analyzer->transient_target_cycles,
            &first,
            &second,
            &second_amount
        );

        SpectrumRowMap *map = &analyzer->row_maps[row];
        *map = (SpectrumRowMap){
            .hz = hz,
            .first_spectrum = first,
            .second_spectrum = second,
            .first_weight = 1.0 - second_amount,
            .second_weight = first == second ? 0.0 : second_amount,
        };

        for (uint64_t i = 0; i < spectrum_count; ++i) {
            map->bins[i] = spectrum_bin_map(analyzer, i, low_hz, hz, high_hz);
        }
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
    defer {
        sound_spectrum_analyzer_destroy(created);
    }

    created->sample_rate = sample_rate;
    (void)columns_per_second;
    created->full_min_hz = SOUND_WAVELET_MIN_HZ;
    created->full_max_hz = fmin(SOUND_WAVELET_MAX_HZ, sample_rate * 0.5);
    created->min_hz = created->full_min_hz;
    created->max_hz = created->full_max_hz;
    update_range_tuning(created);
    created->latency_samples = spectrum_lengths[spectrum_count - 1U] / 2U;

    vDSP_DFT_Setup previous = NULL;
    for (uint64_t i = 0; i < spectrum_count; ++i) {
        if (!init_window(&created->spectra[i], spectrum_lengths[i], previous, error)) {
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
        sound_error_set(error, "could not allocate spectrum buffers");
        return false;
    }

    *analyzer = created;
    created = NULL;
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
    free(analyzer->work_rows);
    free(analyzer->window_rows);
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

bool sound_spectrum_analyzer_set_frequency_range(
    SoundSpectrumAnalyzer *analyzer,
    double min_hz,
    double max_hz,
    SoundError *error
) {
    if (!analyzer) {
        sound_error_set(error, "missing spectrum analyzer");
        return false;
    }

    if (min_hz < analyzer->full_min_hz) {
        min_hz = analyzer->full_min_hz;
    }

    if (max_hz > analyzer->full_max_hz) {
        max_hz = analyzer->full_max_hz;
    }

    if (min_hz <= 0.0 || max_hz <= min_hz) {
        sound_error_set(error, "invalid spectrum frequency range");
        return false;
    }

    if (analyzer->min_hz != min_hz || analyzer->max_hz != max_hz) {
        analyzer->min_hz = min_hz;
        analyzer->max_hz = max_hz;
        update_range_tuning(analyzer);
        analyzer->row_map_count = 0;
    }

    return true;
}

static float *window_rows_for(
    SoundSpectrumAnalyzer *analyzer,
    uint64_t spectrum_index,
    uint64_t row_count
) {
    return analyzer->window_rows + spectrum_index * row_count;
}

static bool fill_hann_window_rows(
    SoundSpectrumAnalyzer *analyzer,
    const SoundRingBuffer *ring,
    uint64_t center_sample,
    uint64_t row_count,
    SoundError *error
) {
    for (uint64_t i = 0; i < spectrum_count; ++i) {
        if (!evaluate_window(analyzer, ring, center_sample, i, error)) {
            return false;
        }

        float *rows = window_rows_for(analyzer, i, row_count);
        double scale = analyzer->spectra[i].power_scale;

        for (uint64_t row = 0; row < row_count; ++row) {
            const SpectrumRowMap *map = &analyzer->row_maps[row];
            rows[row] = (float)mapped_spectrum_power(analyzer, i, &map->bins[i], scale);
        }
    }

    return true;
}

static bool fill_multitaper_window_rows(
    SoundSpectrumAnalyzer *analyzer,
    const SoundRingBuffer *ring,
    uint64_t center_sample,
    uint64_t row_count,
    SoundError *error
) {
    for (uint64_t i = 0; i < spectrum_count; ++i) {
        float *rows = window_rows_for(analyzer, i, row_count);
        memset(rows, 0, sizeof(float) * (size_t)row_count);

        for (uint64_t taper = 0; taper < multitaper_count; ++taper) {
            if (!evaluate_window_with_taper(
                    analyzer,
                    ring,
                    center_sample,
                    i,
                    analyzer->spectra[i].tapers[taper],
                    error
                )) {
                return false;
            }

            double scale = analyzer->spectra[i].taper_power_scales[taper];

            for (uint64_t row = 0; row < row_count; ++row) {
                const SpectrumRowMap *map = &analyzer->row_maps[row];
                double power = mapped_spectrum_power(analyzer, i, &map->bins[i], scale);

                rows[row] += (float)(power / (double)multitaper_count);
            }
        }
    }

    return true;
}

static void build_weighted_rows(
    SoundSpectrumAnalyzer *analyzer,
    uint64_t row_count,
    double target_cycles,
    float *rows
) {
    for (uint64_t row = 0; row < row_count; ++row) {
        const SpectrumRowMap *map = &analyzer->row_maps[row];
        uint64_t first;
        uint64_t second;
        double second_amount;

        choose_spectra(
            analyzer,
            map->hz,
            target_cycles,
            &first,
            &second,
            &second_amount
        );

        double first_power = window_rows_for(analyzer, first, row_count)[row];
        double power = first_power;

        if (second != first) {
            double second_power = window_rows_for(analyzer, second, row_count)[row];

            power = first_power * (1.0 - second_amount) + second_power * second_amount;
        }

        rows[row] = (float)power;
    }
}

static void deposit_power(
    float *rows,
    uint64_t row_count,
    double center,
    double power,
    double sigma_rows
) {
    if (power <= 0.0 || row_count == 0U) {
        return;
    }

    int first = (int)floor(center - 3.0 * sigma_rows);
    int last = (int)ceil(center + 3.0 * sigma_rows);

    if (first < 0) {
        first = 0;
    }

    if (last >= (int)row_count) {
        last = (int)row_count - 1;
    }

    double weight_sum = 0.0;
    for (int row = first; row <= last; ++row) {
        double distance = ((double)row - center) / sigma_rows;
        weight_sum += exp(-0.5 * distance * distance);
    }

    if (weight_sum <= 0.0) {
        return;
    }

    for (int row = first; row <= last; ++row) {
        double distance = ((double)row - center) / sigma_rows;
        double weight = exp(-0.5 * distance * distance) / weight_sum;

        rows[row] += (float)(power * weight);
    }
}

static uint64_t nearest_peak(
    const float *rows,
    uint64_t row_count,
    uint64_t row,
    uint64_t radius
) {
    uint64_t first = row > radius ? row - radius : 0;
    uint64_t last = row + radius;

    if (last >= row_count) {
        last = row_count - 1U;
    }

    uint64_t best = row;
    float best_power = rows[row];

    for (uint64_t i = first; i <= last; ++i) {
        if (rows[i] > best_power) {
            best_power = rows[i];
            best = i;
        }
    }

    return best;
}

static double neighborhood_average(
    const float *rows,
    uint64_t row_count,
    uint64_t row,
    uint64_t radius
) {
    uint64_t first = row > radius ? row - radius : 0;
    uint64_t last = row + radius;

    if (last >= row_count) {
        last = row_count - 1U;
    }

    double sum = 0.0;
    for (uint64_t i = first; i <= last; ++i) {
        sum += rows[i];
    }

    return sum / (double)(last - first + 1U);
}

static double parabolic_peak_offset(const float *rows, uint64_t row_count, uint64_t peak) {
    if (peak == 0U || peak + 1U >= row_count) {
        return 0.0;
    }

    double left = log(fmax((double)rows[peak - 1U], minimum_linear_power));
    double center = log(fmax((double)rows[peak], minimum_linear_power));
    double right = log(fmax((double)rows[peak + 1U], minimum_linear_power));
    double denominator = left - 2.0 * center + right;

    if (fabs(denominator) < 1.0e-12) {
        return 0.0;
    }

    return clamp_double(0.5 * (left - right) / denominator, -0.5, 0.5);
}

static void build_reassigned_rows(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    build_weighted_rows(
        analyzer,
        row_count,
        analyzer->transient_target_cycles,
        analyzer->work_rows
    );
    memset(analyzer->power_rows, 0, sizeof(float) * (size_t)row_count);

    for (uint64_t row = 0; row < row_count; ++row) {
        double power = analyzer->work_rows[row];
        uint64_t peak = nearest_peak(analyzer->work_rows, row_count, row, 6);
        double local = neighborhood_average(analyzer->work_rows, row_count, peak, 8);
        double contrast = analyzer->work_rows[peak] / fmax(local, minimum_linear_power);

        if (contrast > 1.35) {
            double target = (double)peak +
                parabolic_peak_offset(analyzer->work_rows, row_count, peak);

            deposit_power(analyzer->power_rows, row_count, target, power * 0.85, 1.0);
            deposit_power(analyzer->power_rows, row_count, (double)row, power * 0.15, 1.6);
        } else {
            deposit_power(analyzer->power_rows, row_count, (double)row, power, 1.3);
        }
    }
}

static void build_squeezed_rows(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    build_weighted_rows(
        analyzer,
        row_count,
        analyzer->transient_target_cycles,
        analyzer->work_rows
    );
    memset(analyzer->power_rows, 0, sizeof(float) * (size_t)row_count);

    for (uint64_t row = 0; row < row_count; ++row) {
        double power = analyzer->work_rows[row];
        uint64_t peak = nearest_peak(analyzer->work_rows, row_count, row, 10);
        double local = neighborhood_average(analyzer->work_rows, row_count, peak, 12);
        double contrast = analyzer->work_rows[peak] / fmax(local, minimum_linear_power);

        if (contrast > 1.18) {
            double target = (double)peak +
                parabolic_peak_offset(analyzer->work_rows, row_count, peak);

            deposit_power(analyzer->power_rows, row_count, target, power * 0.95, 0.65);
            deposit_power(analyzer->power_rows, row_count, (double)row, power * 0.05, 1.8);
        } else {
            deposit_power(analyzer->power_rows, row_count, (double)row, power, 1.1);
        }
    }
}

static void build_superlet_rows(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    for (uint64_t row = 0; row < row_count; ++row) {
        const SpectrumRowMap *map = &analyzer->row_maps[row];
        uint64_t first;
        uint64_t last;
        double ignored;

        choose_spectra(
            analyzer,
            map->hz,
            analyzer->transient_target_cycles,
            &first,
            &last,
            &ignored
        );

        (void)first;

        double log_sum = 0.0;
        uint64_t count = last + 1U;

        for (uint64_t i = 0; i <= last; ++i) {
            double power = window_rows_for(analyzer, i, row_count)[row];
            log_sum += log(fmax(power, minimum_linear_power));
        }

        analyzer->power_rows[row] = (float)exp(log_sum / (double)count);
    }
}

static bool is_local_maximum(const float *rows, uint64_t row_count, uint64_t row) {
    float center = rows[row];
    float before = row == 0U ? -1.0F : rows[row - 1U];
    float after = row + 1U >= row_count ? -1.0F : rows[row + 1U];

    return center >= before && center >= after;
}

static void build_sparse_rows(SoundSpectrumAnalyzer *analyzer, uint64_t row_count) {
    build_weighted_rows(
        analyzer,
        row_count,
        analyzer->transient_target_cycles,
        analyzer->work_rows
    );

    float maximum = 0.0F;
    for (uint64_t row = 0; row < row_count; ++row) {
        if (analyzer->work_rows[row] > maximum) {
            maximum = analyzer->work_rows[row];
        }
    }

    float threshold = maximum * 1.0e-4F;
    memset(analyzer->power_rows, 0, sizeof(float) * (size_t)row_count);

    for (uint64_t row = 0; row < row_count; ++row) {
        double power = analyzer->work_rows[row];

        if (is_local_maximum(analyzer->work_rows, row_count, row) && power >= threshold) {
            double target = (double)row +
                parabolic_peak_offset(analyzer->work_rows, row_count, row);

            deposit_power(analyzer->power_rows, row_count, target, power, 0.75);
        } else {
            deposit_power(analyzer->power_rows, row_count, (double)row, power * 0.025, 1.4);
        }
    }
}

static void convert_power_rows_to_db(
    const SoundSpectrumAnalyzer *analyzer,
    float *dbfs_rows,
    uint64_t row_count
) {
    for (uint64_t row = 0; row < row_count; ++row) {
        double db = 10.0 * log10(fmax((double)analyzer->power_rows[row], minimum_linear_power));

        if (db < display_db_floor) {
            db = display_db_floor;
        }

        dbfs_rows[row] = (float)db;
    }
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

    if (mode == SOUND_SPECTRUM_MULTITAPER) {
        if (!fill_multitaper_window_rows(analyzer, ring, center_sample, row_count, error)) {
            return false;
        }
    } else if (!fill_hann_window_rows(analyzer, ring, center_sample, row_count, error)) {
        return false;
    }

    switch (mode) {
        case SOUND_SPECTRUM_TRANSIENT:
        case SOUND_SPECTRUM_MULTITAPER:
            build_weighted_rows(
                analyzer,
                row_count,
                analyzer->transient_target_cycles,
                analyzer->power_rows
            );
            break;
        case SOUND_SPECTRUM_REASSIGNED:
            build_reassigned_rows(analyzer, row_count);
            break;
        case SOUND_SPECTRUM_SQUEEZED:
            build_squeezed_rows(analyzer, row_count);
            break;
        case SOUND_SPECTRUM_SUPERLET:
            build_superlet_rows(analyzer, row_count);
            break;
        case SOUND_SPECTRUM_S_TRANSFORM:
            build_weighted_rows(
                analyzer,
                row_count,
                analyzer->s_transform_target_cycles,
                analyzer->power_rows
            );
            break;
        case SOUND_SPECTRUM_SPARSE:
            build_sparse_rows(analyzer, row_count);
            break;
    }

    convert_power_rows_to_db(analyzer, dbfs_rows, row_count);
    return true;
}
