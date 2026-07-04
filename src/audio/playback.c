#include "sounds/playback.h"

#include <CoreAudio/CoreAudio.h>

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct SoundPlayback {
    AudioDeviceID device;
    AudioDeviceIOProcID callback_id;
    float *samples;
    uint64_t frame_count;
    double source_frame;
    double source_step;
    double output_sample_rate;
    atomic_bool playing;
    atomic_uint_fast64_t position;
    bool started;
};

static bool osstatus_ok(OSStatus status, const char *operation, SoundError *error) {
    if (status == noErr) {
        return true;
    }

    sound_error_set(error, "%s failed with OSStatus %d", operation, (int)status);
    return false;
}

static bool default_output_device(AudioDeviceID *device, SoundError *error) {
    *device = kAudioObjectUnknown;
    UInt32 size = sizeof(*device);

    AudioObjectPropertyAddress address = {
        .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
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
            "AudioObjectGetPropertyData(default output device)",
            error
        )) {
        return false;
    }

    if (*device == kAudioObjectUnknown) {
        sound_error_set(error, "no default output device found");
        return false;
    }

    return true;
}

static bool first_output_stream_format(
    AudioDeviceID device,
    AudioStreamBasicDescription *format,
    SoundError *error
) {
    AudioObjectPropertyAddress streams_address = {
        .mSelector = kAudioDevicePropertyStreams,
        .mScope = kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMain,
    };

    UInt32 size = 0;
    if (!osstatus_ok(
            AudioObjectGetPropertyDataSize(device, &streams_address, 0, NULL, &size),
            "AudioObjectGetPropertyDataSize(output streams)",
            error
        )) {
        return false;
    }

    if (size == 0) {
        sound_error_set(error, "output device has no output streams");
        return false;
    }

    AudioStreamID *streams = malloc(size);
    if (!streams) {
        sound_error_set(error, "could not allocate output stream list");
        return false;
    }

    bool ok = osstatus_ok(
        AudioObjectGetPropertyData(device, &streams_address, 0, NULL, &size, streams),
        "AudioObjectGetPropertyData(output streams)",
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
        "AudioObjectGetPropertyData(output stream virtual format)",
        error
    );
}

static bool validate_output_format(
    const AudioStreamBasicDescription *format,
    SoundError *error
) {
    if (format->mSampleRate <= 0.0) {
        sound_error_set(error, "output device reported an invalid sample rate");
        return false;
    }

    if (format->mFormatID != kAudioFormatLinearPCM ||
        (format->mFormatFlags & kAudioFormatFlagIsFloat) == 0 ||
        format->mBitsPerChannel != 32) {
        sound_error_set(
            error,
            "expected 32-bit float linear PCM from Core Audio HAL output"
        );
        return false;
    }

    return true;
}

static UInt32 buffer_frame_count(const AudioBuffer *buffer) {
    if (!buffer || !buffer->mData || buffer->mDataByteSize == 0) {
        return 0;
    }

    UInt32 channels = buffer->mNumberChannels == 0 ? 1U : buffer->mNumberChannels;
    return buffer->mDataByteSize / (UInt32)(sizeof(float) * channels);
}

static UInt32 output_frame_count(const AudioBufferList *buffers) {
    UInt32 frames = 0;

    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
        UInt32 buffer_frames = buffer_frame_count(&buffers->mBuffers[i]);
        frames = buffer_frames > frames ? buffer_frames : frames;
    }

    return frames;
}

static void clear_output(AudioBufferList *buffers) {
    if (!buffers) {
        return;
    }

    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
        AudioBuffer *buffer = &buffers->mBuffers[i];
        if (buffer->mData && buffer->mDataByteSize > 0) {
            memset(buffer->mData, 0, buffer->mDataByteSize);
        }
    }
}

static void write_output_sample(AudioBufferList *buffers, UInt32 frame, float sample) {
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
        AudioBuffer *buffer = &buffers->mBuffers[i];
        UInt32 channels = buffer->mNumberChannels == 0 ? 1U : buffer->mNumberChannels;
        UInt32 frames = buffer_frame_count(buffer);

        if (!buffer->mData || frame >= frames) {
            continue;
        }

        float *output = buffer->mData;
        for (UInt32 channel = 0; channel < channels; ++channel) {
            output[(frame * channels) + channel] = sample;
        }
    }
}

static float source_sample_at(const SoundPlayback *playback, double frame) {
    uint64_t index = (uint64_t)frame;

    if (index >= playback->frame_count) {
        return 0.0F;
    }

    uint64_t next = index + 1;
    if (next >= playback->frame_count) {
        return playback->samples[index];
    }

    float a = playback->samples[index];
    float b = playback->samples[next];
    float amount = (float)(frame - (double)index);
    return a + ((b - a) * amount);
}

static uint64_t clamped_position(const SoundPlayback *playback) {
    if (playback->source_frame >= (double)playback->frame_count) {
        return playback->frame_count;
    }

    if (playback->source_frame <= 0.0) {
        return 0;
    }

    return (uint64_t)playback->source_frame;
}

static bool render_next_sample(SoundPlayback *playback, float *sample) {
    if (playback->source_frame >= (double)playback->frame_count) {
        *sample = 0.0F;
        return false;
    }

    *sample = source_sample_at(playback, playback->source_frame);
    playback->source_frame += playback->source_step;

    if (playback->source_frame > (double)playback->frame_count) {
        playback->source_frame = (double)playback->frame_count;
    }

    return true;
}

static OSStatus stream_output_callback(
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
    (void)input_data;
    (void)input_time;
    (void)output_time;

    SoundPlayback *playback = client_data;
    if (!playback || !output_data) {
        return noErr;
    }

    if (!atomic_load_explicit(&playback->playing, memory_order_acquire)) {
        clear_output(output_data);
        return noErr;
    }

    UInt32 frames = output_frame_count(output_data);
    bool still_playing = true;

    for (UInt32 frame = 0; frame < frames; ++frame) {
        float sample = 0.0F;
        if (still_playing) {
            still_playing = render_next_sample(playback, &sample);
        }

        write_output_sample(output_data, frame, sample);
    }

    uint64_t position = clamped_position(playback);
    atomic_store_explicit(&playback->position, position, memory_order_release);

    if (!still_playing || position >= playback->frame_count) {
        atomic_store_explicit(&playback->playing, false, memory_order_release);
    }

    return noErr;
}

static void release_samples(SoundPlayback *playback) {
    free(playback->samples);
    playback->samples = NULL;
    playback->frame_count = 0;
    playback->source_frame = 0.0;
    playback->source_step = 1.0;
}

static bool stop_device(SoundPlayback *playback, SoundError *error) {
    if (!playback->started) {
        return true;
    }

    if (!osstatus_ok(
            AudioDeviceStop(playback->device, playback->callback_id),
            "AudioDeviceStop",
            error
        )) {
        return false;
    }

    playback->started = false;
    return true;
}

bool sound_playback_open(SoundPlayback **playback, SoundError *error) {
    sound_error_clear(error);

    if (!playback) {
        sound_error_set(error, "missing playback output");
        return false;
    }

    *playback = NULL;

    SoundPlayback *created = calloc(1, sizeof(*created));
    if (!created) {
        sound_error_set(error, "could not allocate playback stream");
        return false;
    }

    AudioStreamBasicDescription asbd;
    if (!default_output_device(&created->device, error) ||
        !first_output_stream_format(created->device, &asbd, error) ||
        !validate_output_format(&asbd, error)) {
        free(created);
        return false;
    }

    created->output_sample_rate = asbd.mSampleRate;
    created->source_step = 1.0;
    atomic_init(&created->playing, false);
    atomic_init(&created->position, 0);

    if (!osstatus_ok(
            AudioDeviceCreateIOProcID(
                created->device,
                stream_output_callback,
                created,
                &created->callback_id
            ),
            "AudioDeviceCreateIOProcID",
            error
        )) {
        sound_playback_close(created);
        return false;
    }

    *playback = created;
    return true;
}

bool sound_playback_start(
    SoundPlayback *playback,
    const float *samples,
    uint64_t frame_count,
    double sample_rate,
    SoundError *error
) {
    sound_error_clear(error);

    if (!playback) {
        sound_error_set(error, "missing playback stream");
        return false;
    }

    if (!samples || frame_count == 0 || !isfinite(sample_rate) || sample_rate <= 0.0) {
        sound_error_set(error, "invalid playback buffer");
        return false;
    }

    if (frame_count > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "playback buffer is too large");
        return false;
    }

    if (!stop_device(playback, error)) {
        return false;
    }

    release_samples(playback);

    float *copy = malloc((size_t)frame_count * sizeof(float));
    if (!copy) {
        sound_error_set(error, "could not allocate playback samples");
        return false;
    }

    memcpy(copy, samples, (size_t)frame_count * sizeof(float));

    playback->samples = copy;
    playback->frame_count = frame_count;
    playback->source_frame = 0.0;
    playback->source_step = sample_rate / playback->output_sample_rate;
    atomic_store_explicit(&playback->position, 0, memory_order_release);
    atomic_store_explicit(&playback->playing, true, memory_order_release);

    if (!osstatus_ok(
            AudioDeviceStart(playback->device, playback->callback_id),
            "AudioDeviceStart",
            error
        )) {
        atomic_store_explicit(&playback->playing, false, memory_order_release);
        release_samples(playback);
        return false;
    }

    playback->started = true;
    return true;
}

bool sound_playback_stop(SoundPlayback *playback, SoundError *error) {
    sound_error_clear(error);

    if (!playback) {
        sound_error_set(error, "missing playback stream");
        return false;
    }

    if (!stop_device(playback, error)) {
        return false;
    }

    atomic_store_explicit(&playback->playing, false, memory_order_release);
    release_samples(playback);
    return true;
}

bool sound_playback_is_playing(const SoundPlayback *playback) {
    if (!playback) {
        return false;
    }

    return atomic_load_explicit(&playback->playing, memory_order_acquire);
}

uint64_t sound_playback_position(const SoundPlayback *playback) {
    if (!playback) {
        return 0;
    }

    return atomic_load_explicit(&playback->position, memory_order_acquire);
}

void sound_playback_close(SoundPlayback *playback) {
    if (!playback) {
        return;
    }

    if (playback->started) {
        (void)AudioDeviceStop(playback->device, playback->callback_id);
    }

    if (playback->callback_id) {
        (void)AudioDeviceDestroyIOProcID(playback->device, playback->callback_id);
    }

    release_samples(playback);
    free(playback);
}
