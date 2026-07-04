#ifndef SOUNDS_CAPTURE_H
#define SOUNDS_CAPTURE_H

#include "sounds/error.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SoundInputStream SoundInputStream;

typedef void (*SoundInputCallback)(
    const float *samples,
    uint32_t sample_count,
    void *user_data
);

typedef struct SoundInputFormat {
    double sample_rate;
    uint32_t format_id;
    uint32_t format_flags;
    uint32_t bits_per_channel;
    uint32_t channels_per_frame;
    uint32_t bytes_per_frame;
} SoundInputFormat;

typedef struct SoundRecording {
    float *samples;
    uint64_t sample_count;
    SoundInputFormat format;
} SoundRecording;

typedef struct SoundCaptureOptions {
    double seconds;
} SoundCaptureOptions;

typedef struct SoundInputStreamOptions {
    SoundInputCallback callback;
    void *user_data;
} SoundInputStreamOptions;

bool sound_default_input_format(SoundInputFormat *format, SoundError *error);

bool sound_capture_default_input(
    const SoundCaptureOptions *options,
    SoundRecording *recording,
    SoundError *error
);

void sound_recording_free(SoundRecording *recording);

bool sound_input_stream_open(
    const SoundInputStreamOptions *options,
    SoundInputStream **stream,
    SoundInputFormat *format,
    SoundError *error
);

bool sound_input_stream_start(SoundInputStream *stream, SoundError *error);
bool sound_input_stream_stop(SoundInputStream *stream, SoundError *error);
void sound_input_stream_close(SoundInputStream *stream);

#endif
