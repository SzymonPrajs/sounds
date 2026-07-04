#include "sounds/offline_spectrum.h"

#include <Accelerate/Accelerate.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const double pi_value = 3.14159265358979323846264338327950288;
static const double log_two = 0.693147180559945309417232121458176568;
static const double display_db_floor = -140.0;
static const double minimum_amplitude = 1.0e-7;

typedef struct RealDft {
    uint64_t length;
    uint64_t half_length;
    float *time;
    float *even;
    float *odd;
    float *real;
    float *imag;
    vDSP_DFT_Setup setup;
} RealDft;

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

static bool next_power_of_two(uint64_t value, uint64_t *result) {
    uint64_t power = 2;

    while (power < value) {
        if (power > UINT64_MAX / 2U) {
            return false;
        }

        power *= 2U;
    }

    *result = power;
    return true;
}

static void real_dft_destroy(RealDft *dft) {
    if (!dft) {
        return;
    }

    vDSP_DFT_DestroySetup(dft->setup);
    free(dft->time);
    free(dft->even);
    free(dft->odd);
    free(dft->real);
    free(dft->imag);
    memset(dft, 0, sizeof(*dft));
}

static bool real_dft_create(RealDft *dft, uint64_t length, SoundError *error) {
    memset(dft, 0, sizeof(*dft));

    if (length < 2U || (length & 1U) != 0U) {
        sound_error_set(error, "invalid offline spectrum DFT length");
        return false;
    }

    dft->length = length;
    dft->half_length = length / 2U;
    dft->setup = vDSP_DFT_zrop_CreateSetup(NULL, (vDSP_Length)length, vDSP_DFT_FORWARD);

    if (!dft->setup ||
        !allocate_float_buffer(&dft->time, length) ||
        !allocate_float_buffer(&dft->even, dft->half_length) ||
        !allocate_float_buffer(&dft->odd, dft->half_length) ||
        !allocate_float_buffer(&dft->real, dft->half_length) ||
        !allocate_float_buffer(&dft->imag, dft->half_length)) {
        real_dft_destroy(dft);
        sound_error_set(error, "could not allocate offline spectrum DFT buffers");
        return false;
    }

    return true;
}

static void real_dft_forward(RealDft *dft) {
    for (uint64_t i = 0; i < dft->half_length; ++i) {
        dft->even[i] = dft->time[i * 2U];
        dft->odd[i] = dft->time[i * 2U + 1U];
    }

    vDSP_DFT_Execute(dft->setup, dft->even, dft->odd, dft->real, dft->imag);
}

static double row_edge_frequency(
    double min_hz,
    double max_hz,
    uint64_t edge,
    uint64_t row_count
) {
    double unit = row_count == 0U ? 0.0 : (double)edge / (double)row_count;
    double log_min = log2_double(min_hz);
    double log_max = log2_double(max_hz);
    double log_hz = log_max + (log_min - log_max) * clamp_double(unit, 0.0, 1.0);

    return pow(2.0, log_hz);
}

static double bin_power(const RealDft *dft, uint64_t bin) {
    double real;
    double imag;

    if (bin == 0U) {
        real = dft->real[0];
        imag = 0.0;
    } else if (bin >= dft->half_length) {
        real = dft->imag[0];
        imag = 0.0;
    } else {
        real = dft->real[bin];
        imag = dft->imag[bin];
    }

    return real * real + imag * imag;
}

static float power_to_db(double power, double window_sum, uint64_t bin, uint64_t half_length) {
    double amplitude = sqrt(fmax(power, 0.0)) / fmax(window_sum, 1.0e-12);

    if (bin > 0U && bin < half_length) {
        amplitude *= 2.0;
    }

    return (float)fmax(display_db_floor, 20.0 * log10(fmax(amplitude, minimum_amplitude)));
}

double sound_offline_spectrum_frequency_for_row(
    double min_hz,
    double max_hz,
    uint64_t row,
    uint64_t row_count
) {
    if (min_hz <= 0.0 || max_hz <= min_hz || row_count == 0U) {
        return 0.0;
    }

    double unit = row_count == 1U ? 0.5 : ((double)row + 0.5) / (double)row_count;
    double log_min = log2_double(min_hz);
    double log_max = log2_double(max_hz);
    double log_hz = log_max + (log_min - log_max) * clamp_double(unit, 0.0, 1.0);

    return pow(2.0, log_hz);
}

bool sound_offline_spectrum_db(
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    double min_hz,
    double max_hz,
    float *dbfs_rows,
    uint64_t row_count,
    SoundError *error
) {
    sound_error_clear(error);

    if (!samples || !dbfs_rows || sample_count == 0U || row_count == 0U ||
        sample_rate <= 0.0 || min_hz <= 0.0 || max_hz <= min_hz) {
        sound_error_set(error, "invalid offline spectrum request");
        return false;
    }

    double nyquist = sample_rate * 0.5;
    max_hz = fmin(max_hz, nyquist);
    if (min_hz >= max_hz) {
        sound_error_set(error, "offline spectrum range is outside Nyquist");
        return false;
    }

    uint64_t fft_length = 0;
    if (!next_power_of_two(sample_count, &fft_length)) {
        sound_error_set(error, "offline spectrum clip is too large");
        return false;
    }

    RealDft dft;
    if (!real_dft_create(&dft, fft_length, error)) {
        return false;
    }

    double window_sum = 0.0;
    if (sample_count == 1U) {
        dft.time[0] = samples[0];
        window_sum = 1.0;
    } else {
        for (uint64_t i = 0; i < sample_count; ++i) {
            double unit = (double)i / (double)(sample_count - 1U);
            double window = 0.5 - 0.5 * cos(2.0 * pi_value * unit);

            dft.time[i] = (float)((double)samples[i] * window);
            window_sum += window;
        }
    }

    real_dft_forward(&dft);

    for (uint64_t row = 0; row < row_count; ++row) {
        double high_hz = row_edge_frequency(min_hz, max_hz, row, row_count);
        double low_hz = row_edge_frequency(min_hz, max_hz, row + 1U, row_count);
        uint64_t first_bin = (uint64_t)floor(low_hz * (double)fft_length / sample_rate);
        uint64_t last_bin = (uint64_t)ceil(high_hz * (double)fft_length / sample_rate);

        if (first_bin > dft.half_length) {
            first_bin = dft.half_length;
        }

        if (last_bin > dft.half_length) {
            last_bin = dft.half_length;
        }

        if (last_bin < first_bin) {
            last_bin = first_bin;
        }

        double best_power = 0.0;
        uint64_t best_bin = first_bin;
        for (uint64_t bin = first_bin; bin <= last_bin; ++bin) {
            double power = bin_power(&dft, bin);
            if (power > best_power) {
                best_power = power;
                best_bin = bin;
            }

            if (bin == dft.half_length) {
                break;
            }
        }

        dbfs_rows[row] = power_to_db(best_power, window_sum, best_bin, dft.half_length);
    }

    real_dft_destroy(&dft);
    return true;
}
