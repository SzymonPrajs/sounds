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
#include "workbench.h"

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

static bool apply_ui_events(
    const SoundUiEvents *events,
    SoundAnalysisEngine *engine,
    SoundAppMode *mode,
    SoundSettings *settings,
    SoundError *error,
    bool *reset_spectrogram
) {
    bool settings_changed = false;

    if (events->mode_changed) {
        *mode = events->mode;
        settings->mode = *mode;
        sound_analysis_engine_set_mode(engine, *mode);
        *reset_spectrogram = true;
        settings_changed = true;
    }

    if (events->colormap_changed) {
        settings->colormap = events->colormap;
        *reset_spectrogram = true;
        settings_changed = true;
    }

    if (events->toggle_sst) {
        sound_analysis_engine_toggle_sst(engine);
        *reset_spectrogram = true;
    }

    if (settings_changed && !sound_settings_save(settings, error)) {
        return false;
    }

    return true;
}

static bool save_recording(
    const SoundRingBuffer *ring,
    uint64_t started_at,
    uint64_t ended_at,
    double sample_rate,
    SoundError *error
) {
    if (ended_at <= started_at) {
        return true;
    }

    uint64_t sample_count = 0;
    char recording_path[SOUND_RECORDING_PATH_CAPACITY];

    if (!sound_recording_save_recent(
            ring,
            ended_at - started_at,
            sample_rate,
            &sample_count,
            recording_path,
            sizeof(recording_path),
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

static bool toggle_recording(
    const SoundRingBuffer *ring,
    uint64_t written_samples,
    double sample_rate,
    bool *recording_enabled,
    uint64_t *recording_started_at,
    SoundError *error
) {
    if (!*recording_enabled) {
        *recording_enabled = true;
        *recording_started_at = written_samples;
        printf("recording started\n");
        return true;
    }

    if (!save_recording(
            ring,
            *recording_started_at,
            written_samples,
            sample_rate,
            error
        )) {
        return false;
    }

    *recording_enabled = false;
    *recording_started_at = written_samples;
    return true;
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
    SoundAppMode mode = SOUND_APP_MODE_TRANSIENT;
    SoundWorkspace workspace = SOUND_WORKSPACE_LIVE;
    WorkbenchAudio workbench;
    bool recording_enabled = false;
    uint64_t recording_started_at = 0;

    SoundError error;
    sound_error_clear(&error);
    workbench_audio_init(&workbench);

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
        .min_hz = sound_analysis_engine_min_frequency(engine),
        .max_hz = sound_analysis_engine_max_frequency(engine),
        .colormap = settings.colormap,
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
        sound_ui_poll_events(ui, mode, workspace, &events);
        if (events.quit) {
            break;
        }

        bool reset_spectrogram = false;
        if (!apply_ui_events(
                &events,
                engine,
                &mode,
                &settings,
                &error,
                &reset_spectrogram
            )) {
            goto fail;
        }

        if (events.workspace_changed) {
            workspace = events.workspace;
            if (workspace == SOUND_WORKSPACE_LIVE) {
                reset_spectrogram = true;
            }
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

        if (!sound_ui_sync(ui, &error)) {
            goto fail;
        }

        bool menu_open = sound_ui_menu_open(ui);
        bool menu_changed = menu_open != was_menu_open;
        if (was_menu_open && !menu_open) {
            reset_spectrogram = true;
        }

        uint64_t waveform_count =
            sound_ring_buffer_read_latest(ring, waveform, waveform_capacity);
        uint64_t written = sound_ring_buffer_written(ring);

        if (events.capture_clip &&
            !workbench_freeze_recent_clip(
                &workbench,
                ring,
                written,
                format.sample_rate,
                "recent live",
                &error
            )) {
            goto fail;
        }

        if (events.toggle_recording &&
            recording_enabled) {
            uint64_t started_at = recording_started_at;
            uint64_t ended_at = written;

            if (!toggle_recording(
                    ring,
                    written,
                    format.sample_rate,
                    &recording_enabled,
                    &recording_started_at,
                    &error
                ) ||
                !workbench_replace_clip_from_recording(
                    &workbench,
                    ring,
                    started_at,
                    ended_at,
                    format.sample_rate,
                    &error
                )) {
                goto fail;
            }
        } else if (events.toggle_recording &&
            !toggle_recording(
                ring,
                written,
                format.sample_rate,
                &recording_enabled,
                &recording_started_at,
                &error
            )) {
            goto fail;
        }

        workbench_apply_trim_event(&workbench, &events);

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

        if (reset_spectrogram) {
            sound_analysis_engine_reset_timeline(engine, written);
            sound_ui_clear_spectrogram(ui);
            columns_drawn = 0;
        }

        double rms = 0.0;
        double peak = 0.0;
        sound_compute_levels(waveform, waveform_count, &rms, &peak);

        bool sst_enabled = sound_analysis_engine_sst_enabled(engine);
        bool playback_enabled = sound_playback_is_playing(playback);
        const float *clip_samples = sound_clip_samples(&workbench.clip);
        uint64_t clip_sample_count = sound_clip_sample_count(&workbench.clip);
        SoundUiWorkbenchState state = workbench_ui_state(
            &workbench,
            workspace,
            recording_enabled,
            playback_enabled
        );

        if (!menu_open && workspace == SOUND_WORKSPACE_LIVE) {
            SoundAnalysisFrame analysis_frame;
            if (!sound_analysis_engine_update(
                    engine,
                    ring,
                    written,
                    ring_capacity,
                    sound_ui_spectrogram_rows(ui),
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
                workspace,
                sst_enabled,
                recording_enabled,
                playback_enabled
            );
        } else if (!menu_open) {
            double clip_peak = 0.0;
            double clip_rms = 0.0;
            sound_compute_levels(clip_samples, clip_sample_count, &clip_rms, &clip_peak);
            sound_ui_draw_waveform(ui, clip_samples, clip_sample_count, clip_peak);

            if (!sound_clip_has_audio(&workbench.clip)) {
                sound_ui_draw_empty_workspace(ui, &state);
            } else if (workspace == SOUND_WORKSPACE_CLIPS ||
                workspace == SOUND_WORKSPACE_SETTINGS) {
                sound_ui_draw_clip_workspace(ui, clip_samples, clip_sample_count, &state);
            } else if (workspace == SOUND_WORKSPACE_SPECTRUM ||
                workspace == SOUND_WORKSPACE_BAND) {
                if (!workbench_ensure_spectrum_rows(
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

                sound_ui_draw_spectrum_workspace(
                    ui,
                    workbench.spectrum_rows,
                    workbench.spectrum_row_count,
                    &state
                );
            } else if (workspace == SOUND_WORKSPACE_COMPARE) {
                if (!workbench_ensure_band_render(&workbench, &error)) {
                    goto fail;
                }

                sound_ui_draw_compare_workspace(
                    ui,
                    clip_samples,
                    clip_sample_count,
                    workbench.selected_samples,
                    workbench.render_count,
                    &state
                );
            }

            sound_ui_draw_banner(
                ui,
                mode,
                workspace,
                sst_enabled,
                recording_enabled,
                playback_enabled
            );
        }

        sound_ui_draw_menu(
            ui,
            mode,
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
            events.colormap_changed) {
            SoundUiTitle title = {
                .sample_rate = format.sample_rate,
                .rms = rms,
                .peak = peak,
                .min_hz = sound_analysis_engine_min_frequency(engine),
                .max_hz = sound_analysis_engine_max_frequency(engine),
                .mode = mode,
                .workspace = workspace,
                .colormap = sound_ui_colormap(ui),
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

        if (!save_recording(
                ring,
                recording_started_at,
                written,
                format.sample_rate,
                &error
            )) {
            fprintf(stderr, "%s\n", sound_error_message(&error));
            status = 1;
        }
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
