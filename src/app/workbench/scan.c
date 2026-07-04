#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    recording->created_at = workbench_file_created_at(status);
    recording->disk_backed = true;
    recording->loaded = false;
    workbench_copy_recording_label(
        recording->summary.label,
        sizeof(recording->summary.label),
        path
    );
    workbench_format_created_label(
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
        if (!workbench_insert_recording_sorted(audio, &scan->recordings[i], error, &inserted)) {
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
