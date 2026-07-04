#include "sounds/settings.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char settings_path[] = "sounds.settings";
static const char settings_magic[8] = {'S', 'N', 'D', 'S', 'E', 'T', '1', '\0'};
static const uint32_t settings_version = 1;

typedef struct SoundSettingsFile {
    char magic[8];
    uint32_t version;
    int32_t mode;
    int32_t colormap;
    int32_t recording_format;
    uint32_t reserved[8];
} SoundSettingsFile;

void sound_settings_defaults(SoundSettings *settings) {
    *settings = (SoundSettings){
        .mode = SOUND_APP_MODE_TRANSIENT,
        .colormap = SOUND_COLORMAP_VIRIDIS,
        .recording_format = SOUND_RECORDING_WAV_FLOAT32,
    };
}

static bool mode_is_valid(SoundAppMode mode) {
    for (int i = 0; i < sound_app_mode_count(); ++i) {
        if (sound_app_mode_at(i) == mode) {
            return true;
        }
    }

    return false;
}

static bool colormap_is_valid(SoundColormap colormap) {
    for (int i = 0; i < sound_colormap_count(); ++i) {
        if (sound_colormap_at(i) == colormap) {
            return true;
        }
    }

    return false;
}

static bool recording_format_is_valid(SoundRecordingFormat format) {
    for (int i = 0; i < sound_recording_format_count(); ++i) {
        if (sound_recording_format_at(i) == format) {
            return true;
        }
    }

    return false;
}

static bool settings_file_is_valid(const SoundSettingsFile *file) {
    return memcmp(file->magic, settings_magic, sizeof(file->magic)) == 0 &&
        file->version == settings_version &&
        mode_is_valid((SoundAppMode)file->mode) &&
        colormap_is_valid((SoundColormap)file->colormap) &&
        recording_format_is_valid((SoundRecordingFormat)file->recording_format);
}

static SoundSettingsFile settings_to_file(const SoundSettings *settings) {
    SoundSettings safe = *settings;

    if (!mode_is_valid(safe.mode)) {
        safe.mode = SOUND_APP_MODE_TRANSIENT;
    }

    if (!colormap_is_valid(safe.colormap)) {
        safe.colormap = SOUND_COLORMAP_VIRIDIS;
    }

    if (!recording_format_is_valid(safe.recording_format)) {
        safe.recording_format = SOUND_RECORDING_WAV_FLOAT32;
    }

    SoundSettingsFile file = {
        .version = settings_version,
        .mode = (int32_t)safe.mode,
        .colormap = (int32_t)safe.colormap,
        .recording_format = (int32_t)safe.recording_format,
    };

    memcpy(file.magic, settings_magic, sizeof(file.magic));
    return file;
}

static void settings_from_file(const SoundSettingsFile *file, SoundSettings *settings) {
    *settings = (SoundSettings){
        .mode = (SoundAppMode)file->mode,
        .colormap = (SoundColormap)file->colormap,
        .recording_format = (SoundRecordingFormat)file->recording_format,
    };
}

bool sound_settings_save(const SoundSettings *settings, SoundError *error) {
    SoundSettingsFile file = settings_to_file(settings);
    FILE *stream = fopen(settings_path, "wb");

    if (!stream) {
        sound_error_set(error, "could not open %s", settings_path);
        return false;
    }

    bool ok = fwrite(&file, sizeof(file), 1, stream) == 1;
    bool closed = fclose(stream) == 0;

    if (!ok || !closed) {
        sound_error_set(error, "could not write %s", settings_path);
        return false;
    }

    return true;
}

bool sound_settings_load(SoundSettings *settings, SoundError *error) {
    sound_settings_defaults(settings);

    FILE *stream = fopen(settings_path, "rb");
    if (!stream) {
        if (errno == ENOENT) {
            return sound_settings_save(settings, error);
        }

        sound_error_set(error, "could not open %s", settings_path);
        return false;
    }

    SoundSettingsFile file;
    bool read_ok = fread(&file, sizeof(file), 1, stream) == 1;
    bool closed = fclose(stream) == 0;

    if (!closed) {
        sound_error_set(error, "could not close %s", settings_path);
        return false;
    }

    if (!read_ok || !settings_file_is_valid(&file)) {
        sound_settings_defaults(settings);
        return sound_settings_save(settings, error);
    }

    settings_from_file(&file, settings);
    return true;
}
