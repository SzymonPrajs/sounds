#include "sounds/analysis.h"

#include <Accelerate/Accelerate.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct SoundSpectrumAnalyzer {
    uint64_t fft_size;
    uint64_t bin_count;
    unsigned int log2n;
    float *window;
    float *windowed;
    float *realp;
    float *imagp;
    FFTSetup setup;
};

static unsigned int log2_u64(uint64_t value) {
    unsigned int result = 0;

    while (value > 1) {
        value >>= 1U;
        ++result;
    }

    return result;
}

bool sound_is_power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

uint64_t sound_previous_power_of_two(uint64_t value) {
    uint64_t power = 1;

    if (value == 0) {
        return 0;
    }

    while (power <= UINT64_MAX / 2U && power * 2U <= value) {
        power *= 2U;
    }

    return power;
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

bool sound_spectrum_analyzer_create(
    uint64_t fft_size,
    SoundSpectrumAnalyzer **analyzer,
    SoundError *error
) {
    sound_error_clear(error);

    if (!analyzer) {
        sound_error_set(error, "missing spectrum analyzer output");
        return false;
    }

    *analyzer = NULL;

    if (!sound_is_power_of_two(fft_size) || fft_size < 1024) {
        sound_error_set(error, "FFT size must be a power of two of at least 1024");
        return false;
    }

    if (fft_size > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "FFT size is too large");
        return false;
    }

    SoundSpectrumAnalyzer *created = calloc(1, sizeof(*created));
    if (!created) {
        sound_error_set(error, "could not allocate spectrum analyzer");
        return false;
    }

    created->fft_size = fft_size;
    created->bin_count = fft_size / 2U;
    created->log2n = log2_u64(fft_size);
    created->window = malloc(sizeof(float) * (size_t)fft_size);
    created->windowed = malloc(sizeof(float) * (size_t)fft_size);
    created->realp = malloc(sizeof(float) * (size_t)created->bin_count);
    created->imagp = malloc(sizeof(float) * (size_t)created->bin_count);

    if (!created->window || !created->windowed || !created->realp || !created->imagp) {
        sound_spectrum_analyzer_destroy(created);
        sound_error_set(error, "could not allocate FFT buffers");
        return false;
    }

    vDSP_hann_window(created->window, (vDSP_Length)fft_size, vDSP_HANN_DENORM);

    created->setup = vDSP_create_fftsetup((vDSP_Length)created->log2n, kFFTRadix2);
    if (!created->setup) {
        sound_spectrum_analyzer_destroy(created);
        sound_error_set(error, "vDSP_create_fftsetup failed");
        return false;
    }

    *analyzer = created;
    return true;
}

void sound_spectrum_analyzer_destroy(SoundSpectrumAnalyzer *analyzer) {
    if (!analyzer) {
        return;
    }

    if (analyzer->setup) {
        vDSP_destroy_fftsetup(analyzer->setup);
    }

    free(analyzer->window);
    free(analyzer->windowed);
    free(analyzer->realp);
    free(analyzer->imagp);
    free(analyzer);
}

uint64_t sound_spectrum_analyzer_fft_size(const SoundSpectrumAnalyzer *analyzer) {
    return analyzer ? analyzer->fft_size : 0;
}

uint64_t sound_spectrum_analyzer_bin_count(const SoundSpectrumAnalyzer *analyzer) {
    return analyzer ? analyzer->bin_count : 0;
}

bool sound_spectrum_analyzer_compute(
    SoundSpectrumAnalyzer *analyzer,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    float *dbfs,
    uint64_t dbfs_count,
    SoundError *error
) {
    if (!analyzer || !samples || !dbfs || sample_rate <= 0.0) {
        sound_error_set(error, "invalid spectrum-analysis input");
        return false;
    }

    if (sample_count < analyzer->fft_size) {
        sound_error_set(error, "not enough samples for FFT");
        return false;
    }

    if (dbfs_count < analyzer->bin_count) {
        sound_error_set(error, "spectrum output buffer is too small");
        return false;
    }

    const float *input = samples + sample_count - analyzer->fft_size;

    vDSP_vmul(
        input,
        1,
        analyzer->window,
        1,
        analyzer->windowed,
        1,
        (vDSP_Length)analyzer->fft_size
    );

    DSPSplitComplex split = {
        .realp = analyzer->realp,
        .imagp = analyzer->imagp,
    };

    vDSP_ctoz((const DSPComplex *)analyzer->windowed, 2, &split, 1, (vDSP_Length)analyzer->bin_count);
    vDSP_fft_zrip(analyzer->setup, &split, 1, (vDSP_Length)analyzer->log2n, FFT_FORWARD);

    const float coherent_gain = 0.5F;
    const float scale = 2.0F / ((float)analyzer->fft_size * coherent_gain);

    for (uint64_t bin = 0; bin < analyzer->bin_count; ++bin) {
        float real = split.realp[bin];
        float imag = bin == 0 ? 0.0F : split.imagp[bin];
        float magnitude = sqrtf(real * real + imag * imag) * scale;
        dbfs[bin] = 20.0F * log10f(fmaxf(magnitude, 1.0e-12F));
    }

    return true;
}

bool sound_write_raw_f32(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    SoundError *error
) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        sound_error_set(error, "could not open %s", path);
        return false;
    }

    size_t written = fwrite(samples, sizeof(float), (size_t)sample_count, file);
    bool closed = fclose(file) == 0;

    if (written != (size_t)sample_count || !closed) {
        sound_error_set(error, "could not write %s", path);
        return false;
    }

    return true;
}

bool sound_write_levels_csv(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    uint64_t block_size,
    SoundError *error
) {
    if (!samples || sample_count == 0 || sample_rate <= 0.0 || block_size == 0) {
        sound_error_set(error, "invalid level-analysis input");
        return false;
    }

    FILE *file = fopen(path, "w");
    if (!file) {
        sound_error_set(error, "could not open %s", path);
        return false;
    }

    (void)fprintf(file, "time_seconds,rms,peak\n");

    for (uint64_t offset = 0; offset + block_size <= sample_count; offset += block_size) {
        double rms = 0.0;
        double peak = 0.0;
        sound_compute_levels(samples + offset, block_size, &rms, &peak);

        double seconds = (double)offset / sample_rate;
        (void)fprintf(file, "%.9f,%.9f,%.9f\n", seconds, rms, peak);
    }

    if (fclose(file) != 0) {
        sound_error_set(error, "could not write %s", path);
        return false;
    }

    return true;
}

bool sound_write_spectrum_csv(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    uint64_t requested_fft_size,
    SoundError *error
) {
    if (!samples || sample_count == 0 || sample_rate <= 0.0) {
        sound_error_set(error, "invalid spectrum-analysis input");
        return false;
    }

    if (!sound_is_power_of_two(requested_fft_size)) {
        sound_error_set(error, "FFT size must be a power of two");
        return false;
    }

    uint64_t fft_size = requested_fft_size;
    if (sample_count < fft_size) {
        fft_size = sound_previous_power_of_two(sample_count);
    }

    if (fft_size < 1024) {
        sound_error_set(error, "not enough samples for FFT");
        return false;
    }

    uint64_t start = sample_count > fft_size ? (sample_count - fft_size) / 2U : 0;

    SoundSpectrumAnalyzer *analyzer = NULL;
    if (!sound_spectrum_analyzer_create(fft_size, &analyzer, error)) {
        return false;
    }

    uint64_t bin_count = sound_spectrum_analyzer_bin_count(analyzer);
    float *dbfs = malloc(sizeof(float) * (size_t)bin_count);
    if (!dbfs) {
        sound_spectrum_analyzer_destroy(analyzer);
        sound_error_set(error, "could not allocate spectrum output");
        return false;
    }

    if (!sound_spectrum_analyzer_compute(
            analyzer,
            samples + start,
            fft_size,
            sample_rate,
            dbfs,
            bin_count,
            error
        )) {
        sound_spectrum_analyzer_destroy(analyzer);
        free(dbfs);
        return false;
    }

    FILE *file = fopen(path, "w");
    if (!file) {
        sound_spectrum_analyzer_destroy(analyzer);
        free(dbfs);
        sound_error_set(error, "could not open %s", path);
        return false;
    }

    (void)fprintf(file, "hz,dbfs\n");

    for (uint64_t bin = 1; bin < bin_count; ++bin) {
        double hz = ((double)bin * sample_rate) / (double)fft_size;
        (void)fprintf(file, "%.9f,%.9f\n", hz, dbfs[bin]);
    }

    bool ok = fclose(file) == 0;

    sound_spectrum_analyzer_destroy(analyzer);
    free(dbfs);

    if (!ok) {
        sound_error_set(error, "could not write %s", path);
        return false;
    }

    return true;
}
