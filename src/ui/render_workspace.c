#include "internal.h"

#include "font.h"

#include <math.h>
#include <stdio.h>

void sound_ui_clear_analysis_pane(SoundUi *ui) {
    ui->spectrogram_wrap_enabled = false;

    sound_ui_fill_rect(
        ui,
        0,
        ui->spectrogram_top,
        ui->width,
        ui->spectrogram_height,
        SOUND_UI_BACKGROUND_COLOR
    );
    sound_ui_draw_axis(ui);
}

int sound_ui_plot_width(const SoundUi *ui) {
    int width = ui->width - ui->spectrogram_left;
    return width > 0 ? width : 0;
}

static uint64_t clamp_sample(uint64_t sample, uint64_t sample_count) {
    return sample > sample_count ? sample_count : sample;
}

static int marker_x_for_sample(
    int left,
    int width,
    uint64_t sample,
    uint64_t sample_count
) {
    if (width <= 1 || sample_count == 0) {
        return left;
    }

    sample = clamp_sample(sample, sample_count);
    double unit = (double)sample / (double)sample_count;
    return left + (int)llround(unit * (double)(width - 1));
}

static void blend_rect(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height,
    uint32_t color,
    float amount
) {
    if (left < 0) {
        width += left;
        left = 0;
    }

    if (top < 0) {
        height += top;
        top = 0;
    }

    if (left + width > ui->width) {
        width = ui->width - left;
    }

    if (top + height > ui->height) {
        height = ui->height - top;
    }

    if (width <= 0 || height <= 0) {
        return;
    }

    for (int y = top; y < top + height; ++y) {
        uint32_t *row = sound_ui_row(ui, y);
        for (int x = left; x < left + width; ++x) {
            row[x] = sound_ui_blend_color(row[x], color, amount);
        }
    }
}

static void draw_vertical_marker(
    SoundUi *ui,
    int x,
    int top,
    int height,
    uint32_t color
) {
    int thickness = ui->text_scale > 1 ? ui->text_scale : 1;
    sound_ui_fill_rect(ui, x - thickness / 2, top, thickness, height, color);
}

static void draw_horizontal_marker(
    SoundUi *ui,
    int y,
    int left,
    int width,
    uint32_t color
) {
    int thickness = ui->text_scale > 1 ? ui->text_scale : 1;
    sound_ui_fill_rect(ui, left, y - thickness / 2, width, thickness, color);
}

static void draw_marker_label(
    SoundUi *ui,
    const char *text,
    int center_x,
    int y,
    int left,
    int right,
    uint32_t color
) {
    int scale = ui->text_scale;
    int text_width = sound_ui_text_width_pixels(text, scale);
    int x = center_x - text_width / 2;

    if (x < left) {
        x = left;
    }

    if (x + text_width > right) {
        x = right - text_width;
    }

    if (x < left) {
        x = left;
    }

    sound_ui_draw_text_scaled(ui, text, x, y, scale, color);
}

static void draw_clip_timeline_markers(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height,
    const SoundUiWorkbenchState *state
) {
    if (!state || state->source_sample_count == 0 || width <= 0 || height <= 0) {
        return;
    }

    uint64_t source_count = state->source_sample_count;
    uint64_t trim_start = clamp_sample(state->trim_start_sample, source_count);
    uint64_t trim_end = state->trim_end_sample == 0 ?
        source_count :
        clamp_sample(state->trim_end_sample, source_count);

    if (trim_end < trim_start) {
        trim_end = trim_start;
    }

    int right = left + width;
    int start_x = marker_x_for_sample(left, width, trim_start, source_count);
    int end_x = marker_x_for_sample(left, width, trim_end, source_count);

    if (start_x > left) {
        blend_rect(ui, left, top, start_x - left, height, SOUND_UI_BACKGROUND_COLOR, 0.62F);
    }

    if (end_x < right - 1) {
        blend_rect(
            ui,
            end_x + 1,
            top,
            right - end_x - 1,
            height,
            SOUND_UI_BACKGROUND_COLOR,
            0.62F
        );
    }

    draw_vertical_marker(ui, start_x, top, height, SOUND_UI_MARKER_COLOR);
    draw_vertical_marker(ui, end_x, top, height, SOUND_UI_MARKER_COLOR);

    int label_y = top + 3 * ui->text_scale;
    draw_marker_label(
        ui,
        "TRIM START",
        start_x,
        label_y,
        left,
        right,
        SOUND_UI_MARKER_COLOR
    );
    draw_marker_label(
        ui,
        "TRIM END",
        end_x,
        label_y + (SOUND_UI_GLYPH_HEIGHT + 4) * ui->text_scale,
        left,
        right,
        SOUND_UI_MARKER_COLOR
    );

    if (state->playback_cursor_visible) {
        int play_x =
            marker_x_for_sample(left, width, state->playback_sample, source_count);
        draw_vertical_marker(ui, play_x, top, height, SOUND_UI_PLAYHEAD_COLOR);
        draw_marker_label(
            ui,
            "PLAY",
            play_x,
            top + height - (SOUND_UI_GLYPH_HEIGHT + 5) * ui->text_scale,
            left,
            right,
            SOUND_UI_PLAYHEAD_COLOR
        );
    }
}

static void draw_active_playhead_marker(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height,
    const SoundUiWorkbenchState *state
) {
    if (!state || !state->playback_cursor_visible ||
        state->trim_end_sample <= state->trim_start_sample) {
        return;
    }

    uint64_t active_count = state->trim_end_sample - state->trim_start_sample;
    uint64_t position = 0;
    if (state->playback_sample > state->trim_start_sample) {
        position = state->playback_sample - state->trim_start_sample;
    }

    int x = marker_x_for_sample(left, width, position, active_count);
    draw_vertical_marker(ui, x, top, height, SOUND_UI_PLAYHEAD_COLOR);
}

static void draw_band_edge_markers(SoundUi *ui, const SoundUiWorkbenchState *state) {
    if (!state) {
        return;
    }

    int left = ui->spectrogram_left;
    int width = sound_ui_plot_width(ui);
    int scale = ui->text_scale;
    int high_row = sound_ui_spectrogram_row_for_frequency(ui, state->high_hz);
    int low_row = sound_ui_spectrogram_row_for_frequency(ui, state->low_hz);
    char high_label[80];
    char low_label[80];

    if (width <= 0) {
        return;
    }

    (void)snprintf(
        high_label,
        sizeof(high_label),
        state->upper_band_selected ? "[HIGH] %.0F HZ" : "HIGH %.0F HZ",
        state->high_hz
    );
    (void)snprintf(
        low_label,
        sizeof(low_label),
        state->upper_band_selected ? "LOW %.0F HZ" : "[LOW] %.0F HZ",
        state->low_hz
    );

    if (high_row >= 0) {
        int y = ui->spectrogram_top + high_row;
        draw_horizontal_marker(ui, y, left, width, SOUND_UI_MARKER_COLOR);
        sound_ui_draw_text_scaled(
            ui,
            high_label,
            left + 8 * scale,
            y + 3 * scale,
            scale,
            SOUND_UI_MARKER_COLOR
        );
    }

    if (low_row >= 0) {
        int y = ui->spectrogram_top + low_row;
        int label_y = y - (SOUND_UI_GLYPH_HEIGHT + 4) * scale;

        if (label_y < ui->spectrogram_top + 2 * scale) {
            label_y = y + 3 * scale;
        }

        draw_horizontal_marker(ui, y, left, width, SOUND_UI_MARKER_COLOR);
        sound_ui_draw_text_scaled(
            ui,
            low_label,
            left + 8 * scale,
            label_y,
            scale,
            SOUND_UI_MARKER_COLOR
        );
    }
}

void sound_ui_draw_panel_text(
    SoundUi *ui,
    const char *text,
    int line,
    uint32_t color
) {
    int scale = ui->text_scale;
    int y = ui->spectrogram_top + 12 * scale +
        line * (SOUND_UI_GLYPH_HEIGHT + 5) * scale;
    sound_ui_draw_text(ui, text, ui->spectrogram_left + 8 * scale, y, color);
}

void sound_ui_draw_waveform_in_rect(
    SoundUi *ui,
    const float *samples,
    uint64_t sample_count,
    int left,
    int top,
    int width,
    int height,
    uint32_t color
) {
    if (!samples || sample_count == 0 || width <= 0 || height <= 0) {
        return;
    }

    sound_ui_fill_rect(ui, left, top + height / 2, width, 1, SOUND_UI_MIDLINE_COLOR);

    double peak = 0.0;
    for (uint64_t i = 0; i < sample_count; ++i) {
        double value = fabs((double)samples[i]);
        if (value > peak) {
            peak = value;
        }
    }

    double gain = peak > 1.0e-7 ? fmin(0.90 / peak, 32.0) : 1.0;

    for (int x = 0; x < width; ++x) {
        uint64_t begin = (uint64_t)x * sample_count / (uint64_t)width;
        uint64_t end = (uint64_t)(x + 1) * sample_count / (uint64_t)width;
        if (end <= begin) {
            end = begin + 1;
        }
        if (end > sample_count) {
            end = sample_count;
        }

        double low = samples[begin];
        double high = samples[begin];
        for (uint64_t i = begin + 1; i < end; ++i) {
            if (samples[i] < low) {
                low = samples[i];
            }
            if (samples[i] > high) {
                high = samples[i];
            }
        }

        int y_high = sound_ui_waveform_y(high, gain, height);
        int y_low = sound_ui_waveform_y(low, gain, height);
        for (int y = y_high; y <= y_low; ++y) {
            sound_ui_row(ui, top + y)[left + x] = color;
        }
    }
}

void sound_ui_draw_empty_workspace(SoundUi *ui, const SoundUiWorkbenchState *state) {
    sound_ui_clear_analysis_pane(ui);

    char line[128];
    (void)snprintf(
        line,
        sizeof(line),
        "%s NEEDS A RECORDING",
        sound_workspace_name(state->workspace)
    );
    sound_ui_draw_panel_text(ui, line, 0, SOUND_UI_MENU_TITLE_COLOR);
    sound_ui_draw_panel_text(ui, "[R]/[ENTER] START OR STOP RECORDING", 2, SOUND_UI_AXIS_TEXT_COLOR);
    sound_ui_draw_panel_text(ui, "[TAB]/ARROWS SWITCH WORKSPACES   [M] MENU", 3, SOUND_UI_AXIS_TEXT_COLOR);
}

void sound_ui_draw_clip_workspace(
    SoundUi *ui,
    const float *samples,
    uint64_t sample_count,
    const SoundUiWorkbenchState *state
) {
    sound_ui_clear_analysis_pane(ui);

    int scale = ui->text_scale;
    int left = ui->spectrogram_left + 8 * scale;
    int width = ui->width - left - 8 * scale;
    int top = ui->spectrogram_top + 68 * scale;
    int height = ui->spectrogram_height - 88 * scale;
    if (height < 24 * scale) {
        height = ui->spectrogram_height / 2;
    }

    char line[160];
    (void)snprintf(
        line,
        sizeof(line),
        "RECORDING %llu/%llu  %s  FULL %.2F S  ACTIVE %.2F-%.2F S  %.2F S",
        (unsigned long long)(state->recording_index + 1U),
        (unsigned long long)state->recording_count,
        state->clip_label ? state->clip_label : "clip",
        state->clip_seconds,
        state->trim_start_seconds,
        state->trim_end_seconds,
        state->active_seconds
    );
    sound_ui_draw_panel_text(ui, line, 0, SOUND_UI_MENU_TITLE_COLOR);
    sound_ui_draw_panel_text(
        ui,
        "BRACKET KEYS [ AND ] CYCLE RECORDINGS   [P] PLAY ACTIVE REGION",
        2,
        SOUND_UI_AXIS_TEXT_COLOR
    );
    sound_ui_draw_panel_text(
        ui,
        "[,] MOVE START   [.] MOVE END   [/] CLEAR   WHITE TRIM LINES   YELLOW PLAYHEAD",
        3,
        SOUND_UI_AXIS_TEXT_COLOR
    );
    sound_ui_draw_waveform_in_rect(
        ui,
        samples,
        sample_count,
        left,
        top,
        width,
        height,
        SOUND_UI_WAVEFORM_COLOR
    );
    draw_clip_timeline_markers(ui, left, top, width, height, state);
}

void sound_ui_draw_spectrum_workspace(
    SoundUi *ui,
    const float *db_rows,
    uint64_t row_count,
    const SoundUiWorkbenchState *state
) {
    sound_ui_clear_analysis_pane(ui);

    if (!db_rows || row_count != (uint64_t)ui->spectrogram_height) {
        sound_ui_draw_empty_workspace(ui, state);
        return;
    }

    int left = ui->spectrogram_left;
    int width = sound_ui_plot_width(ui);
    if (width <= 0) {
        return;
    }

    if (state->workspace == SOUND_WORKSPACE_BAND) {
        sound_ui_draw_frequency_band_fill(
            ui,
            state->low_hz,
            state->high_hz,
            SOUND_UI_BAND_FILL_COLOR
        );
    }

    for (int y = 0; y < ui->spectrogram_height; ++y) {
        float db = db_rows[y];
        float unit = (db - sound_ui_floor_db) / (sound_ui_ceiling_db - sound_ui_floor_db);
        if (unit < 0.0F) {
            unit = 0.0F;
        } else if (unit > 1.0F) {
            unit = 1.0F;
        }

        int bar = (int)lrintf(unit * (float)(width - 1));
        uint32_t color = sound_ui_color_for_db(ui, db);
        uint32_t *pixels = sound_ui_row(ui, ui->spectrogram_top + y);

        for (int x = 0; x <= bar; ++x) {
            pixels[left + x] = color;
        }

        if (ui->grid_flags[y]) {
            for (int x = left; x < left + width; ++x) {
                pixels[x] = sound_ui_blend_color(pixels[x], SOUND_UI_GRIDLINE_COLOR, 0.18F);
            }
        }
    }

    char line[160];
    (void)snprintf(
        line,
        sizeof(line),
        "%s  BAND %.0F-%.0F HZ  %s",
        state->workspace == SOUND_WORKSPACE_BAND ? "BAND LAB" : "WHOLE SPECTRUM",
        state->low_hz,
        state->high_hz,
        state->method_label ? state->method_label : "FFT"
    );
    sound_ui_draw_panel_text(ui, line, 0, SOUND_UI_MENU_TITLE_COLOR);

    if (state->workspace == SOUND_WORKSPACE_BAND) {
        draw_band_edge_markers(ui, state);
        sound_ui_draw_panel_text(
            ui,
            "[H] SELECT LOW/HIGH HANDLE   [UP]/[DOWN] MOVE SELECTED",
            2,
            SOUND_UI_AXIS_TEXT_COLOR
        );
        sound_ui_draw_panel_text(
            ui,
            "LOW EDGE KEYS: [ AND ]   HIGH EDGE KEYS: - AND =",
            3,
            SOUND_UI_AXIS_TEXT_COLOR
        );
        sound_ui_draw_panel_text(
            ui,
            "[F] METHOD   [A] AUDITION   [P] PLAY   [X] COMPARE",
            4,
            SOUND_UI_AXIS_TEXT_COLOR
        );
    } else {
        sound_ui_draw_panel_text(
            ui,
            "[B] SELECT A BAND   [F] CYCLE BAND METHOD   [P] PLAY CLIP",
            2,
            SOUND_UI_AXIS_TEXT_COLOR
        );
    }
}

void sound_ui_draw_compare_workspace(
    SoundUi *ui,
    const float *source,
    uint64_t source_count,
    const float *rendered,
    uint64_t rendered_count,
    const SoundUiWorkbenchState *state
) {
    sound_ui_clear_analysis_pane(ui);

    int scale = ui->text_scale;
    int left = ui->spectrogram_left + 8 * scale;
    int width = ui->width - left - 8 * scale;
    int row_height = (ui->spectrogram_height - 48 * scale) / 2;
    if (row_height < 24 * scale) {
        row_height = 24 * scale;
    }

    sound_ui_draw_panel_text(ui, "COMPARE ORIGINAL / RENDERED", 0, SOUND_UI_MENU_TITLE_COLOR);
    sound_ui_draw_panel_text(ui, state->audition_label ? state->audition_label : "NO RENDER", 2, SOUND_UI_AXIS_TEXT_COLOR);
    sound_ui_draw_panel_text(ui, "[A] AUDITION ORIGINAL/SELECTED/REJECTED   [P] PLAY", 3, SOUND_UI_AXIS_TEXT_COLOR);
    sound_ui_draw_waveform_in_rect(
        ui,
        source,
        source_count,
        left,
        ui->spectrogram_top + 44 * scale,
        width,
        row_height,
        SOUND_UI_WAVEFORM_COLOR
    );
    draw_active_playhead_marker(
        ui,
        left,
        ui->spectrogram_top + 44 * scale,
        width,
        row_height,
        state
    );
    sound_ui_draw_waveform_in_rect(
        ui,
        rendered,
        rendered_count,
        left,
        ui->spectrogram_top + 52 * scale + row_height,
        width,
        row_height,
        SOUND_UI_RENDER_GREEN_COLOR
    );
    draw_active_playhead_marker(
        ui,
        left,
        ui->spectrogram_top + 52 * scale + row_height,
        width,
        row_height,
        state
    );
}

void sound_ui_draw_waveform(
    SoundUi *ui,
    const float *samples,
    uint64_t sample_count,
    double peak
) {
    int rows = ui->waveform_height;
    int waveform_top = ui->banner_height;

    sound_ui_fill_rows(ui, waveform_top, rows, SOUND_UI_BACKGROUND_COLOR);
    sound_ui_fill_rows(ui, waveform_top + rows / 2, 1, SOUND_UI_MIDLINE_COLOR);

    double gain = peak > 1.0e-7 ? fmin(40.0, 0.85 / peak) : 1.0;

    for (int x = 0; sample_count > 0 && x < ui->width; ++x) {
        uint64_t begin = (uint64_t)x * sample_count / (uint64_t)ui->width;
        uint64_t end = (uint64_t)(x + 1) * sample_count / (uint64_t)ui->width;

        if (end <= begin) {
            end = begin + 1;
        }

        if (end > sample_count) {
            end = sample_count;
        }

        double low = samples[begin];
        double high = samples[begin];

        for (uint64_t i = begin + 1; i < end; ++i) {
            if (samples[i] < low) {
                low = samples[i];
            }

            if (samples[i] > high) {
                high = samples[i];
            }
        }

        int top = sound_ui_waveform_y(high, gain, rows);
        int bottom = sound_ui_waveform_y(low, gain, rows);

        for (int y = top; y <= bottom; ++y) {
            sound_ui_row(ui, waveform_top + y)[x] = SOUND_UI_WAVEFORM_COLOR;
        }
    }
}
