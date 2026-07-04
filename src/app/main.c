/*
 * Sounds: a real-time microphone view in a window.
 *
 * main.c wires the app together. Capture writes into a ring buffer, the
 * analysis engine turns that audio into spectrogram columns, and the UI
 * module owns the window and all rendering.
 */

#include "sounds/analysis.h"
#include "sounds/analysis_engine.h"
#include "sounds/band_render.h"
#include "sounds/capture.h"
#include "sounds/clip.h"
#include "sounds/error.h"
#include "sounds/offline_spectrum.h"
#include "sounds/playback.h"
#include "sounds/recording.h"
#include "sounds/ring_buffer.h"
#include "sounds/settings.h"
#include "sounds/ui.h"
#include "sounds/workspace.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
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
static const double default_freeze_seconds = 5.0;
static const double trim_step_seconds = 0.25;

/*
 * The spectrogram advances on the audio clock, not the display clock:
 * 240 columns/s makes each column about 4 ms of signal.
 */
static const double spectrogram_columns_per_second = 240.0;

enum {
    minimum_waveform_samples = 1024,
};

typedef enum SoundAuditionTarget {
    SOUND_AUDITION_ORIGINAL,
    SOUND_AUDITION_SELECTED,
    SOUND_AUDITION_REJECTED,
    SOUND_AUDITION_COUNT,
} SoundAuditionTarget;

typedef struct WorkbenchAudio {
    SoundClip clip;
    float *spectrum_rows;
    uint64_t spectrum_row_count;
    float *selected_samples;
    float *rejected_samples;
    uint64_t render_count;
    double low_hz;
    double high_hz;
    SoundBandRenderMethod band_method;
    SoundAuditionTarget audition;
    bool upper_handle_selected;
    bool spectrum_dirty;
    bool render_dirty;
} WorkbenchAudio;

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

static SoundAuditionTarget next_audition(SoundAuditionTarget target) {
    return (SoundAuditionTarget)(((int)target + 1) % (int)SOUND_AUDITION_COUNT);
}

static void workbench_audio_init(WorkbenchAudio *audio) {
    sound_clip_init(&audio->clip);
    audio->spectrum_rows = NULL;
    audio->spectrum_row_count = 0;
    audio->selected_samples = NULL;
    audio->rejected_samples = NULL;
    audio->render_count = 0;
    audio->low_hz = 120.0;
    audio->high_hz = 1000.0;
    audio->band_method = SOUND_BAND_RENDER_FFT_MASK;
    audio->audition = SOUND_AUDITION_ORIGINAL;
    audio->upper_handle_selected = false;
    audio->spectrum_dirty = true;
    audio->render_dirty = true;
}

static void workbench_audio_free(WorkbenchAudio *audio) {
    sound_clip_free(&audio->clip);
    free(audio->spectrum_rows);
    free(audio->selected_samples);
    free(audio->rejected_samples);
}

static void mark_clip_changed(WorkbenchAudio *audio) {
    audio->spectrum_dirty = true;
    audio->render_dirty = true;
    audio->render_count = 0;
}

static bool ensure_spectrum_rows(
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

static bool ensure_band_render(
    WorkbenchAudio *audio,
    SoundError *error
) {
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

static bool freeze_recent_clip(
    WorkbenchAudio *audio,
    const SoundRingBuffer *ring,
    uint64_t written_samples,
    double sample_rate,
    const char *label,
    SoundError *error
) {
    uint64_t requested = sample_count_for_seconds(sample_rate, default_freeze_seconds);
    uint64_t available = written_samples < sound_ring_buffer_capacity(ring) ?
        written_samples :
        sound_ring_buffer_capacity(ring);

    if (available == 0) {
        return true;
    }

    if (requested > available) {
        requested = available;
    }

    if (!sound_clip_replace_from_ring(
            &audio->clip,
            ring,
            written_samples,
            requested,
            sample_rate,
            label,
            error
        )) {
        return false;
    }

    mark_clip_changed(audio);
    return true;
}

static bool replace_clip_from_recording(
    WorkbenchAudio *audio,
    const SoundRingBuffer *ring,
    uint64_t started_at,
    uint64_t ended_at,
    double sample_rate,
    SoundError *error
) {
    if (ended_at <= started_at) {
        return true;
    }

    if (!sound_clip_replace_from_ring(
            &audio->clip,
            ring,
            ended_at,
            ended_at - started_at,
            sample_rate,
            "recording",
            error
        )) {
        return false;
    }

    mark_clip_changed(audio);
    return true;
}

static void apply_trim_event(WorkbenchAudio *audio, const SoundUiEvents *events) {
    if (!sound_clip_has_audio(&audio->clip)) {
        return;
    }

    uint64_t step = sample_count_for_seconds(audio->clip.sample_rate, trim_step_seconds);
    if (step == 0) {
        step = 1;
    }

    if (events->trim_start_delta > 0) {
        sound_clip_trim_start_by(&audio->clip, step * (uint64_t)events->trim_start_delta);
        mark_clip_changed(audio);
    }

    if (events->trim_end_delta > 0) {
        sound_clip_trim_end_by(&audio->clip, step * (uint64_t)events->trim_end_delta);
        mark_clip_changed(audio);
    }

    if (events->trim_clear) {
        sound_clip_clear_trim(&audio->clip);
        mark_clip_changed(audio);
    }
}

static bool toggle_playback_for_state(
    SoundPlayback *playback,
    WorkbenchAudio *audio,
    SoundWorkspace workspace,
    SoundError *error
) {
    if (sound_playback_is_playing(playback)) {
        return sound_playback_stop(playback, error);
    }

    if (!sound_clip_has_audio(&audio->clip)) {
        return true;
    }

    const float *samples = sound_clip_samples(&audio->clip);
    uint64_t sample_count = sound_clip_sample_count(&audio->clip);

    if ((workspace == SOUND_WORKSPACE_BAND || workspace == SOUND_WORKSPACE_COMPARE) &&
        audio->audition != SOUND_AUDITION_ORIGINAL) {
        if (!ensure_band_render(audio, error)) {
            return false;
        }

        if (audio->audition == SOUND_AUDITION_SELECTED) {
            samples = audio->selected_samples;
        } else {
            samples = audio->rejected_samples;
        }
        sample_count = audio->render_count;
    }

    return sound_playback_start(
        playback,
        samples,
        sample_count,
        audio->clip.sample_rate,
        error
    );
}

static SoundUiWorkbenchState workbench_state(
    const WorkbenchAudio *audio,
    SoundWorkspace workspace,
    bool recording_enabled,
    bool playback_enabled
) {
    return (SoundUiWorkbenchState){
        .workspace = workspace,
        .clip_label = audio->clip.label,
        .method_label = sound_band_render_method_name(audio->band_method),
        .audition_label = audition_name(audio->audition),
        .clip_seconds = sound_clip_duration_seconds(&audio->clip),
        .trim_start_seconds = sound_clip_trim_start_seconds(&audio->clip),
        .trim_end_seconds = sound_clip_trim_end_seconds(&audio->clip),
        .low_hz = audio->low_hz,
        .high_hz = audio->high_hz,
        .recording_enabled = recording_enabled,
        .playback_enabled = playback_enabled,
        .has_clip = sound_clip_has_audio(&audio->clip),
    };
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
            workbench.audition = next_audition(workbench.audition);
        }

        if (events.cycle_band_method) {
            workbench.band_method =
                sound_band_render_method_offset(workbench.band_method, 1);
            workbench.render_dirty = true;
        }

        if (events.cycle_band_handle) {
            workbench.upper_handle_selected = !workbench.upper_handle_selected;
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
            !freeze_recent_clip(
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
                !replace_clip_from_recording(
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

        apply_trim_event(&workbench, &events);

        if (events.selected_band_delta != 0) {
            move_band_edge(
                &workbench,
                workbench.upper_handle_selected,
                events.selected_band_delta,
                sound_analysis_engine_min_frequency(engine),
                sound_analysis_engine_max_frequency(engine)
            );
        }

        if (events.lower_band_delta != 0) {
            move_band_edge(
                &workbench,
                false,
                events.lower_band_delta,
                sound_analysis_engine_min_frequency(engine),
                sound_analysis_engine_max_frequency(engine)
            );
        }

        if (events.upper_band_delta != 0) {
            move_band_edge(
                &workbench,
                true,
                events.upper_band_delta,
                sound_analysis_engine_min_frequency(engine),
                sound_analysis_engine_max_frequency(engine)
            );
        }

        if (events.toggle_playback &&
            !toggle_playback_for_state(playback, &workbench, workspace, &error)) {
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
        SoundUiWorkbenchState state = workbench_state(
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
                if (!ensure_spectrum_rows(
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
                    !ensure_band_render(&workbench, &error)) {
                    goto fail;
                }

                sound_ui_draw_spectrum_workspace(
                    ui,
                    workbench.spectrum_rows,
                    workbench.spectrum_row_count,
                    &state
                );
            } else if (workspace == SOUND_WORKSPACE_COMPARE) {
                if (!ensure_band_render(&workbench, &error)) {
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
