#include "../workbench.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "internal.h"

void workbench_format_created_label(uint64_t created_at, char *text, size_t text_size) {
    time_t timestamp = (time_t)created_at;
    struct tm local_time;

    if (timestamp <= 0 || !localtime_r(&timestamp, &local_time) ||
        strftime(text, text_size, "%Y-%m-%d %H:%M", &local_time) == 0) {
        (void)snprintf(text, text_size, "unknown time");
    }
}

void workbench_copy_recording_label(
    char *label,
    size_t label_size,
    const char *path
) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    char trimmed[SOUND_UI_RECORDING_LABEL_CAPACITY];
    (void)snprintf(trimmed, sizeof(trimmed), "%s", name);

    char *extension = strrchr(trimmed, '.');
    if (extension &&
        (strcmp(extension, ".wav") == 0 ||
            strcmp(extension, ".WAV") == 0 ||
            strcmp(extension, ".Wav") == 0)) {
        *extension = '\0';
    }

    (void)snprintf(label, label_size, "%s", trimmed[0] != '\0' ? trimmed : "recording");
}

uint64_t workbench_file_created_at(const struct stat *status) {
#if defined(__APPLE__)
    if (status->st_birthtimespec.tv_sec > 0) {
        return (uint64_t)status->st_birthtimespec.tv_sec;
    }
#endif

    return status->st_mtime > 0 ? (uint64_t)status->st_mtime : 0;
}

void workbench_recording_init(WorkbenchRecording *recording) {
    sound_clip_init(&recording->clip);
    recording->summary = (SoundUiRecordingSummary){0};
    recording->path[0] = '\0';
    recording->created_at = 0;
    recording->loaded = false;
    recording->disk_backed = false;
}

void workbench_recording_free(WorkbenchRecording *recording) {
    sound_clip_free(&recording->clip);
    workbench_recording_init(recording);
}

static bool same_recording_path(const WorkbenchRecording *recording, const char *path) {
    return path && path[0] != '\0' && strcmp(recording->path, path) == 0;
}

void workbench_refresh_recording_summaries(WorkbenchAudio *audio) {
    for (uint64_t i = 0; i < audio->recording_count; ++i) {
        audio->recording_summaries[i] = audio->recordings[i].summary;
    }
}

void workbench_audio_init(WorkbenchAudio *audio) {
    sound_clip_init(&audio->clip);
    audio->recordings = NULL;
    audio->recording_summaries = NULL;
    audio->recording_count = 0;
    audio->recording_capacity = 0;
    audio->selected_recording = 0;
    audio->active_recording = 0;
    audio->recording_scan = NULL;
    audio->recording_scan_complete = false;
    audio->recording_scan_failed = false;
    audio->spectrum_cells = NULL;
    audio->spectrum_row_count = 0;
    audio->spectrum_min_hz = 0.0;
    audio->spectrum_max_hz = 0.0;
    audio->active_spectrogram = (WorkbenchOfflineSpectrogramCache){.dirty = true};
    audio->band_spectrogram = (WorkbenchOfflineSpectrogramCache){.dirty = true};
    audio->selected_samples = NULL;
    audio->rejected_samples = NULL;
    audio->render_count = 0;
    audio->low_hz = SOUND_FREQUENCY_MID_MIN_HZ;
    audio->high_hz = SOUND_FREQUENCY_MID_MAX_HZ;
    audio->playback_offset = 0;
    audio->band_method = SOUND_BAND_RENDER_FFT_MASK;
    audio->audition = SOUND_AUDITION_ORIGINAL;
    audio->trim_edge = WORKBENCH_TRIM_START;
    audio->upper_handle_selected = false;
    audio->trim_editing = false;
    audio->has_active_recording = false;
    audio->recording_rename_active = false;
    audio->recording_delete_pending = false;
    audio->spectrum_dirty = true;
    audio->render_dirty = true;
    audio->draft_trim_start = 0;
    audio->draft_trim_end = 0;
    audio->recording_delete_index = 0;
}

void workbench_audio_free(WorkbenchAudio *audio) {
    sound_clip_free(&audio->clip);
    for (uint64_t i = 0; i < audio->recording_count; ++i) {
        workbench_recording_free(&audio->recordings[i]);
    }
    free(audio->recordings);
    free(audio->recording_summaries);

    if (audio->recording_scan) {
        if (!audio->recording_scan->joined) {
            (void)pthread_join(audio->recording_scan->thread, NULL);
        }
        (void)pthread_mutex_destroy(&audio->recording_scan->mutex);
        for (uint64_t i = 0; i < audio->recording_scan->recording_count; ++i) {
            workbench_recording_free(&audio->recording_scan->recordings[i]);
        }
        free(audio->recording_scan->recordings);
        free(audio->recording_scan);
    }

    free(audio->spectrum_cells);
    free(audio->active_spectrogram.cells);
    free(audio->band_spectrogram.cells);
    free(audio->selected_samples);
    free(audio->rejected_samples);
}

void workbench_sync_selected_recording_trim(WorkbenchAudio *audio) {
    if (!audio->has_active_recording || audio->active_recording >= audio->recording_count) {
        return;
    }

    WorkbenchRecording *recording = &audio->recordings[audio->active_recording];
    if (!recording->loaded || recording->clip.sample_count != audio->clip.sample_count) {
        return;
    }

    recording->clip.trim_start = audio->clip.trim_start;
    recording->clip.trim_end = audio->clip.trim_end;
}

static bool ensure_recording_capacity(
    WorkbenchAudio *audio,
    uint64_t wanted,
    SoundError *error
) {
    if (wanted <= audio->recording_capacity) {
        return true;
    }

    uint64_t capacity = audio->recording_capacity > 0 ?
        audio->recording_capacity * 2U :
        4U;
    while (capacity < wanted) {
        capacity *= 2U;
    }

    if (capacity > (uint64_t)(SIZE_MAX / sizeof(*audio->recordings)) ||
        capacity > (uint64_t)(SIZE_MAX / sizeof(*audio->recording_summaries))) {
        sound_error_set(error, "too many recordings");
        return false;
    }

    WorkbenchRecording *recordings =
        realloc(audio->recordings, sizeof(*audio->recordings) * (size_t)capacity);
    if (!recordings) {
        sound_error_set(error, "could not allocate recording list");
        return false;
    }

    SoundUiRecordingSummary *summaries = realloc(
        audio->recording_summaries,
        sizeof(*audio->recording_summaries) * (size_t)capacity
    );
    if (!summaries) {
        audio->recordings = recordings;
        sound_error_set(error, "could not allocate recording summaries");
        return false;
    }

    for (uint64_t i = audio->recording_capacity; i < capacity; ++i) {
        workbench_recording_init(&recordings[i]);
        summaries[i] = (SoundUiRecordingSummary){0};
    }

    audio->recordings = recordings;
    audio->recording_summaries = summaries;
    audio->recording_capacity = capacity;
    return true;
}

static bool load_recording_samples(
    WorkbenchRecording *recording,
    SoundError *error
) {
    if (recording->loaded) {
        return true;
    }

    if (!recording->disk_backed || recording->path[0] == '\0') {
        sound_error_set(error, "recording is not loaded");
        return false;
    }

    float *samples = NULL;
    uint64_t sample_count = 0;
    double sample_rate = 0.0;

    if (!sound_recording_load_samples(
            recording->path,
            &samples,
            &sample_count,
            &sample_rate,
            error
        )) {
        return false;
    }

    bool ok = sound_clip_replace(
        &recording->clip,
        samples,
        sample_count,
        sample_rate,
        recording->summary.label,
        error
    );
    free(samples);

    if (!ok) {
        return false;
    }

    recording->summary.seconds =
        sample_rate > 0.0 ? (double)sample_count / sample_rate : 0.0;
    recording->loaded = true;
    recording->summary.loaded = true;
    return true;
}

bool workbench_select_recording_index(
    WorkbenchAudio *audio,
    uint64_t index,
    SoundError *error
) {
    if (index >= audio->recording_count) {
        return true;
    }

    workbench_sync_selected_recording_trim(audio);

    WorkbenchRecording *recording = &audio->recordings[index];
    if (!load_recording_samples(recording, error)) {
        return false;
    }

    if (!sound_clip_replace(
            &audio->clip,
            recording->clip.samples,
            recording->clip.sample_count,
            recording->clip.sample_rate,
            recording->clip.label,
            error
        )) {
        return false;
    }

    audio->clip.trim_start = recording->clip.trim_start;
    audio->clip.trim_end = recording->clip.trim_end;
    audio->selected_recording = index;
    audio->active_recording = index;
    audio->has_active_recording = true;
    audio->trim_editing = false;
    audio->recording_delete_pending = false;
    audio->recording_summaries[index] = recording->summary;
    workbench_mark_clip_changed(audio);
    return true;
}

bool workbench_ensure_selected_recording_loaded(
    WorkbenchAudio *audio,
    SoundError *error
) {
    if (!audio || audio->recording_count == 0) {
        return true;
    }

    if (audio->selected_recording >= audio->recording_count) {
        audio->selected_recording = 0;
    }

    WorkbenchRecording *recording = &audio->recordings[audio->selected_recording];
    if (audio->has_active_recording &&
        audio->active_recording == audio->selected_recording &&
        recording->loaded &&
        sound_clip_has_audio(&audio->clip) &&
        audio->clip.sample_count == recording->clip.sample_count) {
        return true;
    }

    return workbench_select_recording_index(audio, audio->selected_recording, error);
}

bool workbench_insert_recording_sorted(
    WorkbenchAudio *audio,
    WorkbenchRecording *recording,
    SoundError *error,
    uint64_t *inserted_index
) {
    *inserted_index = 0;

    if (recording->path[0] != '\0') {
        for (uint64_t i = 0; i < audio->recording_count; ++i) {
            if (same_recording_path(&audio->recordings[i], recording->path)) {
                workbench_recording_free(recording);
                *inserted_index = i;
                return true;
            }
        }
    }

    if (!ensure_recording_capacity(audio, audio->recording_count + 1U, error)) {
        return false;
    }

    uint64_t index = 0;
    while (index < audio->recording_count &&
        audio->recordings[index].created_at >= recording->created_at) {
        ++index;
    }

    if (index <= audio->selected_recording && audio->recording_count > 0) {
        ++audio->selected_recording;
    }

    if (audio->has_active_recording &&
        index <= audio->active_recording &&
        audio->recording_count > 0) {
        ++audio->active_recording;
    }

    uint64_t moved = audio->recording_count - index;
    if (moved > 0) {
        memmove(
            &audio->recordings[index + 1U],
            &audio->recordings[index],
            sizeof(*audio->recordings) * (size_t)moved
        );
        memmove(
            &audio->recording_summaries[index + 1U],
            &audio->recording_summaries[index],
            sizeof(*audio->recording_summaries) * (size_t)moved
        );
    }

    audio->recordings[index] = *recording;
    audio->recording_summaries[index] = recording->summary;
    workbench_recording_init(recording);
    ++audio->recording_count;
    *inserted_index = index;
    return true;
}

void workbench_mark_clip_changed(WorkbenchAudio *audio) {
    audio->spectrum_dirty = true;
    audio->active_spectrogram.dirty = true;
    audio->band_spectrogram.dirty = true;
    audio->band_spectrogram.columns = 0;
    audio->band_spectrogram.rows = 0;
    audio->render_dirty = true;
    audio->render_count = 0;
    audio->playback_offset = audio->clip.trim_start;
}

bool workbench_add_recording_from_samples(
    WorkbenchAudio *audio,
    const float *samples,
    uint64_t sample_count,
    double sample_rate,
    const char *path,
    SoundError *error
) {
    if (!audio || !samples || sample_count == 0) {
        return true;
    }

    workbench_sync_selected_recording_trim(audio);

    WorkbenchRecording recording;
    workbench_recording_init(&recording);

    if (path && path[0] != '\0') {
        (void)snprintf(recording.path, sizeof(recording.path), "%s", path);
        recording.disk_backed = true;
    }

    workbench_copy_recording_label(
        recording.summary.label,
        sizeof(recording.summary.label),
        path && path[0] != '\0' ? path : "new recording"
    );

    struct stat status;
    recording.created_at =
        path && path[0] != '\0' && stat(path, &status) == 0 ?
            workbench_file_created_at(&status) :
            (uint64_t)time(NULL);
    workbench_format_created_label(
        recording.created_at,
        recording.summary.created,
        sizeof(recording.summary.created)
    );
    recording.summary.seconds = sample_rate > 0.0 ?
        (double)sample_count / sample_rate :
        0.0;
    recording.summary.loaded = true;

    if (!sound_clip_replace(
            &recording.clip,
            samples,
            sample_count,
            sample_rate,
            recording.summary.label,
            error
        )) {
        workbench_recording_free(&recording);
        return false;
    }

    recording.loaded = true;

    uint64_t inserted = 0;
    if (!workbench_insert_recording_sorted(audio, &recording, error, &inserted)) {
        workbench_recording_free(&recording);
        return false;
    }

    return workbench_select_recording_index(audio, inserted, error);
}
