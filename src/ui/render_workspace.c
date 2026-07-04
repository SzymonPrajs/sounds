#include "internal.h"

#include "font.h"

#include <math.h>
#include <stdio.h>

void sound_ui_clear_analysis_pane(SoundUi *ui) {
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
        "%s NEEDS AN ACTIVE CLIP",
        sound_workspace_name(state->workspace)
    );
    sound_ui_draw_panel_text(ui, line, 0, SOUND_UI_MENU_TITLE_COLOR);
    sound_ui_draw_panel_text(ui, "PRESS ENTER TO FREEZE RECENT LIVE AUDIO OR R TO RECORD", 2, SOUND_UI_AXIS_TEXT_COLOR);
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
    int top = ui->spectrogram_top + 52 * scale;
    int height = ui->spectrogram_height - 72 * scale;
    if (height < 24 * scale) {
        height = ui->spectrogram_height / 2;
    }

    char line[160];
    (void)snprintf(
        line,
        sizeof(line),
        "%s  %.2f S  TRIM %.2f / %.2f S",
        state->clip_label ? state->clip_label : "clip",
        state->clip_seconds,
        state->trim_start_seconds,
        state->trim_end_seconds
    );
    sound_ui_draw_panel_text(ui, line, 0, SOUND_UI_MENU_TITLE_COLOR);
    sound_ui_draw_panel_text(ui, "[P] PLAY  [,] TRIM START  [.] TRIM END  [/] CLEAR TRIM", 2, SOUND_UI_AXIS_TEXT_COLOR);
    sound_ui_draw_waveform_in_rect(ui, samples, sample_count, left, top, width, height, SOUND_UI_WAVEFORM_COLOR);
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

    sound_ui_draw_frequency_band_fill(ui, state->low_hz, state->high_hz, SOUND_UI_BAND_FILL_COLOR);

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
