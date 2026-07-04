#include "workbench.h"

#include "sounds/offline_spectrum.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>

static const double trim_step_seconds = 0.25;

struct WorkbenchRecordingScan {
    pthread_t thread;
    pthread_mutex_t mutex;
    WorkbenchRecording *recordings;
    uint64_t recording_count;
    bool complete;
    bool failed;
    bool joined;
    char error[256];
};

static uint64_t sample_count_for_seconds(double sample_rate, double seconds) {
    return (uint64_t)(sample_rate * seconds);
}

static const char *audition_name(SoundAuditionTarget target) {
    switch (target) {
        case SOUND_AUDITION_ORIGINAL:
            return "ORIGINAL";
        case SOUND_AUDITION_SELECTED:
            return "SELECTED BAND";
        case SOUND_AUDITION_REJECTED:
            return "REJECTED BAND";
        case SOUND_AUDITION_COUNT:
            break;
    }

    return "ORIGINAL";
}

static void format_created_label(uint64_t created_at, char *text, size_t text_size) {
    time_t timestamp = (time_t)created_at;
    struct tm local_time;

    if (timestamp <= 0 || !localtime_r(&timestamp, &local_time) ||
        strftime(text, text_size, "%Y-%m-%d %H:%M", &local_time) == 0) {
        (void)snprintf(text, text_size, "unknown time");
    }
}

static void copy_recording_label(
    char *label,
    size_t label_size,
    const char *path
) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    char trimmed[SOUND_UI_RECORDING_LABEL_CAPACITY];
    (void)snprintf(trimmed, sizeof(trimmed), "%s", name);

    char *extension = strrchr(trimmed, '.');
    if (extension && strcmp(extension, ".wav") == 0) {
        *extension = '\0';
    }

    (void)snprintf(label, label_size, "%s", trimmed[0] != '\0' ? trimmed : "recording");
}

static uint64_t file_created_at(const struct stat *status) {
#if defined(__APPLE__)
    if (status->st_birthtimespec.tv_sec > 0) {
        return (uint64_t)status->st_birthtimespec.tv_sec;
    }
#endif

    return status->st_mtime > 0 ? (uint64_t)status->st_mtime : 0;
}

static void workbench_recording_init(WorkbenchRecording *recording) {
    sound_clip_init(&recording->clip);
    recording->summary = (SoundUiRecordingSummary){0};
    recording->path[0] = '\0';
    recording->created_at = 0;
    recording->loaded = false;
    recording->disk_backed = false;
}

static void workbench_recording_free(WorkbenchRecording *recording) {
    sound_clip_free(&recording->clip);
    workbench_recording_init(recording);
}

static bool same_recording_path(const WorkbenchRecording *recording, const char *path) {
    return path && path[0] != '\0' && strcmp(recording->path, path) == 0;
}

void workbench_audio_init(WorkbenchAudio *audio) {
    sound_clip_init(&audio->clip);
    audio->recordings = NULL;
    audio->recording_count = 0;
    audio->recording_capacity = 0;
    audio->selected_recording = 0;
    audio->recording_scan = NULL;
    audio->recording_scan_complete = false;
    audio->recording_scan_failed = false;
    audio->spectrum_rows = NULL;
    audio->spectrum_row_count = 0;
    audio->selected_samples = NULL;
    audio->rejected_samples = NULL;
    audio->render_count = 0;
    audio->low_hz = 120.0;
    audio->high_hz = 1000.0;
    audio->playback_offset = 0;
    audio->band_method = SOUND_BAND_RENDER_FFT_MASK;
    audio->audition = SOUND_AUDITION_ORIGINAL;
    audio->upper_handle_selected = false;
    audio->spectrum_dirty = true;
    audio->render_dirty = true;
}

void workbench_audio_free(WorkbenchAudio *audio) {
    sound_clip_free(&audio->clip);
    for (uint64_t i = 0; i < audio->recording_count; ++i) {
        workbench_recording_free(&audio->recordings[i]);
    }
    free(audio->recordings);

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

    free(audio->spectrum_rows);
    free(audio->selected_samples);
    free(audio->rejected_samples);
}

static void sync_selected_recording_trim(WorkbenchAudio *audio) {
    if (audio->selected_recording >= audio->recording_count) {
        return;
    }

    WorkbenchRecording *recording = &audio->recordings[audio->selected_recording];
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

    if (capacity > (uint64_t)(SIZE_MAX / sizeof(*audio->recordings))) {
        sound_error_set(error, "too many recordings");
        return false;
    }

    SoundClip *recordings =
        realloc(audio->recordings, sizeof(*audio->recordings) * (size_t)capacity);
    if (!recordings) {
        sound_error_set(error, "could not allocate recording list");
        return false;
    }

    for (uint64_t i = audio->recording_capacity; i < capacity; ++i) {
        workbench_recording_init(&recordings[i]);
    }

    audio->recordings = recordings;
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

static bool select_recording(
    WorkbenchAudio *audio,
    uint64_t index,
    SoundError *error
) {
    if (index >= audio->recording_count) {
        return true;
    }

    sync_selected_recording_trim(audio);

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
    if (recording->loaded && sound_clip_has_audio(&audio->clip) &&
        audio->clip.sample_count == recording->clip.sample_count) {
        return true;
    }

    return select_recording(audio, audio->selected_recording, error);
}

static bool insert_recording_sorted(
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

    for (uint64_t i = audio->recording_count; i > index; --i) {
        audio->recordings[i] = audio->recordings[i - 1U];
    }

    audio->recordings[index] = *recording;
    workbench_recording_init(recording);
    ++audio->recording_count;
    *inserted_index = index;
    return true;
}

static bool has_wav_extension(const char *name) {
    const char *extension = strrchr(name, '.');

    if (!extension) {
        return false;
    }

    return strcmp(extension, ".wav") == 0 ||
        strcmp(extension, ".WAV") == 0 ||
        strcmp(extension, ".Wav") == 0;
}

static int compare_recordings_newest_first(const void *left, const void *right) {
    const WorkbenchRecording *a = left;
    const WorkbenchRecording *b = right;

    if (a->created_at < b->created_at) {
        return 1;
    }

    if (a->created_at > b->created_at) {
        return -1;
    }

    return strcmp(a->summary.label, b->summary.label);
}

static bool scan_recording_capacity(
    WorkbenchRecording **recordings,
    uint64_t *capacity,
    uint64_t wanted
) {
    if (wanted <= *capacity) {
        return true;
    }

    uint64_t next = *capacity > 0 ? *capacity * 2U : 16U;
    while (next < wanted) {
        next *= 2U;
    }

    if (next > (uint64_t)(SIZE_MAX / sizeof(**recordings))) {
        return false;
    }

    WorkbenchRecording *grown =
        realloc(*recordings, sizeof(**recordings) * (size_t)next);
    if (!grown) {
        return false;
    }

    for (uint64_t i = *capacity; i < next; ++i) {
        workbench_recording_init(&grown[i]);
    }

    *recordings = grown;
    *capacity = next;
    return true;
}

static bool recording_from_path(
    const char *path,
    const struct stat *status,
    WorkbenchRecording *recording
) {
    SoundError error;
    SoundRecordingInfo info;
    sound_error_clear(&error);

    if (!sound_recording_read_info(path, &info, &error)) {
        return false;
    }

    workbench_recording_init(recording);
    (void)snprintf(recording->path, sizeof(recording->path), "%s", path);
    recording->created_at = file_created_at(status);
    recording->disk_backed = true;
    recording->loaded = false;
    copy_recording_label(
        recording->summary.label,
        sizeof(recording->summary.label),
        path
    );
    format_created_label(
        recording->created_at,
        recording->summary.created,
        sizeof(recording->summary.created)
    );
    recording->summary.seconds = info.duration_seconds;
    recording->summary.loaded = false;
    return true;
}

static void recording_scan_finish(
    WorkbenchRecordingScan *scan,
    WorkbenchRecording *recordings,
    uint64_t recording_count,
    const char *error
) {
    (void)pthread_mutex_lock(&scan->mutex);
    scan->recordings = recordings;
    scan->recording_count = recording_count;
    scan->failed = error != NULL;
    if (error) {
        (void)snprintf(scan->error, sizeof(scan->error), "%s", error);
    }
    scan->complete = true;
    (void)pthread_mutex_unlock(&scan->mutex);
}

static void *recording_scan_thread(void *user_data) {
    WorkbenchRecordingScan *scan = user_data;
    WorkbenchRecording *recordings = NULL;
    uint64_t recording_count = 0;
    uint64_t recording_capacity = 0;
    const char *directory = sound_recording_directory();
    DIR *dir = opendir(directory);

    if (!dir) {
        if (errno == ENOENT) {
            recording_scan_finish(scan, NULL, 0, NULL);
        } else {
            recording_scan_finish(scan, NULL, 0, "could not open recordings folder");
        }
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !has_wav_extension(entry->d_name)) {
            continue;
        }

        char path[SOUND_RECORDING_PATH_CAPACITY];
        int written = snprintf(
            path,
            sizeof(path),
            "%s/%s",
            directory,
            entry->d_name
        );
        if (written <= 0 || (size_t)written >= sizeof(path)) {
            continue;
        }

        struct stat status;
        if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
            continue;
        }

        if (!scan_recording_capacity(
                &recordings,
                &recording_capacity,
                recording_count + 1U
            )) {
            (void)closedir(dir);
            for (uint64_t i = 0; i < recording_count; ++i) {
                workbench_recording_free(&recordings[i]);
            }
            free(recordings);
            recording_scan_finish(scan, NULL, 0, "could not allocate recording scan");
            return NULL;
        }

        if (recording_from_path(path, &status, &recordings[recording_count])) {
            ++recording_count;
        }
    }

    (void)closedir(dir);

    if (recording_count > 1) {
        qsort(
            recordings,
            (size_t)recording_count,
            sizeof(*recordings),
            compare_recordings_newest_first
        );
    }

    recording_scan_finish(scan, recordings, recording_count, NULL);
    return NULL;
}

bool workbench_start_recording_scan(WorkbenchAudio *audio, SoundError *error) {
    if (!audio || audio->recording_scan) {
        return true;
    }

    WorkbenchRecordingScan *scan = calloc(1, sizeof(*scan));
    if (!scan) {
        sound_error_set(error, "could not allocate recording scanner");
        return false;
    }

    if (pthread_mutex_init(&scan->mutex, NULL) != 0) {
        free(scan);
        sound_error_set(error, "could not initialize recording scanner");
        return false;
    }

    if (pthread_create(&scan->thread, NULL, recording_scan_thread, scan) != 0) {
        (void)pthread_mutex_destroy(&scan->mutex);
        free(scan);
        sound_error_set(error, "could not start recording scanner");
        return false;
    }

    audio->recording_scan = scan;
    return true;
}

bool workbench_poll_recording_scan(WorkbenchAudio *audio, SoundError *error) {
    if (!audio || !audio->recording_scan || audio->recording_scan_complete) {
        return true;
    }

    WorkbenchRecordingScan *scan = audio->recording_scan;

    (void)pthread_mutex_lock(&scan->mutex);
    bool complete = scan->complete;
    (void)pthread_mutex_unlock(&scan->mutex);

    if (!complete) {
        return true;
    }

    if (!scan->joined) {
        (void)pthread_join(scan->thread, NULL);
        scan->joined = true;
    }

    audio->recording_scan_complete = true;
    audio->recording_scan_failed = scan->failed;

    if (scan->failed) {
        return true;
    }

    uint64_t previous_count = audio->recording_count;
    for (uint64_t i = 0; i < scan->recording_count; ++i) {
        uint64_t inserted = 0;
        if (!insert_recording_sorted(audio, &scan->recordings[i], error, &inserted)) {
            return false;
        }
    }

    free(scan->recordings);
    scan->recordings = NULL;
    scan->recording_count = 0;

    if (previous_count == 0 && audio->recording_count > 0) {
        audio->selected_recording = 0;
    }

    return true;
}

void workbench_mark_clip_changed(WorkbenchAudio *audio) {
    audio->spectrum_dirty = true;
    audio->render_dirty = true;
    audio->render_count = 0;
    audio->playback_offset = audio->clip.trim_start;
}

void workbench_cycle_audition(WorkbenchAudio *audio) {
    audio->audition =
        (SoundAuditionTarget)(((int)audio->audition + 1) % (int)SOUND_AUDITION_COUNT);
}

bool workbench_cycle_recording(
    WorkbenchAudio *audio,
    int delta,
    SoundError *error
) {
    if (!audio || delta == 0 || audio->recording_count == 0) {
        return true;
    }

    int64_t count = (int64_t)audio->recording_count;
    int64_t index = (int64_t)audio->selected_recording + (int64_t)delta;

    while (index < 0) {
        index += count;
    }

    return select_recording(audio, (uint64_t)(index % count), error);
}

void workbench_cycle_band_method(WorkbenchAudio *audio) {
    audio->band_method = sound_band_render_method_offset(audio->band_method, 1);
    audio->render_dirty = true;
}

void workbench_cycle_band_handle(WorkbenchAudio *audio) {
    audio->upper_handle_selected = !audio->upper_handle_selected;
}

static void move_band_edge(
    WorkbenchAudio *audio,
    bool upper,
    int semitone_steps,
    double min_hz,
    double max_hz
) {
    if (semitone_steps == 0) {
        return;
    }

    double ratio = pow(2.0, (double)semitone_steps / 12.0);

    if (upper) {
        audio->high_hz = fmin(max_hz, fmax(audio->low_hz * 1.03, audio->high_hz * ratio));
    } else {
        audio->low_hz = fmax(min_hz, fmin(audio->high_hz / 1.03, audio->low_hz * ratio));
    }

    audio->render_dirty = true;
}

void workbench_move_selected_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
) {
    move_band_edge(audio, audio->upper_handle_selected, semitone_steps, min_hz, max_hz);
}

void workbench_move_lower_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
) {
    move_band_edge(audio, false, semitone_steps, min_hz, max_hz);
}

void workbench_move_upper_band_edge(
    WorkbenchAudio *audio,
    int semitone_steps,
    double min_hz,
    double max_hz
) {
    move_band_edge(audio, true, semitone_steps, min_hz, max_hz);
}

bool workbench_ensure_spectrum_rows(
    WorkbenchAudio *audio,
    uint64_t row_count,
    double sample_rate,
    double min_hz,
    double max_hz,
    SoundError *error
) {
    if (!sound_clip_has_audio(&audio->clip) || row_count == 0) {
        return true;
    }

    if (audio->spectrum_row_count != row_count) {
        float *rows = realloc(audio->spectrum_rows, sizeof(float) * (size_t)row_count);
        if (!rows) {
            sound_error_set(error, "could not allocate whole-spectrum rows");
            return false;
        }

        audio->spectrum_rows = rows;
        audio->spectrum_row_count = row_count;
        audio->spectrum_dirty = true;
    }

    if (!audio->spectrum_dirty) {
        return true;
    }

    if (!sound_offline_spectrum_db(
            sound_clip_samples(&audio->clip),
            sound_clip_sample_count(&audio->clip),
            sample_rate,
            min_hz,
            max_hz,
            audio->spectrum_rows,
            row_count,
            error
        )) {
        return false;
    }

    audio->spectrum_dirty = false;
    return true;
}

static bool ensure_render_buffers(
    WorkbenchAudio *audio,
    uint64_t sample_count,
    SoundError *error
) {
    if (audio->render_count == sample_count &&
        audio->selected_samples &&
        audio->rejected_samples) {
        return true;
    }

    if (sample_count == 0 || sample_count > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "invalid band render sample count");
        return false;
    }

    float *selected = realloc(audio->selected_samples, sizeof(float) * (size_t)sample_count);
    if (!selected) {
        sound_error_set(error, "could not allocate selected-band samples");
        return false;
    }
    audio->selected_samples = selected;

    float *rejected = realloc(audio->rejected_samples, sizeof(float) * (size_t)sample_count);
    if (!rejected) {
        sound_error_set(error, "could not allocate rejected-band samples");
        return false;
    }
    audio->rejected_samples = rejected;
    audio->render_count = sample_count;
    audio->render_dirty = true;
    return true;
}

bool workbench_ensure_band_render(WorkbenchAudio *audio, SoundError *error) {
    if (!sound_clip_has_audio(&audio->clip)) {
        return true;
    }

    const float *samples = sound_clip_samples(&audio->clip);
    uint64_t sample_count = sound_clip_sample_count(&audio->clip);

    if (!ensure_render_buffers(audio, sample_count, error)) {
        return false;
    }

    if (!audio->render_dirty) {
        return true;
    }

    SoundBandRenderRequest request = {
        .sample_rate = audio->clip.sample_rate,
        .low_hz = audio->low_hz,
        .high_hz = audio->high_hz,
        .iterations = 2,
    };

    if (!sound_band_render(
            samples,
            sample_count,
            &request,
            audio->band_method,
            audio->selected_samples,
            error
        )) {
        return false;
    }

    for (uint64_t i = 0; i < sample_count; ++i) {
        audio->rejected_samples[i] = samples[i] - audio->selected_samples[i];
    }

    audio->render_dirty = false;
    return true;
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

    sync_selected_recording_trim(audio);

    WorkbenchRecording recording;
    workbench_recording_init(&recording);

    if (path && path[0] != '\0') {
        (void)snprintf(recording.path, sizeof(recording.path), "%s", path);
        recording.disk_backed = true;
    }

    copy_recording_label(
        recording.summary.label,
        sizeof(recording.summary.label),
        path && path[0] != '\0' ? path : "new recording"
    );

    struct stat status;
    recording.created_at =
        path && path[0] != '\0' && stat(path, &status) == 0 ?
            file_created_at(&status) :
            (uint64_t)time(NULL);
    format_created_label(
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
    if (!insert_recording_sorted(audio, &recording, error, &inserted)) {
        workbench_recording_free(&recording);
        return false;
    }

    return select_recording(audio, inserted, error);
}

void workbench_apply_trim_event(WorkbenchAudio *audio, const SoundUiEvents *events) {
    if (!sound_clip_has_audio(&audio->clip)) {
        return;
    }

    uint64_t step = sample_count_for_seconds(audio->clip.sample_rate, trim_step_seconds);
    if (step == 0) {
        step = 1;
    }

    if (events->trim_start_delta > 0) {
        sound_clip_trim_start_by(&audio->clip, step * (uint64_t)events->trim_start_delta);
        sync_selected_recording_trim(audio);
        workbench_mark_clip_changed(audio);
    }

    if (events->trim_end_delta > 0) {
        sound_clip_trim_end_by(&audio->clip, step * (uint64_t)events->trim_end_delta);
        sync_selected_recording_trim(audio);
        workbench_mark_clip_changed(audio);
    }

    if (events->trim_clear) {
        sound_clip_clear_trim(&audio->clip);
        sync_selected_recording_trim(audio);
        workbench_mark_clip_changed(audio);
    }
}

bool workbench_toggle_playback(
    SoundPlayback *playback,
    WorkbenchAudio *audio,
    SoundWorkspace workspace,
    SoundError *error
) {
    if (sound_playback_is_playing(playback)) {
        return sound_playback_stop(playback, error);
    }

    if (!sound_clip_has_audio(&audio->clip) &&
        !workbench_ensure_selected_recording_loaded(audio, error)) {
        return false;
    }

    if (!sound_clip_has_audio(&audio->clip)) {
        return true;
    }

    const float *samples = sound_clip_samples(&audio->clip);
    uint64_t sample_count = sound_clip_sample_count(&audio->clip);

    if ((workspace == SOUND_WORKSPACE_BAND || workspace == SOUND_WORKSPACE_COMPARE) &&
        audio->audition != SOUND_AUDITION_ORIGINAL) {
        if (!workbench_ensure_band_render(audio, error)) {
            return false;
        }

        samples = audio->audition == SOUND_AUDITION_SELECTED ?
            audio->selected_samples :
            audio->rejected_samples;
        sample_count = audio->render_count;
    }

    if (!sound_playback_start(playback, samples, sample_count, audio->clip.sample_rate, error)) {
        return false;
    }

    audio->playback_offset = audio->clip.trim_start;
    return true;
}

SoundUiWorkbenchState workbench_ui_state(
    const WorkbenchAudio *audio,
    SoundWorkspace workspace,
    bool recording_enabled,
    bool playback_enabled,
    uint64_t playback_position
) {
    uint64_t playback_sample = audio->playback_offset + playback_position;
    if (playback_sample > audio->clip.sample_count) {
        playback_sample = audio->clip.sample_count;
    }

    double clip_seconds = 0.0;
    double active_seconds = 0.0;
    double trim_start_seconds = 0.0;
    double trim_end_seconds = 0.0;
    if (audio->clip.sample_rate > 0.0) {
        clip_seconds = (double)audio->clip.sample_count / audio->clip.sample_rate;
        active_seconds =
            (double)sound_clip_sample_count(&audio->clip) / audio->clip.sample_rate;
        trim_start_seconds = (double)audio->clip.trim_start / audio->clip.sample_rate;
        trim_end_seconds = (double)audio->clip.trim_end / audio->clip.sample_rate;
    }

    return (SoundUiWorkbenchState){
        .workspace = workspace,
        .clip_label = audio->clip.label,
        .method_label = sound_band_render_method_name(audio->band_method),
        .audition_label = audition_name(audio->audition),
        .recordings = audio->recording_count > 0 ? &audio->recordings[0].summary : NULL,
        .clip_seconds = clip_seconds,
        .active_seconds = active_seconds,
        .trim_start_seconds = trim_start_seconds,
        .trim_end_seconds = trim_end_seconds,
        .low_hz = audio->low_hz,
        .high_hz = audio->high_hz,
        .source_sample_count = audio->clip.sample_count,
        .trim_start_sample = audio->clip.trim_start,
        .trim_end_sample = audio->clip.trim_end,
        .playback_sample = playback_sample,
        .recording_count = audio->recording_count,
        .recording_index = audio->selected_recording,
        .recording_scan_complete = audio->recording_scan_complete,
        .recording_scan_failed = audio->recording_scan_failed,
        .recording_enabled = recording_enabled,
        .playback_enabled = playback_enabled,
        .playback_cursor_visible =
            playback_enabled && audio->clip.sample_count > 0,
        .has_clip = sound_clip_has_audio(&audio->clip),
        .upper_band_selected = audio->upper_handle_selected,
    };
}
