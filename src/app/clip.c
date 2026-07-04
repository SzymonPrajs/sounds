#include "sounds/clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sound_clip_init(SoundClip *clip) {
    if (!clip) {
        return;
    }

    *clip = (SoundClip){
        .samples = NULL,
        .sample_count = 0,
        .trim_start = 0,
        .trim_end = 0,
        .sample_rate = 0.0,
        .label = "",
    };
}

void sound_clip_free(SoundClip *clip) {
    if (!clip) {
        return;
    }

    free(clip->samples);
    sound_clip_init(clip);
}

bool sound_clip_has_audio(const SoundClip *clip) {
    return clip && clip->samples && clip->trim_end > clip->trim_start;
}

const float *sound_clip_samples(const SoundClip *clip) {
    if (!sound_clip_has_audio(clip)) {
        return NULL;
    }

    return clip->samples + clip->trim_start;
}

uint64_t sound_clip_sample_count(const SoundClip *clip) {
    if (!sound_clip_has_audio(clip)) {
        return 0;
    }

    return clip->trim_end - clip->trim_start;
}

double sound_clip_duration_seconds(const SoundClip *clip) {
    if (!clip || clip->sample_rate <= 0.0) {
        return 0.0;
    }

    return (double)sound_clip_sample_count(clip) / clip->sample_rate;
}

double sound_clip_trim_start_seconds(const SoundClip *clip) {
    if (!clip || clip->sample_rate <= 0.0) {
        return 0.0;
    }

    return (double)clip->trim_start / clip->sample_rate;
}

double sound_clip_trim_end_seconds(const SoundClip *clip) {
    if (!clip || clip->sample_rate <= 0.0 || clip->sample_count < clip->trim_end) {
        return 0.0;
    }

    return (double)(clip->sample_count - clip->trim_end) / clip->sample_rate;
}

static void copy_label(SoundClip *clip, const char *label) {
    if (!label || label[0] == '\0') {
        label = "clip";
    }

    (void)snprintf(clip->label, sizeof(clip->label), "%s", label);
}

bool sound_clip_replace(
    SoundClip *clip,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    const char *label,
    SoundError *error
) {
    if (!clip || !samples || sample_count == 0 || sample_rate <= 0.0) {
        sound_error_set(error, "invalid clip replacement");
        return false;
    }

    if (sample_count > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "clip is too large");
        return false;
    }

    float *copy = malloc(sizeof(float) * (size_t)sample_count);
    if (!copy) {
        sound_error_set(error, "could not allocate clip samples");
        return false;
    }

    memcpy(copy, samples, sizeof(float) * (size_t)sample_count);
    free(clip->samples);

    clip->samples = copy;
    clip->sample_count = sample_count;
    clip->trim_start = 0;
    clip->trim_end = sample_count;
    clip->sample_rate = sample_rate;
    copy_label(clip, label);
    return true;
}

bool sound_clip_replace_from_ring(
    SoundClip *clip,
    const SoundRingBuffer *ring,
    uint64_t end_sample,
    uint64_t requested_samples,
    double sample_rate,
    const char *label,
    SoundError *error
) {
    if (!clip || !ring || requested_samples == 0 || sample_rate <= 0.0) {
        sound_error_set(error, "invalid ring clip replacement");
        return false;
    }

    uint64_t capacity = sound_ring_buffer_capacity(ring);
    uint64_t wanted = requested_samples < capacity ? requested_samples : capacity;
    if (wanted == 0 || wanted > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "clip request is too large");
        return false;
    }

    float *samples = malloc(sizeof(float) * (size_t)wanted);
    if (!samples) {
        sound_error_set(error, "could not allocate clip samples");
        return false;
    }

    uint64_t count = sound_ring_buffer_read_ending_at(ring, end_sample, samples, wanted);
    if (count == 0) {
        free(samples);
        sound_error_set(error, "not enough audio for clip");
        return false;
    }

    bool ok = sound_clip_replace(clip, samples, count, sample_rate, label, error);
    free(samples);
    return ok;
}

void sound_clip_clear_trim(SoundClip *clip) {
    if (!clip) {
        return;
    }

    clip->trim_start = 0;
    clip->trim_end = clip->sample_count;
}

void sound_clip_trim_start_by(SoundClip *clip, uint64_t samples) {
    if (!clip || clip->trim_end <= clip->trim_start + 1U) {
        return;
    }

    uint64_t limit = clip->trim_end - 1U;
    clip->trim_start = clip->trim_start + samples < limit ?
        clip->trim_start + samples :
        limit;
}

void sound_clip_trim_end_by(SoundClip *clip, uint64_t samples) {
    if (!clip || clip->trim_end <= clip->trim_start + 1U) {
        return;
    }

    uint64_t limit = clip->trim_start + 1U;
    clip->trim_end = clip->trim_end > limit + samples ?
        clip->trim_end - samples :
        limit;
}
