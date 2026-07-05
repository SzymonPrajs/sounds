#include "sounds/settings.h"

#include "sounds/defer.h"

#include <errno.h>
#include <math.h>
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
    int32_t legacy_recording_format;
    int32_t frequency_band;
    float custom_min_hz;
    float custom_max_hz;
    uint32_t reserved[5];
} SoundSettingsFile;

void sound_settings_defaults(SoundSettings *settings) {
    *settings = (SoundSettings){
        .mode = SOUND_APP_MODE_TONAL,
        .colormap = SOUND_COLORMAP_VIRIDIS,
        .frequency_band = SOUND_FREQUENCY_BAND_WHOLE,
        .custom_min_hz = SOUND_FREQUENCY_MID_MIN_HZ,
        .custom_max_hz = SOUND_FREQUENCY_MID_MAX_HZ,
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

static bool settings_file_is_valid(const SoundSettingsFile *file) {
    return memcmp(file->magic, settings_magic, sizeof(file->magic)) == 0 &&
        file->version == settings_version &&
        mode_is_valid((SoundAppMode)file->mode) &&
        colormap_is_valid((SoundColormap)file->colormap) &&
        sound_frequency_band_is_valid((SoundFrequencyBand)file->frequency_band);
}

static SoundSettingsFile settings_to_file(const SoundSettings *settings) {
    SoundSettings safe = *settings;

    if (!mode_is_valid(safe.mode)) {
        safe.mode = SOUND_APP_MODE_TONAL;
    }

    if (!colormap_is_valid(safe.colormap)) {
        safe.colormap = SOUND_COLORMAP_VIRIDIS;
    }

    if (!sound_frequency_band_is_valid(safe.frequency_band)) {
        safe.frequency_band = SOUND_FREQUENCY_BAND_WHOLE;
    }

    if (!isfinite(safe.custom_min_hz) ||
        !isfinite(safe.custom_max_hz) ||
        safe.custom_min_hz <= 0.0 ||
        safe.custom_max_hz <= safe.custom_min_hz) {
        safe.custom_min_hz = SOUND_FREQUENCY_MID_MIN_HZ;
        safe.custom_max_hz = SOUND_FREQUENCY_MID_MAX_HZ;
    }

    SoundSettingsFile file = {
        .version = settings_version,
        .mode = (int32_t)safe.mode,
        .colormap = (int32_t)safe.colormap,
        .legacy_recording_format = 0,
        .frequency_band = (int32_t)safe.frequency_band,
        .custom_min_hz = (float)safe.custom_min_hz,
        .custom_max_hz = (float)safe.custom_max_hz,
    };

    memcpy(file.magic, settings_magic, sizeof(file.magic));
    return file;
}

static void settings_from_file(const SoundSettingsFile *file, SoundSettings *settings) {
    *settings = (SoundSettings){
        .mode = (SoundAppMode)file->mode,
        .colormap = (SoundColormap)file->colormap,
        .frequency_band = (SoundFrequencyBand)file->frequency_band,
        .custom_min_hz = file->custom_min_hz,
        .custom_max_hz = file->custom_max_hz,
    };

    if (!isfinite(settings->custom_min_hz) ||
        !isfinite(settings->custom_max_hz) ||
        settings->custom_min_hz <= 0.0 ||
        settings->custom_max_hz <= settings->custom_min_hz) {
        settings->custom_min_hz = SOUND_FREQUENCY_MID_MIN_HZ;
        settings->custom_max_hz = SOUND_FREQUENCY_MID_MAX_HZ;
    }
}

bool sound_settings_save(const SoundSettings *settings, SoundError *error) {
    SoundSettingsFile file = settings_to_file(settings);
    bool ok = false;
    {
        FILE *stream = fopen(settings_path, "wb");

        if (!stream) {
            sound_error_set(error, "could not open %s", settings_path);
            return false;
        }

        defer {
            ok = fclose(stream) == 0 && ok;
        }

        ok = fwrite(&file, sizeof(file), 1, stream) == 1;
    }

    if (!ok) {
        sound_error_set(error, "could not write %s", settings_path);
        return false;
    }

    return true;
}

bool sound_settings_load(SoundSettings *settings, SoundError *error) {
    sound_settings_defaults(settings);

    SoundSettingsFile file;
    bool read_ok = false;
    bool closed = false;
    {
        FILE *stream = fopen(settings_path, "rb");
        if (!stream) {
            if (errno == ENOENT) {
                return sound_settings_save(settings, error);
            }

            sound_error_set(error, "could not open %s", settings_path);
            return false;
        }

        defer {
            closed = fclose(stream) == 0;
        }

        read_ok = fread(&file, sizeof(file), 1, stream) == 1;
    }

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
