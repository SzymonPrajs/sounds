#include "internal.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void workbench_cycle_audition(WorkbenchAudio *audio) {
    audio->audition =
        (SoundAuditionTarget)(((int)audio->audition + 1) % (int)SOUND_AUDITION_COUNT);
}

bool workbench_cycle_recording(
    WorkbenchAudio *audio,
    int delta,
    SoundError *error
) {
    (void)error;

    if (!audio || delta == 0 || audio->recording_count == 0) {
        return true;
    }

    workbench_sync_selected_recording_trim(audio);

    int64_t count = (int64_t)audio->recording_count;
    int64_t index = (int64_t)audio->selected_recording + (int64_t)delta;

    while (index < 0) {
        index += count;
    }

    audio->selected_recording = (uint64_t)(index % count);
    audio->recording_delete_pending = false;
    return true;
}

bool workbench_select_recording(
    WorkbenchAudio *audio,
    SoundError *error
) {
    if (!audio || audio->recording_count == 0) {
        return true;
    }

    return workbench_select_recording_index(audio, audio->selected_recording, error);
}

void workbench_clear_active_clip(WorkbenchAudio *audio) {
    sound_clip_free(&audio->clip);
    sound_clip_init(&audio->clip);
    audio->has_active_recording = false;
    audio->active_recording = 0;
    audio->trim_editing = false;
    workbench_mark_clip_changed(audio);
}

static void update_index_after_delete(
    uint64_t deleted,
    uint64_t count,
    uint64_t *index,
    bool *valid
) {
    if (!*valid) {
        return;
    }

    if (*index == deleted) {
        *valid = false;
        *index = 0;
        return;
    }

    if (*index > deleted) {
        --*index;
    }

    if (*index >= count) {
        *valid = count > 0;
        *index = count > 0 ? count - 1U : 0;
    }
}

bool workbench_delete_selected_recording(
    WorkbenchAudio *audio,
    SoundError *error
) {
    if (!audio || audio->recording_count == 0 ||
        audio->selected_recording >= audio->recording_count) {
        return true;
    }

    if (!audio->recording_delete_pending ||
        audio->recording_delete_index != audio->selected_recording) {
        audio->recording_delete_pending = true;
        audio->recording_delete_index = audio->selected_recording;
        return true;
    }

    workbench_sync_selected_recording_trim(audio);

    uint64_t deleted = audio->selected_recording;
    WorkbenchRecording *recording = &audio->recordings[deleted];
    if (recording->disk_backed &&
        recording->path[0] != '\0' &&
        remove(recording->path) != 0) {
        sound_error_set(error, "could not delete %s", recording->path);
        return false;
    }

    bool active_valid = audio->has_active_recording;
    uint64_t active = audio->active_recording;

    workbench_recording_free(recording);
    uint64_t moved = audio->recording_count - deleted - 1U;
    if (moved > 0) {
        memmove(
            &audio->recordings[deleted],
            &audio->recordings[deleted + 1U],
            sizeof(*audio->recordings) * (size_t)moved
        );
        memmove(
            &audio->recording_summaries[deleted],
            &audio->recording_summaries[deleted + 1U],
            sizeof(*audio->recording_summaries) * (size_t)moved
        );
    }

    --audio->recording_count;
    workbench_recording_init(&audio->recordings[audio->recording_count]);
    audio->recording_summaries[audio->recording_count] = (SoundUiRecordingSummary){0};

    if (audio->recording_count == 0) {
        audio->selected_recording = 0;
        workbench_clear_active_clip(audio);
    } else {
        bool selected_valid = true;
        update_index_after_delete(
            deleted,
            audio->recording_count,
            &audio->selected_recording,
            &selected_valid
        );
        if (!selected_valid) {
            audio->selected_recording =
                deleted < audio->recording_count ? deleted : audio->recording_count - 1U;
        }

        update_index_after_delete(
            deleted,
            audio->recording_count,
            &active,
            &active_valid
        );
        audio->active_recording = active;
        audio->has_active_recording = active_valid;
        if (!active_valid) {
            workbench_clear_active_clip(audio);
        }
    }

    audio->recording_delete_pending = false;
    return true;
}

void workbench_cancel_recording_delete(WorkbenchAudio *audio) {
    if (!audio) {
        return;
    }

    audio->recording_delete_pending = false;
}

void workbench_begin_recording_rename(WorkbenchAudio *audio) {
    if (!audio || audio->recording_count == 0 ||
        audio->selected_recording >= audio->recording_count) {
        return;
    }

    workbench_sync_selected_recording_trim(audio);
    audio->recording_rename_active = true;
    audio->recording_delete_pending = false;
    (void)snprintf(
        audio->recording_rename_text,
        sizeof(audio->recording_rename_text),
        "%s",
        audio->recordings[audio->selected_recording].summary.label
    );
}

void workbench_cancel_recording_rename(WorkbenchAudio *audio) {
    if (!audio) {
        return;
    }

    audio->recording_rename_active = false;
    audio->recording_rename_text[0] = '\0';
}

void workbench_recording_rename_backspace(WorkbenchAudio *audio) {
    if (!audio || !audio->recording_rename_active) {
        return;
    }

    size_t length = strlen(audio->recording_rename_text);
    if (length > 0) {
        audio->recording_rename_text[length - 1U] = '\0';
    }
}

void workbench_append_recording_rename_text(WorkbenchAudio *audio, const char *text) {
    if (!audio || !audio->recording_rename_active || !text) {
        return;
    }

    size_t length = strlen(audio->recording_rename_text);
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        unsigned char value = (unsigned char)*cursor;
        if (value < 32U || value > 126U ||
            length + 1U >= sizeof(audio->recording_rename_text)) {
            continue;
        }

        audio->recording_rename_text[length] = (char)value;
        ++length;
        audio->recording_rename_text[length] = '\0';
    }
}

void workbench_set_recording_rename_text(WorkbenchAudio *audio, const char *text) {
    if (!audio || !audio->recording_rename_active) {
        return;
    }

    audio->recording_rename_text[0] = '\0';
    workbench_append_recording_rename_text(audio, text);
}

static void sanitized_recording_stem(
    const char *input,
    char *stem,
    size_t stem_size
) {
    size_t length = 0;

    for (const char *cursor = input; cursor && *cursor != '\0'; ++cursor) {
        unsigned char value = (unsigned char)*cursor;
        char output = '\0';

        if (isalnum(value)) {
            output = (char)value;
        } else if (value == '-' || value == '_' || value == ' ') {
            output = value == ' ' ? '-' : (char)value;
        }

        if (output == '\0') {
            continue;
        }

        if (length > 0 && output == '-' && stem[length - 1U] == '-') {
            continue;
        }

        if (length + 1U >= stem_size) {
            break;
        }

        stem[length] = output;
        ++length;
    }

    while (length > 0 && stem[length - 1U] == '-') {
        --length;
    }

    if (length == 0) {
        (void)snprintf(stem, stem_size, "recording");
    } else {
        stem[length] = '\0';
    }
}

static bool build_renamed_recording_path(
    const char *stem,
    char *path,
    size_t path_size,
    SoundError *error
) {
    int written = snprintf(
        path,
        path_size,
        "%s/%s.wav",
        sound_recording_directory(),
        stem
    );

    if (written <= 0 || (size_t)written >= path_size) {
        sound_error_set(error, "recording name is too long");
        return false;
    }

    return true;
}

bool workbench_commit_recording_rename(
    WorkbenchAudio *audio,
    SoundError *error
) {
    if (!audio || !audio->recording_rename_active) {
        return true;
    }

    if (audio->recording_count == 0 ||
        audio->selected_recording >= audio->recording_count) {
        workbench_cancel_recording_rename(audio);
        return true;
    }

    WorkbenchRecording *recording = &audio->recordings[audio->selected_recording];
    char stem[SOUND_UI_RECORDING_LABEL_CAPACITY];
    char path[SOUND_RECORDING_PATH_CAPACITY];
    sanitized_recording_stem(
        audio->recording_rename_text,
        stem,
        sizeof(stem)
    );

    if (!build_renamed_recording_path(stem, path, sizeof(path), error)) {
        return false;
    }

    if (recording->disk_backed && recording->path[0] != '\0') {
        if (strcmp(recording->path, path) != 0) {
            struct stat status;
            if (stat(path, &status) == 0) {
                sound_error_set(error, "%s already exists", path);
                return false;
            }

            if (rename(recording->path, path) != 0) {
                sound_error_set(error, "could not rename %s", recording->path);
                return false;
            }
        }

        (void)snprintf(recording->path, sizeof(recording->path), "%s", path);
    }

    (void)snprintf(recording->summary.label, sizeof(recording->summary.label), "%s", stem);
    if (recording->loaded) {
        (void)snprintf(recording->clip.label, sizeof(recording->clip.label), "%s", stem);
    }
    if (audio->has_active_recording &&
        audio->active_recording == audio->selected_recording) {
        (void)snprintf(audio->clip.label, sizeof(audio->clip.label), "%s", stem);
    }

    workbench_refresh_recording_summaries(audio);
    workbench_cancel_recording_rename(audio);
    return true;
}
