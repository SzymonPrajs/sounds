#include "sounds/capture.h"

#include <CoreAudio/CoreAudio.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct CaptureState {
    float *samples;
    uint64_t capacity;
    atomic_uint_fast64_t write_index;
} CaptureState;

struct SoundInputStream {
    AudioDeviceID device;
    AudioDeviceIOProcID callback_id;
    SoundInputCallback callback;
    void *user_data;
    float *scratch;
    uint32_t scratch_capacity;
    bool started;
};

static bool osstatus_ok(OSStatus status, const char *operation, SoundError *error) {
    if (status == noErr) {
        return true;
    }

    sound_error_set(error, "%s failed with OSStatus %d", operation, (int)status);
    return false;
}

static void sleep_for_callback(void) {
    const struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = 10000000L,
    };

    (void)nanosleep(&interval, NULL);
}

static double monotonic_seconds(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }

    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static uint64_t capture_wait_limit(double seconds) {
    return (uint64_t)(seconds * 100.0) + 1U;
}

static void copy_format(
    const AudioStreamBasicDescription *source,
    SoundInputFormat *destination
) {
    destination->sample_rate = source->mSampleRate;
    destination->format_id = source->mFormatID;
    destination->format_flags = source->mFormatFlags;
    destination->bits_per_channel = source->mBitsPerChannel;
    destination->channels_per_frame = source->mChannelsPerFrame;
    destination->bytes_per_frame = source->mBytesPerFrame;
}

static bool default_input_device(AudioDeviceID *device, SoundError *error) {
    *device = kAudioObjectUnknown;
    UInt32 size = sizeof(*device);

    AudioObjectPropertyAddress address = {
        .mSelector = kAudioHardwarePropertyDefaultInputDevice,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };

    if (!osstatus_ok(
            AudioObjectGetPropertyData(
                kAudioObjectSystemObject,
                &address,
                0,
                NULL,
                &size,
                device
            ),
            "AudioObjectGetPropertyData(default input device)",
            error
        )) {
        return false;
    }

    if (*device == kAudioObjectUnknown) {
        sound_error_set(error, "no default input device found");
        return false;
    }

    return true;
}

static bool first_input_stream_format(
    AudioDeviceID device,
    AudioStreamBasicDescription *format,
    SoundError *error
) {
    AudioObjectPropertyAddress streams_address = {
        .mSelector = kAudioDevicePropertyStreams,
        .mScope = kAudioDevicePropertyScopeInput,
        .mElement = kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    if (!osstatus_ok(
            AudioObjectGetPropertyDataSize(device, &streams_address, 0, NULL, &size),
            "AudioObjectGetPropertyDataSize(input streams)",
            error
        )) {
        return false;
    }

    if (size == 0) {
        sound_error_set(error, "input device has no input streams");
        return false;
    }

    AudioStreamID *streams = malloc(size);
    if (!streams) {
        sound_error_set(error, "could not allocate input stream list");
        return false;
    }

    bool ok = osstatus_ok(
        AudioObjectGetPropertyData(device, &streams_address, 0, NULL, &size, streams),
        "AudioObjectGetPropertyData(input streams)",
        error
    );

    if (!ok) {
        free(streams);
        return false;
    }

    AudioStreamID stream = streams[0];
    free(streams);

    AudioObjectPropertyAddress format_address = {
        .mSelector = kAudioStreamPropertyVirtualFormat,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };

    memset(format, 0, sizeof(*format));
    size = sizeof(*format);

    return osstatus_ok(
        AudioObjectGetPropertyData(stream, &format_address, 0, NULL, &size, format),
        "AudioObjectGetPropertyData(stream virtual format)",
        error
    );
}

static bool validate_format(
    const AudioStreamBasicDescription *format,
    SoundError *error
) {
    if (format->mSampleRate <= 0.0) {
        sound_error_set(error, "input device reported an invalid sample rate");
        return false;
    }

    if (format->mFormatID != kAudioFormatLinearPCM ||
        (format->mFormatFlags & kAudioFormatFlagIsFloat) == 0 ||
        format->mBitsPerChannel != 32) {
        sound_error_set(
            error,
            "expected 32-bit float linear PCM from Core Audio HAL; inspect the input format in Audio MIDI Setup"
        );
        return false;
    }

    return true;
}

static uint32_t input_buffer_frame_capacity(AudioDeviceID device) {
    UInt32 frames = 0;
    UInt32 size = sizeof(frames);

    AudioObjectPropertyAddress address = {
        .mSelector = kAudioDevicePropertyBufferFrameSize,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };

    OSStatus status = AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &frames);
    if (status != noErr || frames == 0) {
        frames = 4096;
    }

    return frames < 4096 ? 4096 : frames;
}

/* The first buffer of an input callback, viewed as interleaved frames. */
typedef struct InputChunk {
    const float *interleaved;
    UInt32 frames;
    UInt32 channels;
} InputChunk;

static bool input_chunk_from_buffers(const AudioBufferList *input_data, InputChunk *chunk) {
    if (!input_data || input_data->mNumberBuffers == 0) {
        return false;
    }

    const AudioBuffer *buffer = &input_data->mBuffers[0];
    if (!buffer->mData || buffer->mDataByteSize == 0) {
        return false;
    }

    chunk->channels = buffer->mNumberChannels == 0 ? 1U : buffer->mNumberChannels;
    chunk->frames = buffer->mDataByteSize / (UInt32)(sizeof(float) * chunk->channels);
    chunk->interleaved = buffer->mData;
    return chunk->frames > 0;
}

static void copy_first_channel(float *destination, const InputChunk *chunk, UInt32 frames) {
    if (chunk->channels == 1) {
        memcpy(destination, chunk->interleaved, (size_t)frames * sizeof(float));
        return;
    }

    for (UInt32 frame = 0; frame < frames; ++frame) {
        destination[frame] = chunk->interleaved[frame * chunk->channels];
    }
}

static OSStatus input_callback(
    AudioObjectID device,
    const AudioTimeStamp *now,
    const AudioBufferList *input_data,
    const AudioTimeStamp *input_time,
    AudioBufferList *output_data,
    const AudioTimeStamp *output_time,
    void *client_data
) {
    (void)device;
    (void)now;
    (void)input_time;
    (void)output_data;
    (void)output_time;

    CaptureState *state = client_data;
    InputChunk chunk;
    if (!input_chunk_from_buffers(input_data, &chunk)) {
        return noErr;
    }

    uint_fast64_t index = atomic_load_explicit(&state->write_index, memory_order_relaxed);
    if (index >= state->capacity) {
        return noErr;
    }

    uint64_t available = state->capacity - index;
    UInt32 frames = (uint64_t)chunk.frames > available ? (UInt32)available : chunk.frames;

    copy_first_channel(state->samples + index, &chunk, frames);
    atomic_store_explicit(&state->write_index, index + frames, memory_order_release);
    return noErr;
}

static OSStatus stream_input_callback(
    AudioObjectID device,
    const AudioTimeStamp *now,
    const AudioBufferList *input_data,
    const AudioTimeStamp *input_time,
    AudioBufferList *output_data,
    const AudioTimeStamp *output_time,
    void *client_data
) {
    (void)device;
    (void)now;
    (void)input_time;
    (void)output_data;
    (void)output_time;

    SoundInputStream *stream = client_data;
    InputChunk chunk;
    if (!stream || !stream->callback || !input_chunk_from_buffers(input_data, &chunk)) {
        return noErr;
    }

    if (chunk.channels == 1) {
        stream->callback(chunk.interleaved, chunk.frames, stream->user_data);
        return noErr;
    }

    UInt32 frames = chunk.frames < stream->scratch_capacity ? chunk.frames : stream->scratch_capacity;
    copy_first_channel(stream->scratch, &chunk, frames);
    stream->callback(stream->scratch, frames, stream->user_data);
    return noErr;
}

bool sound_default_input_format(SoundInputFormat *format, SoundError *error) {
    sound_error_clear(error);

    if (!format) {
        sound_error_set(error, "missing output format");
        return false;
    }

    AudioDeviceID device = kAudioObjectUnknown;
    AudioStreamBasicDescription asbd;

    if (!default_input_device(&device, error)) {
        return false;
    }

    if (!first_input_stream_format(device, &asbd, error)) {
        return false;
    }

    copy_format(&asbd, format);
    return true;
}

bool sound_capture_default_input(
    const SoundCaptureOptions *options,
    SoundRecording *recording,
    SoundError *error
) {
    sound_error_clear(error);

    if (!options || !recording) {
        sound_error_set(error, "missing capture options or output recording");
        return false;
    }

    memset(recording, 0, sizeof(*recording));

    double seconds = options->seconds > 0.0 ? options->seconds : 5.0;
    AudioDeviceID device = kAudioObjectUnknown;
    AudioStreamBasicDescription asbd;

    if (!default_input_device(&device, error)) {
        return false;
    }

    if (!first_input_stream_format(device, &asbd, error)) {
        return false;
    }

    if (!validate_format(&asbd, error)) {
        return false;
    }

    double requested_frames = seconds * asbd.mSampleRate;
    if (requested_frames <= 0.0) {
        sound_error_set(error, "capture duration is too short");
        return false;
    }

    if (requested_frames > (double)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "capture duration is too long");
        return false;
    }

    uint64_t target_frames = (uint64_t)requested_frames;

    CaptureState state;
    memset(&state, 0, sizeof(state));
    state.capacity = target_frames;
    state.samples = calloc((size_t)target_frames, sizeof(float));
    atomic_init(&state.write_index, 0);

    if (!state.samples) {
        sound_error_set(error, "could not allocate sample buffer");
        return false;
    }

    AudioDeviceIOProcID callback_id = NULL;
    if (!osstatus_ok(
            AudioDeviceCreateIOProcID(device, input_callback, &state, &callback_id),
            "AudioDeviceCreateIOProcID",
            error
        )) {
        free(state.samples);
        return false;
    }

    bool started = osstatus_ok(
        AudioDeviceStart(device, callback_id),
        "AudioDeviceStart",
        error
    );

    if (started) {
        double start_time = monotonic_seconds();
        uint64_t max_waits = capture_wait_limit(seconds);

        for (uint64_t waits = 0; waits < max_waits; ++waits) {
            if (atomic_load_explicit(&state.write_index, memory_order_acquire) >= target_frames) {
                break;
            }

            double now = monotonic_seconds();
            if (start_time > 0.0 && now > 0.0 && (now - start_time) >= seconds) {
                break;
            }

            sleep_for_callback();
        }

        started = osstatus_ok(AudioDeviceStop(device, callback_id), "AudioDeviceStop", error);
    }

    bool destroyed = osstatus_ok(
        AudioDeviceDestroyIOProcID(device, callback_id),
        "AudioDeviceDestroyIOProcID",
        error
    );

    if (!started || !destroyed) {
        free(state.samples);
        return false;
    }

    uint64_t captured = atomic_load_explicit(&state.write_index, memory_order_acquire);
    if (captured == 0) {
        free(state.samples);
        sound_error_set(error, "captured zero samples; check macOS microphone permissions");
        return false;
    }

    copy_format(&asbd, &recording->format);
    recording->samples = state.samples;
    recording->sample_count = captured;
    return true;
}

void sound_recording_free(SoundRecording *recording) {
    if (!recording) {
        return;
    }

    free(recording->samples);
    memset(recording, 0, sizeof(*recording));
}

bool sound_input_stream_open(
    const SoundInputStreamOptions *options,
    SoundInputStream **stream,
    SoundInputFormat *format,
    SoundError *error
) {
    sound_error_clear(error);

    if (!options || !options->callback || !stream) {
        sound_error_set(error, "missing live input callback or output stream");
        return false;
    }

    *stream = NULL;

    SoundInputStream *created = calloc(1, sizeof(*created));
    if (!created) {
        sound_error_set(error, "could not allocate live input stream");
        return false;
    }

    AudioStreamBasicDescription asbd;

    if (!default_input_device(&created->device, error) ||
        !first_input_stream_format(created->device, &asbd, error) ||
        !validate_format(&asbd, error)) {
        free(created);
        return false;
    }

    created->callback = options->callback;
    created->user_data = options->user_data;
    created->scratch_capacity = input_buffer_frame_capacity(created->device);
    created->scratch = malloc(sizeof(float) * (size_t)created->scratch_capacity);

    if (!created->scratch) {
        free(created);
        sound_error_set(error, "could not allocate live input scratch buffer");
        return false;
    }

    if (!osstatus_ok(
            AudioDeviceCreateIOProcID(
                created->device,
                stream_input_callback,
                created,
                &created->callback_id
            ),
            "AudioDeviceCreateIOProcID",
            error
        )) {
        sound_input_stream_close(created);
        return false;
    }

    if (format) {
        copy_format(&asbd, format);
    }

    *stream = created;
    return true;
}

bool sound_input_stream_start(SoundInputStream *stream, SoundError *error) {
    sound_error_clear(error);

    if (!stream) {
        sound_error_set(error, "missing live input stream");
        return false;
    }

    if (stream->started) {
        return true;
    }

    if (!osstatus_ok(AudioDeviceStart(stream->device, stream->callback_id), "AudioDeviceStart", error)) {
        return false;
    }

    stream->started = true;
    return true;
}

bool sound_input_stream_stop(SoundInputStream *stream, SoundError *error) {
    sound_error_clear(error);

    if (!stream) {
        sound_error_set(error, "missing live input stream");
        return false;
    }

    if (!stream->started) {
        return true;
    }

    if (!osstatus_ok(AudioDeviceStop(stream->device, stream->callback_id), "AudioDeviceStop", error)) {
        return false;
    }

    stream->started = false;
    return true;
}

void sound_input_stream_close(SoundInputStream *stream) {
    if (!stream) {
        return;
    }

    if (stream->started) {
        (void)AudioDeviceStop(stream->device, stream->callback_id);
    }

    if (stream->callback_id) {
        (void)AudioDeviceDestroyIOProcID(stream->device, stream->callback_id);
    }

    free(stream->scratch);
    free(stream);
}
