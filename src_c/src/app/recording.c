#include "sounds/recording.h"

#include "sounds/defer.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char recording_directory[] = "recordings";
static const char recording_file_prefix[] = "sounds-";
static const char recording_file_suffix[] = "32f";
static const char recording_file_extension[] = ".wav";

enum {
    maximum_wav_channels = 32,
    maximum_wav_sample_rate = 512000,
};

typedef struct WavInfo {
    uint16_t format;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t block_align;
    uint32_t sample_rate;
    uint64_t data_offset;
    uint64_t data_bytes;
} WavInfo;

static bool wav_format_supported(const WavInfo *info);

const char *sound_recording_directory(void) {
    return recording_directory;
}

static bool multiply_overflows_size(uint64_t count, size_t element_size) {
    return count > (uint64_t)(SIZE_MAX / element_size);
}

static bool ensure_recording_directory(SoundError *error) {
    if (mkdir(recording_directory, 0o755) == 0 || errno == EEXIST) {
        return true;
    }

    sound_error_set(error, "could not create %s", recording_directory);
    return false;
}

static bool write_bytes(FILE *file, const void *bytes, size_t size) {
    return fwrite(bytes, 1, size, file) == size;
}

static bool write_u16_le(FILE *file, uint16_t value) {
    uint8_t bytes[2] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
    };

    return write_bytes(file, bytes, sizeof(bytes));
}

static bool write_u32_le(FILE *file, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
        (uint8_t)((value >> 16) & 0xFFU),
        (uint8_t)((value >> 24) & 0xFFU),
    };

    return write_bytes(file, bytes, sizeof(bytes));
}

static bool read_bytes(FILE *file, void *bytes, size_t size) {
    return fread(bytes, 1, size, file) == size;
}

static bool read_u16_le(FILE *file, uint16_t *value) {
    uint8_t bytes[2];

    if (!read_bytes(file, bytes, sizeof(bytes))) {
        return false;
    }

    *value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return true;
}

static bool read_u24_le(FILE *file, int32_t *value) {
    uint8_t bytes[3];

    if (!read_bytes(file, bytes, sizeof(bytes))) {
        return false;
    }

    int32_t sample =
        (int32_t)bytes[0] | ((int32_t)bytes[1] << 8) | ((int32_t)bytes[2] << 16);
    if ((sample & 0x800000) != 0) {
        sample |= ~0xFFFFFF;
    }

    *value = sample;
    return true;
}

static bool read_u32_le(FILE *file, uint32_t *value) {
    uint8_t bytes[4];

    if (!read_bytes(file, bytes, sizeof(bytes))) {
        return false;
    }

    *value = (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
    return true;
}

static bool read_i32_le(FILE *file, int32_t *value) {
    uint32_t bits = 0;

    if (!read_u32_le(file, &bits)) {
        return false;
    }

    *value = (int32_t)bits;
    return true;
}

static bool write_float32_samples(
    FILE *file,
    const float *samples,
    uint64_t sample_count
) {
    for (uint64_t i = 0; i < sample_count; ++i) {
        union {
            float sample;
            uint32_t bits;
        } value = {.sample = samples[i]};

        if (!write_u32_le(file, value.bits)) {
            return false;
        }
    }

    return true;
}

static bool wav_payload_size(
    uint64_t sample_count,
    uint32_t *data_bytes,
    SoundError *error
) {
    if (sample_count > UINT32_MAX / 4U) {
        sound_error_set(error, "recording is too large for WAV");
        return false;
    }

    uint64_t bytes = sample_count * 4U;
    if (bytes > UINT32_MAX) {
        sound_error_set(error, "recording is too large for WAV");
        return false;
    }

    *data_bytes = (uint32_t)bytes;
    return true;
}

static bool write_wav_samples(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    SoundError *error
) {
    uint32_t bits_per_sample = 32U;
    uint32_t data_bytes = 0;

    if (!isfinite(sample_rate) ||
        sample_rate <= 0.0 ||
        sample_rate > (double)maximum_wav_sample_rate ||
        !wav_payload_size(sample_count, &data_bytes, error)) {
        if (error && error->message[0] == '\0') {
            sound_error_set(error, "invalid WAV sample rate");
        }
        return false;
    }

    uint16_t audio_format = 3U;
    uint16_t channel_count = 1U;
    uint16_t block_align = (uint16_t)(channel_count * (bits_per_sample / 8U));
    uint32_t rate = (uint32_t)lrint(sample_rate);
    uint64_t byte_rate_64 = (uint64_t)rate * (uint64_t)block_align;
    uint32_t fact_size = 12U;
    uint64_t riff_size_64 =
        4U + (8U + 16U) + fact_size + (8U + (uint64_t)data_bytes);

    if (byte_rate_64 > UINT32_MAX || riff_size_64 > UINT32_MAX) {
        sound_error_set(error, "recording is too large for WAV");
        return false;
    }

    bool ok = false;
    {
        FILE *file = fopen(path, "wb");
        if (!file) {
            sound_error_set(error, "could not open %s", path);
            return false;
        }

        defer {
            ok = fclose(file) == 0 && ok;
        }

        ok =
            write_bytes(file, "RIFF", 4) &&
            write_u32_le(file, (uint32_t)riff_size_64) &&
            write_bytes(file, "WAVE", 4) &&
            write_bytes(file, "fmt ", 4) &&
            write_u32_le(file, 16U) &&
            write_u16_le(file, audio_format) &&
            write_u16_le(file, channel_count) &&
            write_u32_le(file, rate) &&
            write_u32_le(file, (uint32_t)byte_rate_64) &&
            write_u16_le(file, block_align) &&
            write_u16_le(file, (uint16_t)bits_per_sample);

        ok = ok &&
            write_bytes(file, "fact", 4) &&
            write_u32_le(file, 4U) &&
            write_u32_le(file, (uint32_t)sample_count) &&
            write_bytes(file, "data", 4) &&
            write_u32_le(file, data_bytes) &&
            write_float32_samples(file, samples, sample_count);
    }

    if (!ok) {
        sound_error_set(error, "could not write %s", path);
        return false;
    }

    return true;
}

static bool seek_forward(FILE *file, uint64_t bytes) {
    while (bytes > 0) {
        long step = bytes > (uint64_t)LONG_MAX ? LONG_MAX : (long)bytes;

        if (fseek(file, step, SEEK_CUR) != 0) {
            return false;
        }

        bytes -= (uint64_t)step;
    }

    return true;
}

static bool seek_to_offset(FILE *file, uint64_t offset) {
    if (offset > (uint64_t)LONG_MAX) {
        return false;
    }

    return fseek(file, (long)offset, SEEK_SET) == 0;
}

static bool file_length(FILE *file, uint64_t *length) {
    long current = ftell(file);
    if (current < 0 || fseek(file, 0, SEEK_END) != 0) {
        return false;
    }

    long end = ftell(file);
    if (end < 0 || fseek(file, current, SEEK_SET) != 0) {
        return false;
    }

    *length = (uint64_t)end;
    return true;
}

static bool wav_info_valid(const WavInfo *info) {
    if (info->sample_rate == 0U ||
        info->sample_rate > maximum_wav_sample_rate ||
        info->channels == 0U ||
        info->channels > maximum_wav_channels ||
        info->block_align == 0U ||
        info->data_bytes < info->block_align ||
        (info->data_bytes % info->block_align) != 0U ||
        !wav_format_supported(info)) {
        return false;
    }

    uint32_t bytes_per_channel = (uint32_t)((info->bits_per_sample + 7U) / 8U);
    if (bytes_per_channel == 0U ||
        info->channels > UINT16_MAX / bytes_per_channel) {
        return false;
    }

    uint32_t expected_align = bytes_per_channel * (uint32_t)info->channels;
    return expected_align > 0U && info->block_align >= expected_align;
}

static bool parse_wav_info(FILE *file, WavInfo *info) {
    char id[4];
    uint32_t riff_size = 0;
    uint64_t file_size = 0;

    memset(info, 0, sizeof(*info));

    if (!file_length(file, &file_size) || file_size < 12U) {
        return false;
    }

    if (!read_bytes(file, id, sizeof(id)) ||
        memcmp(id, "RIFF", 4) != 0 ||
        !read_u32_le(file, &riff_size) ||
        !read_bytes(file, id, sizeof(id)) ||
        memcmp(id, "WAVE", 4) != 0) {
        return false;
    }
    (void)riff_size;

    while (true) {
        long chunk_header = ftell(file);
        if (chunk_header < 0) {
            return false;
        }

        uint64_t header_offset = (uint64_t)chunk_header;
        if (header_offset == file_size) {
            break;
        }
        if (header_offset > file_size || file_size - header_offset < 8U) {
            return false;
        }

        uint32_t size = 0;
        if (!read_bytes(file, id, sizeof(id)) || !read_u32_le(file, &size)) {
            return false;
        }

        long chunk_start = ftell(file);
        if (chunk_start < 0) {
            return false;
        }

        uint64_t body_start = (uint64_t)chunk_start;
        if (body_start > UINT64_MAX - (uint64_t)size) {
            return false;
        }

        uint64_t body_end = body_start + (uint64_t)size;
        uint64_t padded_end = body_end;
        if ((size & 1U) != 0U) {
            if (padded_end == UINT64_MAX) {
                return false;
            }
            ++padded_end;
        }

        if (padded_end > file_size) {
            return false;
        }

        if (memcmp(id, "fmt ", 4) == 0) {
            uint32_t byte_rate = 0;

            if (size < 16U ||
                !read_u16_le(file, &info->format) ||
                !read_u16_le(file, &info->channels) ||
                !read_u32_le(file, &info->sample_rate) ||
                !read_u32_le(file, &byte_rate) ||
                !read_u16_le(file, &info->block_align) ||
                !read_u16_le(file, &info->bits_per_sample)) {
                return false;
            }
            (void)byte_rate;
        } else if (memcmp(id, "data", 4) == 0) {
            info->data_offset = body_start;
            info->data_bytes = size;
        }

        if (!seek_to_offset(file, padded_end)) {
            return false;
        }
    }

    return wav_info_valid(info);
}

static bool parse_wav_path(const char *path, WavInfo *info, SoundError *error) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        sound_error_set(error, "could not open %s", path);
        return false;
    }

    bool ok = parse_wav_info(file, info);
    bool closed = fclose(file) == 0;

    if (!ok || !closed) {
        sound_error_set(error, "could not read WAV metadata from %s", path);
        return false;
    }

    return true;
}

static bool wav_format_supported(const WavInfo *info) {
    if (info->format == 3U && info->bits_per_sample == 32U) {
        return true;
    }

    return info->format == 1U &&
        (info->bits_per_sample == 16U ||
            info->bits_per_sample == 24U ||
            info->bits_per_sample == 32U);
}

static bool read_sample_value(FILE *file, const WavInfo *info, float *sample) {
    if (info->format == 3U && info->bits_per_sample == 32U) {
        union {
            uint32_t bits;
            float value;
        } data = {0};

        if (!read_u32_le(file, &data.bits)) {
            return false;
        }

        *sample = data.value;
        return true;
    }

    if (info->format == 1U && info->bits_per_sample == 16U) {
        uint16_t bits = 0;
        if (!read_u16_le(file, &bits)) {
            return false;
        }

        *sample = (float)((double)(int16_t)bits / 32768.0);
        return true;
    }

    if (info->format == 1U && info->bits_per_sample == 24U) {
        int32_t value = 0;
        if (!read_u24_le(file, &value)) {
            return false;
        }

        *sample = (float)((double)value / 8388608.0);
        return true;
    }

    if (info->format == 1U && info->bits_per_sample == 32U) {
        int32_t value = 0;
        if (!read_i32_le(file, &value)) {
            return false;
        }

        *sample = (float)((double)value / 2147483648.0);
        return true;
    }

    return false;
}

static bool read_wav_samples(
    FILE *file,
    const WavInfo *info,
    float *samples,
    uint64_t sample_count
) {
    uint32_t bytes_per_channel = (uint32_t)((info->bits_per_sample + 7U) / 8U);
    uint32_t expected_align = bytes_per_channel * (uint32_t)info->channels;

    if (!wav_format_supported(info) ||
        bytes_per_channel == 0 ||
        info->block_align < expected_align ||
        sample_count > info->data_bytes / info->block_align ||
        !seek_to_offset(file, info->data_offset)) {
        return false;
    }

    for (uint64_t frame = 0; frame < sample_count; ++frame) {
        double mixed = 0.0;

        for (uint16_t channel = 0; channel < info->channels; ++channel) {
            float value = 0.0F;

            if (!read_sample_value(file, info, &value)) {
                return false;
            }

            mixed += (double)value;
        }

        uint32_t padding = (uint32_t)info->block_align - expected_align;
        if (padding > 0 && !seek_forward(file, padding)) {
            return false;
        }

        samples[frame] = (float)(mixed / (double)info->channels);
    }

    return true;
}

static bool build_recording_path(
    char *path,
    size_t path_size,
    SoundError *error
) {
    time_t now = time(NULL);
    struct tm local_time;
    char timestamp[32];

    if (now == (time_t)-1 || !localtime_r(&now, &local_time)) {
        sound_error_set(error, "could not read current time");
        return false;
    }

    if (strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time) == 0) {
        sound_error_set(error, "could not format recording timestamp");
        return false;
    }

    int written = snprintf(
        path,
        path_size,
        "%s/%s%s-%s%s",
        recording_directory,
        recording_file_prefix,
        timestamp,
        recording_file_suffix,
        recording_file_extension
    );

    if (written <= 0 || (size_t)written >= path_size) {
        sound_error_set(error, "recording path is too long");
        return false;
    }

    return true;
}

bool sound_recording_save_latest(
    const SoundRingBuffer *ring,
    double sample_rate,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
) {
    return sound_recording_save_recent(
        ring,
        sound_ring_buffer_capacity(ring),
        sample_rate,
        sample_count,
        path,
        path_size,
        error
    );
}

bool sound_recording_save_recent(
    const SoundRingBuffer *ring,
    uint64_t requested_samples,
    double sample_rate,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
) {
    *sample_count = 0;
    path[0] = '\0';

    uint64_t capacity = sound_ring_buffer_capacity(ring);
    if (capacity == 0 || requested_samples == 0) {
        return true;
    }

    uint64_t wanted = requested_samples < capacity ? requested_samples : capacity;
    if (multiply_overflows_size(wanted, sizeof(float))) {
        sound_error_set(error, "raw recording buffer is too large");
        return false;
    }

    float *samples = malloc(sizeof(float) * (size_t)wanted);
    if (!samples) {
        sound_error_set(error, "could not allocate raw recording buffer");
        return false;
    }
    defer {
        free(samples);
    }

    uint64_t count = sound_ring_buffer_read_latest(ring, samples, wanted);
    return sound_recording_save_samples(
        samples,
        count,
        sample_rate,
        sample_count,
        path,
        path_size,
        error
    );
}

bool sound_recording_save_samples(
    const float *samples,
    uint64_t requested_samples,
    double sample_rate,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
) {
    if (!sample_count || !path || path_size == 0) {
        sound_error_set(error, "invalid recording output");
        return false;
    }

    *sample_count = 0;
    path[0] = '\0';

    if (!samples || requested_samples == 0) {
        return true;
    }

    bool ok = ensure_recording_directory(error) &&
        build_recording_path(path, path_size, error) &&
        write_wav_samples(path, samples, requested_samples, sample_rate, error);

    *sample_count = ok ? requested_samples : 0;
    return ok;
}

bool sound_recording_read_info(
    const char *path,
    SoundRecordingInfo *info,
    SoundError *error
) {
    if (!path || !info) {
        sound_error_set(error, "invalid recording metadata request");
        return false;
    }

    WavInfo wav;
    if (!parse_wav_path(path, &wav, error)) {
        return false;
    }

    uint64_t frames = wav.data_bytes / wav.block_align;
    *info = (SoundRecordingInfo){
        .sample_rate = (double)wav.sample_rate,
        .duration_seconds = (double)frames / (double)wav.sample_rate,
        .sample_count = frames,
    };
    return true;
}

bool sound_recording_load_samples(
    const char *path,
    float **samples,
    uint64_t *sample_count,
    double *sample_rate,
    SoundError *error
) {
    if (!path || !samples || !sample_count || !sample_rate) {
        sound_error_set(error, "invalid recording load request");
        return false;
    }

    *samples = NULL;
    *sample_count = 0;
    *sample_rate = 0.0;

    WavInfo wav;
    if (!parse_wav_path(path, &wav, error)) {
        return false;
    }

    if (!wav_format_supported(&wav)) {
        sound_error_set(error, "unsupported WAV sample format in %s", path);
        return false;
    }

    uint64_t frames = wav.data_bytes / wav.block_align;
    if (frames == 0 || multiply_overflows_size(frames, sizeof(float))) {
        sound_error_set(error, "invalid WAV sample count in %s", path);
        return false;
    }

    float *loaded = malloc(sizeof(float) * (size_t)frames);
    if (!loaded) {
        sound_error_set(error, "could not allocate WAV samples");
        return false;
    }
    defer {
        free(loaded);
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        sound_error_set(error, "could not open %s", path);
        return false;
    }

    bool ok = read_wav_samples(file, &wav, loaded, frames);
    bool closed = fclose(file) == 0;
    if (!ok || !closed) {
        sound_error_set(error, "could not load WAV samples from %s", path);
        return false;
    }

    *samples = loaded;
    *sample_count = frames;
    *sample_rate = (double)wav.sample_rate;
    loaded = NULL;
    return true;
}
