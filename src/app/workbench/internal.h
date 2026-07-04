#ifndef SOUNDS_APP_WORKBENCH_INTERNAL_H
#define SOUNDS_APP_WORKBENCH_INTERNAL_H

#include "../workbench.h"

#include <pthread.h>
#include <stddef.h>
#include <sys/stat.h>

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

void workbench_recording_init(WorkbenchRecording *recording);
void workbench_recording_free(WorkbenchRecording *recording);
void workbench_refresh_recording_summaries(WorkbenchAudio *audio);
void workbench_sync_selected_recording_trim(WorkbenchAudio *audio);
void workbench_copy_recording_label(
    char *label,
    size_t label_size,
    const char *path
);
void workbench_format_created_label(uint64_t created_at, char *text, size_t text_size);
uint64_t workbench_file_created_at(const struct stat *status);
bool workbench_insert_recording_sorted(
    WorkbenchAudio *audio,
    WorkbenchRecording *recording,
    SoundError *error,
    uint64_t *inserted_index
);
bool workbench_select_recording_index(
    WorkbenchAudio *audio,
    uint64_t index,
    SoundError *error
);
void workbench_clear_active_clip(WorkbenchAudio *audio);

#endif
