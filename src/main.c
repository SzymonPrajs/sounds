/*
 * Sounds: a real-time microphone view in a window.
 *
 * main.c wires the app together. Capture writes into a ring buffer, the
 * analysis engine turns that audio into spectrogram columns, and the UI
 * module owns the window and all rendering.
 */

#include "sounds/analysis.h"
#include "sounds/analysis_engine.h"
#include "sounds/capture.h"
#include "sounds/error.h"
#include "sounds/playback.h"
#include "sounds/recording.h"
#include "sounds/ring_buffer.h"
#include "sounds/settings.h"
#include "sounds/ui.h"
#include "sounds/workspace.h"
#include "app/workbench.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const char app_name[] = "Sounds";
static const char app_identifier[] = "dev.szymon.sounds";
static const char app_version[] = "1.0";

static const int initial_window_width = 1100;
static const int initial_window_height = 640;
static const int minimum_window_width = 480;
static const int minimum_window_height = 320;

static const double waveform_seconds = 0.08;
static const double raw_recording_seconds = 30.0;
static const double capture_buffer_seconds = 2.0;

/*
 * The spectrogram advances on the audio clock, not the display clock:
 * 240 columns/s makes each column about 4 ms of signal.
 */
static const double spectrogram_columns_per_second = 240.0;

enum {
    minimum_waveform_samples = 1024,
};

typedef struct RecordingSnapshot {
    float *samples;
    uint64_t sample_count;
} RecordingSnapshot;

static void write_ring_callback(
    const float *samples,
    uint32_t sample_count,
    void *user_data
) {
    sound_ring_buffer_write(user_data, samples, sample_count);
}

static uint64_t sample_count_for_seconds(double sample_rate, double seconds) {
    return (uint64_t)(sample_rate * seconds);
}

static uint64_t larger_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

static uint64_t waveform_capacity_for_rate(double sample_rate) {
    uint64_t capacity = sample_count_for_seconds(sample_rate, waveform_seconds);
    return larger_u64(capacity, minimum_waveform_samples);
}

static uint64_t ring_capacity_for_rate(double sample_rate) {
    uint64_t capture_capacity =
        sample_count_for_seconds(sample_rate, capture_buffer_seconds);
    uint64_t recording_capacity =
        sample_count_for_seconds(sample_rate, raw_recording_seconds);

    return larger_u64(capture_capacity, recording_capacity);
}

static double recording_elapsed_seconds(
    bool recording_enabled,
    uint64_t recording_started_at,
    uint64_t written_samples,
    double sample_rate
) {
    if (!recording_enabled ||
        written_samples <= recording_started_at ||
        sample_rate <= 0.0) {
        return 0.0;
    }

    return (double)(written_samples - recording_started_at) / sample_rate;
}

static bool apply_ui_events(
    const SoundUiEvents *events,
    SoundAnalysisEngine *engine,
    SoundAppMode *mode,
    SoundSettings *settings,
    uint64_t written_samples,
    SoundError *error,
    bool *recolor_spectrogram,
    bool *frequency_band_changed,
    bool *mark_spectrogram_transition
) {
    bool settings_changed = false;

    if (events->mode_changed) {
        *mode = events->mode;
        settings->mode = *mode;
        sound_analysis_engine_set_mode(engine, *mode);
        sound_analysis_engine_reset_mode_timeline(engine, *mode, written_samples);
        *mark_spectrogram_transition = true;
        settings_changed = true;
    }

    if (events->colormap_changed) {
        settings->colormap = events->colormap;
        *recolor_spectrogram = true;
        settings_changed = true;
    }

    if (events->custom_range_changed) {
        settings->custom_min_hz = events->custom_min_hz;
        settings->custom_max_hz = events->custom_max_hz;
        settings->frequency_band = SOUND_FREQUENCY_BAND_CUSTOM;
        settings_changed = true;
    } else if (events->frequency_band_changed) {
        settings->frequency_band = events->frequency_band;
        settings_changed = true;
    }

    if (events->frequency_band_changed || events->custom_range_changed) {
        if (!sound_analysis_engine_set_frequency_band(
                engine,
                settings->frequency_band,
                settings->custom_min_hz,
                settings->custom_max_hz,
                error
            )) {
            return false;
        }

        *frequency_band_changed = true;
    }

    if (events->toggle_sst) {
        sound_analysis_engine_toggle_sst(engine);
        if (*mode == SOUND_APP_MODE_TONAL) {
            sound_analysis_engine_reset_mode_timeline(engine, *mode, written_samples);
            *mark_spectrogram_transition = true;
        }
    }

    if (settings_changed && !sound_settings_save(settings, error)) {
        return false;
    }

    return true;
}

static void recording_snapshot_free(RecordingSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }

    free(snapshot->samples);
    snapshot->samples = NULL;
    snapshot->sample_count = 0;
}

static bool capture_recording_snapshot(
    const SoundRingBuffer *ring,
    uint64_t started_at,
    uint64_t ended_at,
    RecordingSnapshot *snapshot,
    SoundError *error
) {
    *snapshot = (RecordingSnapshot){0};

    if (ended_at <= started_at) {
        return true;
    }

    uint64_t capacity = sound_ring_buffer_capacity(ring);
    uint64_t requested = ended_at - started_at;
    uint64_t wanted = requested < capacity ? requested : capacity;

    if (wanted == 0) {
        return true;
    }

    if (wanted > (uint64_t)(SIZE_MAX / sizeof(float))) {
        sound_error_set(error, "recording is too large");
        return false;
    }

    float *samples = malloc(sizeof(float) * (size_t)wanted);
    if (!samples) {
        sound_error_set(error, "could not allocate recording snapshot");
        return false;
    }

    uint64_t count =
        sound_ring_buffer_read_available_ending_at(ring, ended_at, samples, wanted);
    if (count == 0) {
        free(samples);
        return true;
    }

    snapshot->samples = samples;
    snapshot->sample_count = count;
    return true;
}

static bool save_recording_snapshot(
    const RecordingSnapshot *snapshot,
    double sample_rate,
    char *recording_path,
    size_t recording_path_capacity,
    SoundError *error
) {
    if (recording_path && recording_path_capacity > 0) {
        recording_path[0] = '\0';
    }

    if (!snapshot || snapshot->sample_count == 0) {
        return true;
    }

    uint64_t sample_count = 0;

    if (!sound_recording_save_samples(
            snapshot->samples,
            snapshot->sample_count,
            sample_rate,
            &sample_count,
            recording_path,
            recording_path_capacity,
            error
        )) {
        return false;
    }

    if (sample_count > 0) {
        printf(
            "saved %" PRIu64 " recorded sample(s) to %s as %s\n",
            sample_count,
            recording_path,
            "32-bit float WAV"
        );
    }

    return true;
}

static void start_recording(
    uint64_t written_samples,
    bool *recording_enabled,
    uint64_t *recording_started_at
) {
    *recording_enabled = true;
    *recording_started_at = written_samples;
    printf("recording started\n");
}

static void draw_workbench_waveform(
    SoundUi *ui,
    const WorkbenchAudio *workbench,
    const SoundUiWorkbenchState *state
) {
    double rms = 0.0;
    double peak = 0.0;

    sound_compute_levels(
        workbench->clip.samples,
        workbench->clip.sample_count,
        &rms,
        &peak
    );
    (void)rms;

    sound_ui_draw_waveform(
        ui,
        workbench->clip.samples,
        workbench->clip.sample_count,
        peak
    );
    sound_ui_draw_waveform_timeline(ui, state);
}

int main(void) {
    int status = 1;
    SoundRingBuffer *ring = NULL;
    SoundInputStream *stream = NULL;
    SoundAnalysisEngine *engine = NULL;
    SoundPlayback *playback = NULL;
    SoundUi *ui = NULL;
    float *waveform = NULL;
    SoundSettings settings;
    SoundAppMode mode = SOUND_APP_MODE_TONAL;
    SoundWorkspace workspace = SOUND_WORKSPACE_LIVE;
    WorkbenchAudio workbench;
    bool recording_enabled = false;
    uint64_t recording_started_at = 0;

    SoundError error;
    sound_error_clear(&error);
    workbench_audio_init(&workbench);

    if (!workbench_start_recording_scan(&workbench, &error)) {
        goto fail;
    }

    if (!sound_settings_load(&settings, &error)) {
        goto fail;
    }
    mode = settings.mode;

    SoundInputFormat format;
    if (!sound_default_input_format(&format, &error)) {
        goto fail;
    }

    uint64_t ring_capacity = ring_capacity_for_rate(format.sample_rate);
    if (!sound_ring_buffer_create(ring_capacity, &ring, &error)) {
        goto fail;
    }

    SoundInputStreamOptions options = {
        .callback = write_ring_callback,
        .user_data = ring,
    };

    if (!sound_input_stream_open(&options, &stream, &format, &error)) {
        goto fail;
    }

    if (!sound_analysis_engine_create(
            format.sample_rate,
            spectrogram_columns_per_second,
            &engine,
            &error
        )) {
        goto fail;
    }
    sound_analysis_engine_set_mode(engine, mode);
    if (!sound_analysis_engine_set_frequency_band(
            engine,
            settings.frequency_band,
            settings.custom_min_hz,
            settings.custom_max_hz,
            &error
        )) {
        goto fail;
    }

    if (!sound_playback_open(&playback, &error)) {
        goto fail;
    }

    SoundUiConfig ui_config = {
        .app_name = app_name,
        .app_identifier = app_identifier,
        .app_version = app_version,
        .initial_width = initial_window_width,
        .initial_height = initial_window_height,
        .minimum_width = minimum_window_width,
        .minimum_height = minimum_window_height,
        .full_min_hz = sound_analysis_engine_full_min_frequency(engine),
        .full_max_hz = sound_analysis_engine_full_max_frequency(engine),
        .min_hz = sound_analysis_engine_min_frequency(engine),
        .max_hz = sound_analysis_engine_max_frequency(engine),
        .custom_min_hz = settings.custom_min_hz,
        .custom_max_hz = settings.custom_max_hz,
        .colormap = settings.colormap,
        .frequency_band = settings.frequency_band,
    };

    if (!sound_ui_create(&ui_config, &ui, &error)) {
        goto fail;
    }

    uint64_t waveform_capacity = waveform_capacity_for_rate(format.sample_rate);
    waveform = malloc(sizeof(float) * (size_t)waveform_capacity);
    if (!waveform) {
        sound_error_set(&error, "could not allocate waveform buffer");
        goto fail;
    }

    if (!sound_input_stream_start(stream, &error)) {
        goto fail;
    }

    uint64_t columns_drawn = 0;
    uint64_t frames_drawn = 0;
    bool was_menu_open = false;

    while (true) {
        SoundUiEvents events;
        sound_ui_poll_events(
            ui,
            mode,
            settings.frequency_band,
            workspace,
            workbench.recording_rename_active,
            &events
        );
        if (events.quit) {
            break;
        }

        if (!workbench_poll_recording_scan(&workbench, &error)) {
            goto fail;
        }

        uint64_t written = sound_ring_buffer_written(ring);
        bool recolor_spectrogram = false;
        bool frequency_band_changed = false;
        bool mark_spectrogram_transition = false;
        if (!apply_ui_events(
                &events,
                engine,
                &mode,
                &settings,
                written,
                &error,
                &recolor_spectrogram,
                &frequency_band_changed,
                &mark_spectrogram_transition
            )) {
            goto fail;
        }

        if (events.workspace_changed) {
            workspace = events.workspace;
            if (workspace == SOUND_WORKSPACE_LIVE) {
                sound_analysis_engine_reset_mode_timeline(engine, mode, written);
                mark_spectrogram_transition = true;
                recolor_spectrogram = true;
            }
        }

        if (frequency_band_changed) {
            sound_ui_set_custom_frequency_range(
                ui,
                settings.custom_min_hz,
                settings.custom_max_hz
            );
            sound_ui_set_frequency_band(
                ui,
                settings.frequency_band,
                sound_analysis_engine_min_frequency(engine),
                sound_analysis_engine_max_frequency(engine)
            );
        }

        if (events.cycle_audition) {
            workbench_cycle_audition(&workbench);
        }

        if (events.cycle_band_method) {
            workbench_cycle_band_method(&workbench);
        }

        if (events.cycle_band_handle) {
            workbench_cycle_band_handle(&workbench);
        }

        if (events.recording_delta != 0 &&
            !workbench_cycle_recording(&workbench, events.recording_delta, &error)) {
            goto fail;
        }

        if (events.select_recording &&
            !workbench_select_recording(&workbench, &error)) {
            goto fail;
        }

        if (events.delete_recording &&
            !workbench_delete_selected_recording(&workbench, &error)) {
            goto fail;
        }

        if (events.begin_recording_rename) {
            workbench_begin_recording_rename(&workbench);
        }

        if (events.cancel_recording_rename) {
            workbench_cancel_recording_rename(&workbench);
        }

        if (events.recording_rename_backspace) {
            workbench_recording_rename_backspace(&workbench);
        }

        if (events.recording_rename_text[0] != '\0') {
            workbench_append_recording_rename_text(
                &workbench,
                events.recording_rename_text
            );
        }

        if (events.commit_recording_rename &&
            !workbench_commit_recording_rename(&workbench, &error)) {
            goto fail;
        }

        if (!sound_ui_sync(ui, &error)) {
            goto fail;
        }

        bool menu_open = sound_ui_menu_open(ui);
        bool menu_changed = menu_open != was_menu_open;
        if (was_menu_open && !menu_open) {
            recolor_spectrogram = true;
        }

        uint64_t waveform_count =
            sound_ring_buffer_read_latest(ring, waveform, waveform_capacity);

        if (events.toggle_recording && recording_enabled) {
            uint64_t started_at = recording_started_at;
            uint64_t ended_at = sound_ring_buffer_written(ring);
            RecordingSnapshot snapshot;
            char recording_path[SOUND_RECORDING_PATH_CAPACITY];

            if (!capture_recording_snapshot(
                    ring,
                    started_at,
                    ended_at,
                    &snapshot,
                    &error
                )) {
                goto fail;
            }

            recording_enabled = false;
            recording_started_at = ended_at;

            if (!save_recording_snapshot(
                    &snapshot,
                    format.sample_rate,
                    recording_path,
                    sizeof(recording_path),
                    &error
                ) ||
                (snapshot.sample_count > 0 &&
                    !workbench_add_recording_from_samples(
                        &workbench,
                        snapshot.samples,
                        snapshot.sample_count,
                        format.sample_rate,
                        recording_path,
                        &error
                    ))) {
                recording_snapshot_free(&snapshot);
                goto fail;
            }

            recording_snapshot_free(&snapshot);
        } else if (events.toggle_recording) {
            start_recording(written, &recording_enabled, &recording_started_at);
        }

        workbench_apply_trim_event(&workbench, &events, &error);

        if (events.selected_band_delta != 0) {
            workbench_move_selected_band_edge(
                &workbench,
                events.selected_band_delta,
                sound_analysis_engine_min_frequency(engine),
                sound_analysis_engine_max_frequency(engine)
            );
        }

        if (events.lower_band_delta != 0) {
            workbench_move_lower_band_edge(
                &workbench,
                events.lower_band_delta,
                sound_analysis_engine_min_frequency(engine),
                sound_analysis_engine_max_frequency(engine)
            );
        }

        if (events.upper_band_delta != 0) {
            workbench_move_upper_band_edge(
                &workbench,
                events.upper_band_delta,
                sound_analysis_engine_min_frequency(engine),
                sound_analysis_engine_max_frequency(engine)
            );
        }

        if (events.toggle_playback &&
            !workbench_toggle_playback(playback, &workbench, workspace, &error)) {
            goto fail;
        }

        if (recolor_spectrogram) {
            sound_ui_recolor_spectrogram(ui);
        }

        if (mark_spectrogram_transition) {
            sound_ui_mark_spectrogram_transition(ui);
        }

        double rms = 0.0;
        double peak = 0.0;
        sound_compute_levels(waveform, waveform_count, &rms, &peak);

        bool sst_enabled = sound_analysis_engine_sst_enabled(engine);
        bool playback_enabled = sound_playback_is_playing(playback);
        uint64_t playback_position = sound_playback_position(playback);
        const float *clip_full_samples = workbench.clip.samples;
        uint64_t clip_full_sample_count = workbench.clip.sample_count;
        SoundUiWorkbenchState state = workbench_ui_state(
            &workbench,
            workspace,
            recording_enabled,
            playback_enabled,
            playback_position
        );

        if (!menu_open && workspace == SOUND_WORKSPACE_LIVE) {
            SoundAnalysisFrame analysis_frame;
            if (!sound_analysis_engine_update(
                    engine,
                    ring,
                    written,
                    ring_capacity,
                    sound_ui_spectrogram_rows(ui),
                    sound_ui_spectrogram_columns(ui),
                    &analysis_frame,
                    &error
                )) {
                goto fail;
            }

            sound_ui_draw_spectrogram_columns(
                ui,
                analysis_frame.columns,
                analysis_frame.column_count,
                analysis_frame.row_count,
                mode
            );
            columns_drawn += analysis_frame.column_count;

            sound_ui_draw_waveform(ui, waveform, waveform_count, peak);
            sound_ui_draw_banner(
                ui,
                mode,
                settings.frequency_band,
                workspace,
                sst_enabled,
                recording_enabled,
                recording_elapsed_seconds(
                    recording_enabled,
                    recording_started_at,
                    written,
                    format.sample_rate
                ),
                playback_enabled
            );
        } else if (!menu_open) {
            if (workspace == SOUND_WORKSPACE_CLIPS) {
                state = workbench_ui_state(
                    &workbench,
                    workspace,
                    recording_enabled,
                    playback_enabled,
                    playback_position
                );
                draw_workbench_waveform(ui, &workbench, &state);
                sound_ui_draw_recordings_workspace(ui, &state);
            } else if (workspace == SOUND_WORKSPACE_TRIM ||
                workspace == SOUND_WORKSPACE_SPECTRUM ||
                workspace == SOUND_WORKSPACE_BAND) {
                if (!workbench_ensure_selected_recording_loaded(&workbench, &error)) {
                    goto fail;
                }

                clip_full_samples = workbench.clip.samples;
                clip_full_sample_count = workbench.clip.sample_count;
                state = workbench_ui_state(
                    &workbench,
                    workspace,
                    recording_enabled,
                    playback_enabled,
                    playback_position
                );
                draw_workbench_waveform(ui, &workbench, &state);

                if (!sound_clip_has_audio(&workbench.clip)) {
                    sound_ui_draw_empty_workspace(ui, &state);
                } else {
                    if (!workbench_ensure_spectrum(
                            &workbench,
                            sound_ui_spectrogram_rows(ui),
                            workbench.clip.sample_rate,
                            sound_analysis_engine_min_frequency(engine),
                            sound_analysis_engine_max_frequency(engine),
                            &error
                        )) {
                        goto fail;
                    }

                    if (workspace == SOUND_WORKSPACE_BAND &&
                        !workbench_ensure_band_render(&workbench, &error)) {
                        goto fail;
                    }

                    if (workspace == SOUND_WORKSPACE_TRIM) {
                        sound_ui_draw_trim_workspace(
                            ui,
                            clip_full_samples,
                            clip_full_sample_count,
                            workbench.spectrum_cells,
                            workbench.spectrum_row_count,
                            &state
                        );
                    } else {
                        sound_ui_draw_spectrum_workspace(
                            ui,
                            workbench.spectrum_cells,
                            workbench.spectrum_row_count,
                            &state
                        );
                    }
                }
            }

            sound_ui_draw_banner(
                ui,
                mode,
                settings.frequency_band,
                workspace,
                sst_enabled,
                recording_enabled,
                recording_elapsed_seconds(
                    recording_enabled,
                    recording_started_at,
                    written,
                    format.sample_rate
                ),
                playback_enabled
            );
        }

        sound_ui_draw_menu(
            ui,
            mode,
            settings.frequency_band,
            workspace,
            sst_enabled,
            recording_enabled,
            playback_enabled
        );

        ++frames_drawn;

        if (frames_drawn % 30U == 0U ||
            (!menu_open && columns_drawn == 0U) ||
            menu_changed ||
            events.toggle_recording ||
            events.colormap_changed ||
            frequency_band_changed) {
            SoundUiTitle title = {
                .sample_rate = format.sample_rate,
                .rms = rms,
                .peak = peak,
                .min_hz = sound_analysis_engine_min_frequency(engine),
                .max_hz = sound_analysis_engine_max_frequency(engine),
                .mode = mode,
                .workspace = workspace,
                .colormap = sound_ui_colormap(ui),
                .frequency_band = sound_ui_frequency_band(ui),
                .sst_enabled = sst_enabled,
                .recording_enabled = recording_enabled,
                .playback_enabled = playback_enabled,
            };
            sound_ui_set_title(ui, &title);
        }

        sound_ui_present(ui);
        was_menu_open = menu_open;
    }

    status = 0;
    goto done;

fail:
    fprintf(stderr, "%s\n", sound_error_message(&error));

done:
    if (stream) {
        (void)sound_input_stream_stop(stream, &error);
    }

    if (playback) {
        (void)sound_playback_stop(playback, &error);
    }

    if (status == 0 && ring && recording_enabled) {
        uint64_t written = sound_ring_buffer_written(ring);
        RecordingSnapshot snapshot;
        char recording_path[SOUND_RECORDING_PATH_CAPACITY];

        if (!capture_recording_snapshot(
                ring,
                recording_started_at,
                written,
                &snapshot,
                &error
            ) ||
            !save_recording_snapshot(
                &snapshot,
                format.sample_rate,
                recording_path,
                sizeof(recording_path),
                &error
            )) {
            fprintf(stderr, "%s\n", sound_error_message(&error));
            status = 1;
        }

        recording_snapshot_free(&snapshot);
    }

    sound_ui_destroy(ui);
    sound_playback_close(playback);
    sound_analysis_engine_destroy(engine);
    sound_input_stream_close(stream);
    sound_ring_buffer_destroy(ring);
    workbench_audio_free(&workbench);
    free(waveform);
    return status;
}
