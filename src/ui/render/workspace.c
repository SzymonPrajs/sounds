#include "../internal.h"

#include "../font.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static void clear_plain_workspace_pane(SoundUi *ui) {
    ui->spectrogram_wrap_enabled = false;
    sound_ui_fill_rect(
        ui,
        0,
        ui->spectrogram_top,
        ui->width,
        ui->spectrogram_height,
        SOUND_UI_BACKGROUND_COLOR
    );
}

static void clear_workspace_below_toolbar(SoundUi *ui) {
    ui->spectrogram_wrap_enabled = false;
    sound_ui_fill_rect(
        ui,
        0,
        ui->banner_height,
        ui->width,
        ui->height - ui->banner_height,
        SOUND_UI_BACKGROUND_COLOR
    );
}

static void begin_non_menu_imui_frame(SoundUi *ui, int scale) {
    sound_imui_set_draw(
        &ui->imui,
        sound_imui_sdl_draw(&ui->imui_adapter, ui)
    );
    sound_imui_set_text_scale(&ui->imui, scale);
    sound_imui_begin(&ui->imui, ui->menu_open ? NULL : &ui->imui_input);
}

static bool imui_rect_contains(SoundImuiRect rect, int x, int y) {
    return rect.width > 0 &&
        rect.height > 0 &&
        x >= rect.x &&
        y >= rect.y &&
        x < rect.x + rect.width &&
        y < rect.y + rect.height;
}

int sound_ui_plot_width(const SoundUi *ui) {
    int width = ui->width - ui->spectrogram_left;
    return width > 0 ? width : 0;
}

/* Layout minimums can push pane rects past the surface on short windows. */
static bool clamp_pane_to_surface(
    const SoundUi *ui,
    int left,
    int top,
    int *width,
    int *height
) {
    if (left < 0 || top < 0 || left >= ui->width || top >= ui->height) {
        return false;
    }

    if (left + *width > ui->width) {
        *width = ui->width - left;
    }

    if (top + *height > ui->height) {
        *height = ui->height - top;
    }

    return *width > 0 && *height > 0;
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
    uint64_t trim_start = clamp_sample(state->draft_trim_start_sample, source_count);
    uint64_t trim_end = state->draft_trim_end_sample == 0 ?
        source_count :
        clamp_sample(state->draft_trim_end_sample, source_count);

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

    uint32_t start_color = !state->trim_end_selected ?
        SOUND_UI_MARKER_COLOR :
        SOUND_UI_MARKER_DIM_COLOR;
    uint32_t end_color = state->trim_end_selected ?
        SOUND_UI_MARKER_COLOR :
        SOUND_UI_MARKER_DIM_COLOR;

    draw_vertical_marker(ui, start_x, top, height, start_color);
    draw_vertical_marker(ui, end_x, top, height, end_color);

    int label_y = top + 3 * ui->text_scale;
    draw_marker_label(
        ui,
        state->trim_editing && !state->trim_end_selected ? "[TRIM START]" : "TRIM START",
        start_x,
        label_y,
        left,
        right,
        start_color
    );
    draw_marker_label(
        ui,
        state->trim_editing && state->trim_end_selected ? "[TRIM END]" : "TRIM END",
        end_x,
        label_y + (SOUND_UI_GLYPH_HEIGHT + 4) * ui->text_scale,
        left,
        right,
        end_color
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

static double seconds_for_source_sample(
    const SoundUiWorkbenchState *state,
    uint64_t sample
) {
    if (!state || state->source_sample_count == 0 || state->clip_seconds <= 0.0) {
        return 0.0;
    }

    return (double)sample *
        state->clip_seconds /
        (double)state->source_sample_count;
}

static void draw_trim_timeline_markers(
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
    uint64_t trim_start = clamp_sample(state->draft_trim_start_sample, source_count);
    uint64_t trim_end = state->draft_trim_end_sample == 0 ?
        source_count :
        clamp_sample(state->draft_trim_end_sample, source_count);

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

    uint32_t start_color = !state->trim_end_selected ?
        SOUND_UI_MARKER_COLOR :
        SOUND_UI_MARKER_DIM_COLOR;
    uint32_t end_color = state->trim_end_selected ?
        SOUND_UI_MARKER_COLOR :
        SOUND_UI_MARKER_DIM_COLOR;
    char start_label[48];
    char end_label[48];

    (void)snprintf(
        start_label,
        sizeof(start_label),
        "START %.3F S",
        seconds_for_source_sample(state, trim_start)
    );
    (void)snprintf(
        end_label,
        sizeof(end_label),
        "END %.3F S",
        seconds_for_source_sample(state, trim_end)
    );

    draw_vertical_marker(ui, start_x, top, height, start_color);
    draw_vertical_marker(ui, end_x, top, height, end_color);

    int label_y = top + 3 * ui->text_scale;
    draw_marker_label(ui, start_label, start_x, label_y, left, right, start_color);
    draw_marker_label(
        ui,
        end_label,
        end_x,
        label_y + (SOUND_UI_GLYPH_HEIGHT + 4) * ui->text_scale,
        left,
        right,
        end_color
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

void sound_ui_draw_waveform_timeline(
    SoundUi *ui,
    const SoundUiWorkbenchState *state
) {
    if (!state || state->source_sample_count == 0) {
        return;
    }

    draw_clip_timeline_markers(
        ui,
        0,
        ui->banner_height,
        ui->width,
        ui->waveform_height,
        state
    );
}

static float db_unit(float db) {
    float unit = (db - sound_ui_floor_db) / (sound_ui_ceiling_db - sound_ui_floor_db);

    if (unit < 0.0F) {
        return 0.0F;
    }

    if (unit > 1.0F) {
        return 1.0F;
    }

    return unit;
}

static uint64_t source_row_for_plot_y(int y, int height, uint64_t row_count) {
    if (height <= 0 || row_count == 0U) {
        return 0;
    }

    uint64_t row = (uint64_t)y * row_count / (uint64_t)height;
    return row < row_count ? row : row_count - 1U;
}

static void draw_offline_spectrogram_in_rect(
    SoundUi *ui,
    const float *cells,
    uint64_t column_count,
    uint64_t row_count,
    int left,
    int top,
    int width,
    int height
) {
    if (!cells ||
        column_count == 0U ||
        row_count == 0U ||
        width <= 0 ||
        height <= 0 ||
        !clamp_pane_to_surface(ui, left, top, &width, &height)) {
        return;
    }

    sound_ui_mark_dirty_rect(ui, left, top, width, height);

    for (int y = 0; y < height; ++y) {
        uint64_t source_row = source_row_for_plot_y(y, height, row_count);
        uint32_t *pixels = sound_ui_row(ui, top + y);
        bool gridline = sound_ui_spectrogram_gridline_for_row(
            ui,
            (int)source_row,
            (int)row_count
        );

        for (int x = 0; x < width; ++x) {
            uint64_t source_column =
                (uint64_t)x * column_count / (uint64_t)width;
            if (source_column >= column_count) {
                source_column = column_count - 1U;
            }

            float db = cells[source_column * row_count + source_row];
            uint32_t color = sound_ui_color_for_db(ui, db);
            if (gridline) {
                color = sound_ui_blend_color(color, SOUND_UI_GRIDLINE_COLOR, 0.25F);
            }

            pixels[left + x] = color;
        }
    }
}

static void draw_profile_bars_in_rect(
    SoundUi *ui,
    const float *db_rows,
    uint64_t row_count,
    int left,
    int top,
    int width,
    int height
) {
    if (!db_rows ||
        row_count == 0U ||
        width <= 0 ||
        height <= 0 ||
        !clamp_pane_to_surface(ui, left, top, &width, &height)) {
        return;
    }

    sound_ui_mark_dirty_rect(ui, left, top, width, height);
    sound_ui_fill_rect(ui, left, top, width, height, SOUND_UI_AXIS_BACKGROUND_COLOR);

    for (int y = 0; y < height; ++y) {
        uint64_t source_row = source_row_for_plot_y(y, height, row_count);
        float db = db_rows[source_row];
        int bar_width = (int)llround((double)db_unit(db) * (double)width);
        uint32_t color = sound_ui_color_for_db(ui, db);
        uint32_t *pixels = sound_ui_row(ui, top + y);
        bool gridline = sound_ui_spectrogram_gridline_for_row(
            ui,
            (int)source_row,
            (int)row_count
        );

        if (bar_width < 0) {
            bar_width = 0;
        }
        if (bar_width > width) {
            bar_width = width;
        }

        for (int x = 0; x < bar_width; ++x) {
            pixels[left + x] = color;
        }

        if (gridline) {
            for (int x = 0; x < width; ++x) {
                pixels[left + x] = sound_ui_blend_color(
                    pixels[left + x],
                    SOUND_UI_GRIDLINE_COLOR,
                    0.25F
                );
            }
        }
    }
}

static int clamped_spectrogram_row_for_height(
    const SoundUi *ui,
    double hz,
    int height
) {
    int row = sound_ui_spectrogram_row_for_height(ui, hz, height);
    if (row >= 0 || height <= 0) {
        return row;
    }

    if (hz >= ui->max_hz) {
        return 0;
    }

    if (hz <= ui->min_hz) {
        return height - 1;
    }

    return -1;
}

static void dim_band_spectrogram_outside(
    SoundUi *ui,
    const SoundUiWorkbenchState *state,
    int left,
    int top,
    int width,
    int height
) {
    int high_row = clamped_spectrogram_row_for_height(ui, state->high_hz, height);
    int low_row = clamped_spectrogram_row_for_height(ui, state->low_hz, height);

    if (high_row < 0 || low_row < 0) {
        return;
    }

    int keep_top = high_row < low_row ? high_row : low_row;
    int keep_bottom = high_row > low_row ? high_row : low_row;

    if (keep_top > 0) {
        blend_rect(
            ui,
            left,
            top,
            width,
            keep_top,
            SOUND_UI_BACKGROUND_COLOR,
            0.35F
        );
    }

    if (keep_bottom + 1 < height) {
        blend_rect(
            ui,
            left,
            top + keep_bottom + 1,
            width,
            height - keep_bottom - 1,
            SOUND_UI_BACKGROUND_COLOR,
            0.35F
        );
    }
}

static void draw_band_edge_marker(
    SoundUi *ui,
    const char *label,
    int row,
    bool label_below,
    int left,
    int top,
    int width,
    int height,
    uint32_t color
) {
    if (row < 0 || row >= height) {
        return;
    }

    int scale = ui->text_scale;
    int y = top + row;
    int label_y = label_below ?
        y + 3 * scale :
        y - (SOUND_UI_GLYPH_HEIGHT + 4) * scale;

    if (label_y < top + 2 * scale) {
        label_y = y + 3 * scale;
    }

    int text_height = SOUND_UI_GLYPH_HEIGHT * scale;
    if (label_y + text_height > top + height - 2 * scale) {
        label_y = y - (SOUND_UI_GLYPH_HEIGHT + 4) * scale;
    }
    if (label_y < top + 2 * scale) {
        label_y = top + 2 * scale;
    }

    draw_horizontal_marker(ui, y, left, width, color);
    sound_ui_draw_text_scaled(
        ui,
        label,
        left + 8 * scale,
        label_y,
        scale,
        color
    );
}

static void draw_band_edge_markers(
    SoundUi *ui,
    const SoundUiWorkbenchState *state,
    int left,
    int top,
    int width,
    int height
) {
    if (!state || width <= 0 || height <= 0) {
        return;
    }

    int high_row = clamped_spectrogram_row_for_height(ui, state->high_hz, height);
    int low_row = clamped_spectrogram_row_for_height(ui, state->low_hz, height);
    char high_label[80];
    char low_label[80];
    uint32_t high_color = state->upper_band_selected ?
        SOUND_UI_MARKER_COLOR :
        SOUND_UI_MARKER_DIM_COLOR;
    uint32_t low_color = state->upper_band_selected ?
        SOUND_UI_MARKER_DIM_COLOR :
        SOUND_UI_MARKER_COLOR;

    (void)snprintf(high_label, sizeof(high_label), "HIGH %.0F HZ", state->high_hz);
    (void)snprintf(low_label, sizeof(low_label), "LOW %.0F HZ", state->low_hz);

    draw_band_edge_marker(
        ui,
        high_label,
        high_row,
        true,
        left,
        top,
        width,
        height,
        high_color
    );
    draw_band_edge_marker(
        ui,
        low_label,
        low_row,
        false,
        left,
        top,
        width,
        height,
        low_color
    );
}

static void update_band_edge_drag(
    SoundUi *ui,
    const char *name,
    bool upper,
    int row,
    int left,
    int top,
    int width,
    int height
) {
    if (row < 0 || row >= height || width <= 0 || height <= 0) {
        return;
    }

    int strip_height = 8 * ui->text_scale;
    if (strip_height < 4) {
        strip_height = 4;
    }

    bool active = false;
    SoundImuiRect grab = sound_imui_rect(
        left,
        top + row - strip_height / 2,
        width,
        strip_height
    );
    (void)sound_imui_hit_rect(&ui->imui, name, grab, NULL, &active);

    const SoundImuiInput *input = ui->imui.input;
    if (input && active && (input->mouse_left_down || input->mouse_left_pressed)) {
        ui->pending_ui_events.band_set_edge = true;
        ui->pending_ui_events.band_set_edge_upper = upper;
        ui->pending_ui_events.band_set_hz =
            sound_ui_spectrogram_frequency_for_row_in_height(
                ui,
                input->mouse_y - top,
                height
            );
    }
}

static void draw_centered_workspace_message(
    SoundUi *ui,
    int top,
    int height,
    const char *text
) {
    int scale = ui->text_scale;
    int text_width = sound_ui_text_width_pixels(text, scale);
    int text_height = SOUND_UI_GLYPH_HEIGHT * scale;
    int x = (ui->width - text_width) / 2;
    int y = top + (height - text_height) / 2;

    if (x < scale) {
        x = scale;
    }
    if (y < top + scale) {
        y = top + scale;
    }

    sound_ui_draw_text_scaled(ui, text, x, y, scale, SOUND_UI_AXIS_TEXT_COLOR);
}

static int advance_band_readout(
    SoundUi *ui,
    const char *text,
    int x,
    int y,
    int height
) {
    int scale = ui->text_scale;
    int text_y = y + (height - SOUND_UI_GLYPH_HEIGHT * scale) / 2;
    sound_ui_draw_text_scaled(ui, text, x, text_y, scale, SOUND_UI_AXIS_TEXT_COLOR);
    return x + sound_ui_text_width_pixels(text, scale) + 4 * scale;
}

static bool band_control_button(
    SoundUi *ui,
    const char *name,
    const char *label,
    int *x,
    int y,
    int height
) {
    int scale = ui->text_scale;
    int width = sound_ui_text_width_pixels(label, scale) + 14 * scale;
    bool clicked = sound_imui_button_rect_id(
        &ui->imui,
        name,
        label,
        sound_imui_rect(*x, y, width, height)
    );
    *x += width + 6 * scale;
    return clicked;
}

static void draw_band_controls(
    SoundUi *ui,
    const SoundUiWorkbenchState *state,
    int left,
    int top,
    int height
) {
    int scale = ui->text_scale;
    int x = left;
    char text[96];

    (void)snprintf(text, sizeof(text), "LOW %.0F", state->low_hz);
    x = advance_band_readout(ui, text, x, top, height);
    if (band_control_button(ui, "low-down", "-", &x, top, height)) {
        ui->pending_ui_events.lower_band_delta = -1;
    }
    if (band_control_button(ui, "low-up", "+", &x, top, height)) {
        ui->pending_ui_events.lower_band_delta = 1;
    }

    x += 6 * scale;
    (void)snprintf(text, sizeof(text), "HIGH %.0F", state->high_hz);
    x = advance_band_readout(ui, text, x, top, height);
    if (band_control_button(ui, "high-down", "-", &x, top, height)) {
        ui->pending_ui_events.upper_band_delta = -1;
    }
    if (band_control_button(ui, "high-up", "+", &x, top, height)) {
        ui->pending_ui_events.upper_band_delta = 1;
    }

    x += 6 * scale;
    (void)snprintf(
        text,
        sizeof(text),
        "METHOD %s",
        state->method_label ? state->method_label : "FFT"
    );
    if (band_control_button(ui, "method", text, &x, top, height)) {
        ui->pending_ui_events.cycle_band_method = true;
    }

    (void)snprintf(
        text,
        sizeof(text),
        "HEAR %s",
        state->audition_label ? state->audition_label : "ORIGINAL"
    );
    if (band_control_button(ui, "audition", text, &x, top, height)) {
        ui->pending_ui_events.cycle_audition = true;
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
    if (!samples ||
        sample_count == 0 ||
        width <= 0 ||
        height <= 0 ||
        !clamp_pane_to_surface(ui, left, top, &width, &height)) {
        return;
    }

    sound_ui_mark_dirty_rect(ui, left, top, width, height);
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
    (void)state;

    clear_workspace_below_toolbar(ui);

    int scale = ui->text_scale;
    int left = ui->spectrogram_left + 8 * scale;
    int top = ui->banner_height + 16 * scale;
    int button_top = top + (SOUND_UI_GLYPH_HEIGHT + 12) * scale;
    int button_width =
        sound_ui_text_width_pixels("GO TO CLIPS", scale) + 14 * scale;
    int button_height = (SOUND_UI_GLYPH_HEIGHT + 6) * scale;

    sound_ui_draw_text_scaled(
        ui,
        "NO CLIP LOADED",
        left,
        top,
        scale,
        SOUND_UI_MENU_TITLE_COLOR
    );

    begin_non_menu_imui_frame(ui, scale);
    if (sound_imui_button_rect_id(
            &ui->imui,
            "empty-go-to-clips",
            "GO TO CLIPS",
            sound_imui_rect(left, button_top, button_width, button_height)
        )) {
        ui->pending_ui_events.workspace = SOUND_WORKSPACE_CLIPS;
        ui->pending_ui_events.workspace_changed = true;
    }
    sound_imui_end(&ui->imui);
}

static void format_duration(double seconds, char *text, size_t text_size) {
    uint64_t total = seconds > 0.0 ? (uint64_t)llround(seconds) : 0;
    uint64_t minutes = total / 60U;
    uint64_t remaining = total % 60U;

    if (minutes >= 100U) {
        uint64_t hours = minutes / 60U;
        minutes %= 60U;
        (void)snprintf(
            text,
            text_size,
            "%llu:%02llu:%02llu",
            (unsigned long long)hours,
            (unsigned long long)minutes,
            (unsigned long long)remaining
        );
    } else {
        (void)snprintf(
            text,
            text_size,
            "%02llu:%02llu",
            (unsigned long long)minutes,
            (unsigned long long)remaining
        );
    }
}

static int recording_delta_between(uint64_t selected, uint64_t clicked) {
    if (clicked >= selected) {
        uint64_t delta = clicked - selected;
        return delta > (uint64_t)INT_MAX ? INT_MAX : (int)delta;
    }

    uint64_t delta = selected - clicked;
    return delta > (uint64_t)INT_MAX ? INT_MIN : -(int)delta;
}

static int recording_action_button_width(const char *label, int scale) {
    return sound_ui_text_width_pixels(label, scale) + 12 * scale;
}

static int recording_action_group_width(
    const char *const *labels,
    int count,
    int scale
) {
    int width = 0;
    int gap = 6 * scale;

    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            width += gap;
        }
        width += recording_action_button_width(labels[i], scale);
    }

    return width;
}

static bool recording_action_button(
    SoundUi *ui,
    const char *name,
    const char *label,
    int *x,
    int top,
    int height,
    int scale
) {
    int width = recording_action_button_width(label, scale);
    bool clicked = sound_imui_button_rect_id(
        &ui->imui,
        name,
        label,
        sound_imui_rect(*x, top, width, height)
    );

    *x += width + 6 * scale;
    return clicked;
}

static void draw_recording_actions(
    SoundUi *ui,
    bool delete_pending,
    int left,
    int top,
    int height,
    int scale
) {
    int x = left;

    if (delete_pending) {
        if (recording_action_button(
                ui,
                "confirm-delete",
                "CONFIRM",
                &x,
                top,
                height,
                scale
            )) {
            ui->pending_ui_events.delete_recording = true;
        }
        if (recording_action_button(
                ui,
                "cancel-delete",
                "CANCEL",
                &x,
                top,
                height,
                scale
            )) {
            ui->pending_ui_events.cancel_recording_delete = true;
        }
        return;
    }

    if (recording_action_button(ui, "load", "LOAD", &x, top, height, scale)) {
        ui->pending_ui_events.select_recording = true;
    }
    if (recording_action_button(ui, "rename", "RENAME", &x, top, height, scale)) {
        ui->pending_ui_events.begin_recording_rename = true;
    }
    if (recording_action_button(ui, "delete", "DELETE", &x, top, height, scale)) {
        ui->pending_ui_events.delete_recording = true;
    }
}

static size_t text_capacity_for_width(int width, int scale) {
    if (width <= 0 || scale <= 0) {
        return 0;
    }

    int advance = (SOUND_UI_GLYPH_WIDTH + 1) * scale;
    return (size_t)((width + scale) / advance);
}

static void copy_text_for_width(
    char *output,
    size_t output_size,
    const char *text,
    int width,
    int scale
) {
    if (output_size == 0U) {
        return;
    }

    output[0] = '\0';
    size_t capacity = text_capacity_for_width(width, scale);
    if (capacity + 1U > output_size) {
        capacity = output_size - 1U;
    }

    if (capacity == 0U) {
        return;
    }

    const char *source = text ? text : "";
    size_t length = strlen(source);
    if (length <= capacity) {
        (void)snprintf(output, output_size, "%s", source);
        return;
    }

    if (capacity > 3U) {
        size_t prefix = capacity - 3U;
        (void)snprintf(output, output_size, "%.*s...", (int)prefix, source);
    } else {
        (void)snprintf(output, output_size, "%.*s", (int)capacity, source);
    }
}

static void copy_recording_stem(
    char *output,
    size_t output_size,
    const char *label
) {
    if (output_size == 0U) {
        return;
    }

    (void)snprintf(output, output_size, "%s", label && label[0] != '\0' ?
        label :
        "recording");

    char *extension = strrchr(output, '.');
    if (extension &&
        (strcmp(extension, ".wav") == 0 ||
            strcmp(extension, ".WAV") == 0 ||
            strcmp(extension, ".Wav") == 0)) {
        *extension = '\0';
    }

    if (output[0] == '\0') {
        (void)snprintf(output, output_size, "%s", "recording");
    }
}

static void format_recording_row_text(
    char *output,
    size_t output_size,
    const SoundUiRecordingSummary *summary,
    bool selected,
    bool active,
    const char *duration,
    int width,
    int scale
) {
    char prefix[8];
    char suffix[96];
    char label[SOUND_UI_RECORDING_LABEL_CAPACITY];
    char full[192];

    (void)snprintf(prefix, sizeof(prefix), "%c%c  ", selected ? '>' : ' ', active ? '*' : ' ');
    (void)snprintf(
        suffix,
        sizeof(suffix),
        "  %s  %s%s",
        duration,
        summary->created,
        summary->loaded ? "" : "  metadata"
    );

    int fixed_width =
        sound_ui_text_width_pixels(prefix, scale) +
        sound_ui_text_width_pixels(suffix, scale);
    copy_text_for_width(
        label,
        sizeof(label),
        summary->label,
        width - fixed_width,
        scale
    );

    (void)snprintf(full, sizeof(full), "%s%s%s", prefix, label, suffix);
    copy_text_for_width(output, output_size, full, width, scale);
}

void sound_ui_draw_recordings_workspace(
    SoundUi *ui,
    const SoundUiWorkbenchState *state
) {
    clear_plain_workspace_pane(ui);

    int scale = ui->text_scale;
    int left = ui->spectrogram_left + 8 * scale;
    int width = ui->width - left - 8 * scale;
    int line_height = (SOUND_UI_GLYPH_HEIGHT + 5) * scale;
    int bottom = ui->spectrogram_top + ui->spectrogram_height - 8 * scale;
    char line[192];
    char active_text[24];
    const char *status_text = NULL;
    uint32_t status_color = SOUND_UI_AXIS_TEXT_COLOR;

    if (state->has_active_recording) {
        (void)snprintf(
            active_text,
            sizeof(active_text),
            "%llu",
            (unsigned long long)(state->active_recording_index + 1U)
        );
    } else {
        (void)snprintf(active_text, sizeof(active_text), "NONE");
    }

    (void)snprintf(
        line,
        sizeof(line),
        "RECORDINGS  %llu FILES  HIGHLIGHT %llu  ACTIVE %s",
        (unsigned long long)state->recording_count,
        state->recording_count > 0 ?
            (unsigned long long)(state->recording_index + 1U) :
            0ULL,
        active_text
    );
    sound_ui_draw_panel_text(ui, line, 0, SOUND_UI_MENU_TITLE_COLOR);

    if (state->recording_scan_failed) {
        status_text = "COULD NOT SCAN recordings/";
        status_color = SOUND_UI_MENU_RECORDING_COLOR;
    } else if (!state->recording_scan_complete) {
        status_text = "SCANNING recordings/ FOR WAV FILES";
    } else if (state->recording_count == 0) {
        status_text = "NO WAV RECORDINGS IN recordings/";
    }

    if (status_text) {
        sound_ui_draw_panel_text(ui, status_text, 2, status_color);
    }

    if (state->recording_count == 0) {
        ui->recording_rename_inline_active = false;
        ui->recording_rename_focus_pending = false;
        return;
    }

    if (!state->recording_rename_active) {
        ui->recording_rename_inline_active = false;
        ui->recording_rename_focus_pending = false;
    }

    int y = ui->spectrogram_top + (status_text ? 56 : 36) * scale;
    int max_rows = (bottom - y) / line_height;
    if (max_rows < 3) {
        max_rows = 3;
    }

    uint64_t first = 0;
    if (state->recording_index >= (uint64_t)max_rows) {
        first = state->recording_index - (uint64_t)max_rows + 1U;
    }

    SoundImuiRect list_rect = sound_imui_rect(
        left - 3 * scale,
        y - 2 * scale,
        width + 6 * scale,
        max_rows * line_height
    );

    begin_non_menu_imui_frame(ui, scale);
    const SoundImuiInput *input = ui->imui.input;
    if (!state->recording_rename_active &&
        input &&
        input->wheel_y != 0 &&
        imui_rect_contains(list_rect, input->mouse_x, input->mouse_y)) {
        ui->pending_ui_events.recording_delta -= input->wheel_y;
    }

    sound_imui_push_id(&ui->imui, "recordings");
    for (int row = 0; row < max_rows; ++row) {
        uint64_t index = first + (uint64_t)row;
        if (index >= state->recording_count) {
            break;
        }

        const SoundUiRecordingSummary *summary = &state->recordings[index];
        bool selected = index == state->recording_index;
        bool active = state->has_active_recording &&
            index == state->active_recording_index;
        char duration[24];
        SoundImuiRect row_rect = sound_imui_rect(
            left - 3 * scale,
            y - 2 * scale,
            width + 6 * scale,
            line_height
        );
        char row_id[32];
        bool renaming = selected && state->recording_rename_active;
        bool delete_pending = selected && state->recording_delete_pending;
        const char *const normal_actions[] = {"LOAD", "RENAME", "DELETE"};
        const char *const delete_actions[] = {"CONFIRM", "CANCEL"};
        int button_height = (SOUND_UI_GLYPH_HEIGHT + 6) * scale;
        int button_top = row_rect.y + (line_height - button_height) / 2;
        int action_width = 0;
        int action_left = left + width;
        int text_right = left + width;
        int text_width = width;

        format_duration(summary->seconds, duration, sizeof(duration));
        (void)snprintf(
            row_id,
            sizeof(row_id),
            "recording-%llu",
            (unsigned long long)index
        );

        if (selected && !renaming) {
            action_width = delete_pending ?
                recording_action_group_width(delete_actions, 2, scale) :
                recording_action_group_width(normal_actions, 3, scale);
            action_left = left + width - action_width;
            if (action_left < left) {
                action_left = left;
            }
            text_right = action_left - 6 * scale;
            if (text_right < left) {
                text_right = left;
            }
            text_width = text_right - left;
        }

        sound_imui_push_id(&ui->imui, row_id);
        SoundImuiRect body_rect = row_rect;
        if (selected && !renaming) {
            int body_right = text_right + 3 * scale;
            if (body_right < body_rect.x) {
                body_right = body_rect.x;
            }
            body_rect.width = body_right - body_rect.x;
        }

        if (!state->recording_rename_active &&
            body_rect.width > 0 &&
            sound_imui_hit_rect(&ui->imui, "body", body_rect, NULL, NULL)) {
            if (selected) {
                ui->pending_ui_events.select_recording = true;
            } else {
                ui->pending_ui_events.recording_delta +=
                    recording_delta_between(state->recording_index, index);
            }
        }

        if (selected) {
            sound_ui_fill_rect(
                ui,
                left - 3 * scale,
                y - 2 * scale,
                width + 6 * scale,
                line_height,
                SOUND_UI_MENU_SELECTED_COLOR
            );
        }

        if (renaming) {
            if (!ui->recording_rename_inline_active ||
                ui->recording_rename_index != index) {
                copy_recording_stem(
                    ui->recording_rename_buffer,
                    sizeof(ui->recording_rename_buffer),
                    summary->label
                );
                ui->recording_rename_index = index;
                ui->recording_rename_inline_active = true;
                ui->recording_rename_focus_pending = true;
            }

            SoundImuiRect field_rect = sound_imui_rect(
                left,
                button_top,
                width,
                button_height
            );
            uint32_t field_id = sound_imui_id(&ui->imui, "rename-field");
            if (ui->recording_rename_focus_pending) {
                sound_imui_focus_text_field(
                    &ui->imui,
                    "rename-field",
                    ui->recording_rename_buffer
                );
                ui->recording_rename_focus_pending = false;
            }

            bool field_was_focused = sound_imui_focus_id(&ui->imui) == field_id;
            bool committed = sound_imui_text_field_rect(
                &ui->imui,
                "rename-field",
                ui->recording_rename_buffer,
                sizeof(ui->recording_rename_buffer),
                field_rect
            );

            if (committed) {
                ui->pending_ui_events.commit_recording_rename = true;
                ui->pending_ui_events.recording_rename_text_replace = true;
                (void)snprintf(
                    ui->pending_ui_events.recording_rename_text,
                    sizeof(ui->pending_ui_events.recording_rename_text),
                    "%s",
                    ui->recording_rename_buffer
                );
                ui->imui.focus_id = 0U;
            } else if (input && input->key_escape && field_was_focused) {
                ui->pending_ui_events.cancel_recording_rename = true;
                ui->imui.focus_id = 0U;
            }
        } else {
            format_recording_row_text(
                line,
                sizeof(line),
                summary,
                selected,
                active,
                duration,
                text_width,
                scale
            );
            sound_ui_draw_text(
                ui,
                line,
                left,
                y,
                delete_pending ?
                    SOUND_UI_MENU_RECORDING_COLOR :
                    (selected ? SOUND_UI_MENU_TITLE_COLOR : SOUND_UI_AXIS_TEXT_COLOR)
            );

            if (selected) {
                draw_recording_actions(
                    ui,
                    delete_pending,
                    action_left,
                    button_top,
                    button_height,
                    scale
                );
            }
        }

        sound_imui_pop_id(&ui->imui);
        y += line_height;
    }
    sound_imui_pop_id(&ui->imui);
    sound_imui_end(&ui->imui);
}

static uint64_t sample_for_strip_x(
    int x,
    int strip_left,
    int strip_width,
    uint64_t sample_count
) {
    if (strip_width <= 0 || sample_count == 0) {
        return 0;
    }

    int relative = x - strip_left;
    if (relative <= 0) {
        return 0;
    }

    if (relative >= strip_width) {
        return sample_count;
    }

    long double scaled = (long double)relative *
        (long double)sample_count /
        (long double)strip_width;
    uint64_t sample = (uint64_t)scaled;
    return sample > sample_count ? sample_count : sample;
}

static uint64_t sample_distance(uint64_t first, uint64_t second) {
    return first > second ? first - second : second - first;
}

static bool trim_sample_nearer_end(
    const SoundUiWorkbenchState *state,
    uint64_t sample
) {
    uint64_t sample_count = state->source_sample_count;
    uint64_t start = clamp_sample(state->draft_trim_start_sample, sample_count);
    uint64_t end = state->draft_trim_end_sample == 0 ?
        sample_count :
        clamp_sample(state->draft_trim_end_sample, sample_count);

    return sample_distance(sample, end) < sample_distance(sample, start);
}

void sound_ui_draw_trim_workspace(
    SoundUi *ui,
    const float *samples,
    uint64_t sample_count,
    const float *spectrogram_cells,
    uint64_t spectrogram_columns,
    uint64_t spectrogram_rows,
    const SoundUiWorkbenchState *state
) {
    clear_workspace_below_toolbar(ui);

    if (!samples || sample_count == 0) {
        sound_ui_draw_empty_workspace(ui, state);
        return;
    }

    int scale = ui->text_scale;
    int left = ui->spectrogram_left + 8 * scale;
    int width = ui->width - left - 8 * scale;
    int pane_top = ui->banner_height;
    int pane_height = ui->height - pane_top;
    int bottom = ui->height - 8 * scale;
    int header_y = pane_top + 10 * scale;
    int header_height = SOUND_UI_GLYPH_HEIGHT * scale;
    int top = header_y + header_height + 12 * scale;
    int gap = 10 * scale;
    int button_height = (SOUND_UI_GLYPH_HEIGHT + 6) * scale;
    int waveform_height = pane_height * 40 / 100;
    char line[192];

    if (width <= 0 || pane_height <= 0) {
        return;
    }

    int minimum_waveform_height = 80 * scale;
    if (waveform_height < minimum_waveform_height) {
        waveform_height = minimum_waveform_height;
    }

    int maximum_waveform_height =
        bottom - top - gap - button_height - gap - 80 * scale;
    if (maximum_waveform_height < 32 * scale) {
        maximum_waveform_height = bottom - top - gap - button_height - gap;
    }
    if (waveform_height > maximum_waveform_height) {
        waveform_height = maximum_waveform_height;
    }
    if (waveform_height <= 0) {
        return;
    }

    int button_top = top + waveform_height + gap;
    int spectrum_top = button_top + button_height + gap;
    int spectrum_height = bottom - spectrum_top;

    (void)snprintf(
        line,
        sizeof(line),
        "TRIM  %s  ACTIVE %.2F S / %.2F S",
        state->clip_label ? state->clip_label : "recording",
        state->active_seconds,
        state->clip_seconds
    );
    sound_ui_draw_text_scaled(
        ui,
        line,
        left,
        header_y,
        scale,
        SOUND_UI_MENU_TITLE_COLOR
    );

    sound_ui_draw_waveform_in_rect(
        ui,
        samples,
        sample_count,
        left,
        top,
        width,
        waveform_height,
        SOUND_UI_WAVEFORM_COLOR
    );
    draw_trim_timeline_markers(ui, left, top, width, waveform_height, state);

    begin_non_menu_imui_frame(ui, scale);
    sound_imui_push_id(&ui->imui, "trim");
    bool hovered = false;
    bool active = false;
    SoundImuiRect strip_rect = sound_imui_rect(left, top, width, waveform_height);
    (void)sound_imui_hit_rect(
        &ui->imui,
        "source-waveform",
        strip_rect,
        &hovered,
        &active
    );

    const SoundImuiInput *input = ui->imui.input;
    if (input && hovered && input->mouse_left_pressed) {
        uint64_t sample = sample_for_strip_x(
            input->mouse_x,
            left,
            width,
            state->source_sample_count
        );
        ui->trim_drag_handle_end = trim_sample_nearer_end(state, sample);
    }

    if (input && active && (input->mouse_left_down || input->mouse_left_pressed)) {
        ui->pending_ui_events.trim_set_handle = true;
        ui->pending_ui_events.trim_set_handle_end = ui->trim_drag_handle_end;
        ui->pending_ui_events.trim_set_sample = sample_for_strip_x(
            input->mouse_x,
            left,
            width,
            state->source_sample_count
        );
    }

    int apply_width =
        sound_ui_text_width_pixels("APPLY TRIM", scale) + 14 * scale;
    int clear_width = sound_ui_text_width_pixels("CLEAR", scale) + 14 * scale;
    SoundImuiRect button_rect = sound_imui_rect(
        left,
        button_top,
        apply_width,
        button_height
    );
    if (sound_imui_button_rect_id(
            &ui->imui,
            "apply-trim",
            "APPLY TRIM",
            button_rect
        )) {
        ui->pending_ui_events.trim_commit = true;
    }

    button_rect.x += apply_width + 8 * scale;
    button_rect.width = clear_width;
    if (sound_imui_button_rect_id(
            &ui->imui,
            "clear-trim",
            "CLEAR",
            button_rect
        )) {
        ui->pending_ui_events.trim_clear = true;
    }

    sound_imui_pop_id(&ui->imui);
    sound_imui_end(&ui->imui);

    if (spectrum_height <= 0) {
        return;
    }

    sound_ui_draw_axis_in_rect(ui, spectrum_top, spectrum_height);
    draw_offline_spectrogram_in_rect(
        ui,
        spectrogram_cells,
        spectrogram_columns,
        spectrogram_rows,
        ui->spectrogram_left,
        spectrum_top,
        sound_ui_plot_width(ui),
        spectrum_height
    );
}

void sound_ui_draw_spectrum_workspace(
    SoundUi *ui,
    const float *spectrogram_cells,
    uint64_t spectrogram_columns,
    uint64_t spectrogram_rows,
    const float *db_rows,
    uint64_t row_count,
    const SoundUiWorkbenchState *state
) {
    clear_plain_workspace_pane(ui);

    if (!spectrogram_cells ||
        spectrogram_columns == 0U ||
        spectrogram_rows == 0U ||
        !db_rows ||
        row_count == 0U) {
        sound_ui_draw_empty_workspace(ui, state);
        return;
    }

    int scale = ui->text_scale;
    int left = ui->spectrogram_left;
    int width = sound_ui_plot_width(ui);
    if (width <= 0) {
        return;
    }

    int header_y = ui->spectrogram_top + 10 * scale;
    int header_height = SOUND_UI_GLYPH_HEIGHT * scale;
    int plot_top = header_y + header_height + 10 * scale;
    int plot_bottom = ui->height - 8 * scale;
    int plot_height = plot_bottom - plot_top;
    int gap = 8 * scale;
    int profile_width = width / 5;
    if (profile_width < 48 * scale) {
        profile_width = 48 * scale;
    }
    if (profile_width > width / 2) {
        profile_width = width / 2;
    }

    int spectrogram_width = width - profile_width - gap;
    if (spectrogram_width <= 0 || plot_height <= 0) {
        return;
    }

    char line[160];
    (void)snprintf(
        line,
        sizeof(line),
        "SPECTRUM  %s  %.2F S",
        state->clip_label ? state->clip_label : "recording",
        state->active_seconds
    );
    sound_ui_draw_text_scaled(
        ui,
        line,
        left,
        header_y,
        scale,
        SOUND_UI_MENU_TITLE_COLOR
    );

    sound_ui_draw_axis_in_rect(ui, plot_top, plot_height);
    draw_offline_spectrogram_in_rect(
        ui,
        spectrogram_cells,
        spectrogram_columns,
        spectrogram_rows,
        left,
        plot_top,
        spectrogram_width,
        plot_height
    );
    draw_profile_bars_in_rect(
        ui,
        db_rows,
        row_count,
        left + spectrogram_width + gap,
        plot_top,
        profile_width,
        plot_height
    );
}

void sound_ui_draw_band_workspace(
    SoundUi *ui,
    const SoundUiWorkbenchState *state
) {
    clear_plain_workspace_pane(ui);

    if (!state || !state->has_clip) {
        sound_ui_draw_empty_workspace(ui, state);
        return;
    }

    if (!state->band_spectrogram_cells ||
        state->band_spectrogram_columns == 0U ||
        state->band_spectrogram_rows == 0U ||
        !state->filtered_samples ||
        state->filtered_sample_count == 0U) {
        draw_centered_workspace_message(
            ui,
            ui->spectrogram_top,
            ui->spectrogram_height,
            "RENDERING BAND..."
        );
        return;
    }

    int scale = ui->text_scale;
    int plot_left = ui->spectrogram_left;
    int plot_width = sound_ui_plot_width(ui);
    int text_left = plot_left + 8 * scale;
    int pane_top = ui->spectrogram_top;
    int pane_bottom = ui->height - 8 * scale;
    int pane_height = pane_bottom - pane_top;
    int header_y = pane_top + 10 * scale;
    int header_height = SOUND_UI_GLYPH_HEIGHT * scale;
    int control_height = (SOUND_UI_GLYPH_HEIGHT + 6) * scale;
    int control_top = header_y + header_height + 8 * scale;
    int gap = 8 * scale;
    int plot_top = control_top + control_height + 10 * scale;
    int remaining_height = pane_bottom - plot_top;

    if (plot_width <= 0 || pane_height <= 0 || remaining_height <= gap) {
        return;
    }

    int plot_height = remaining_height * 65 / 100;
    int minimum_plot_height = 80 * scale;
    if (plot_height < minimum_plot_height &&
        remaining_height - gap > minimum_plot_height) {
        plot_height = minimum_plot_height;
    }

    int waveform_top = plot_top + plot_height + gap;
    int waveform_height = pane_bottom - waveform_top;
    int minimum_waveform_height = 40 * scale;
    if (waveform_height < minimum_waveform_height) {
        plot_height = remaining_height - gap - minimum_waveform_height;
        waveform_top = plot_top + plot_height + gap;
        waveform_height = pane_bottom - waveform_top;
    }

    if (plot_height <= 0 || waveform_height <= 0) {
        return;
    }

    char line[160];
    (void)snprintf(
        line,
        sizeof(line),
        "BAND  %.0F-%.0F HZ  %s",
        state->low_hz,
        state->high_hz,
        state->method_label ? state->method_label : "FFT"
    );
    sound_ui_draw_text_scaled(
        ui,
        line,
        text_left,
        header_y,
        scale,
        SOUND_UI_MENU_TITLE_COLOR
    );

    begin_non_menu_imui_frame(ui, scale);
    sound_imui_push_id(&ui->imui, "band");
    draw_band_controls(ui, state, text_left, control_top, control_height);

    sound_ui_draw_axis_in_rect(ui, plot_top, plot_height);
    draw_offline_spectrogram_in_rect(
        ui,
        state->band_spectrogram_cells,
        state->band_spectrogram_columns,
        state->band_spectrogram_rows,
        plot_left,
        plot_top,
        plot_width,
        plot_height
    );

    dim_band_spectrogram_outside(
        ui,
        state,
        plot_left,
        plot_top,
        plot_width,
        plot_height
    );
    draw_band_edge_markers(ui, state, plot_left, plot_top, plot_width, plot_height);

    int high_row =
        clamped_spectrogram_row_for_height(ui, state->high_hz, plot_height);
    int low_row =
        clamped_spectrogram_row_for_height(ui, state->low_hz, plot_height);
    update_band_edge_drag(
        ui,
        "high-edge",
        true,
        high_row,
        plot_left,
        plot_top,
        plot_width,
        plot_height
    );
    update_band_edge_drag(
        ui,
        "low-edge",
        false,
        low_row,
        plot_left,
        plot_top,
        plot_width,
        plot_height
    );
    sound_imui_pop_id(&ui->imui);
    sound_imui_end(&ui->imui);

    sound_ui_draw_waveform_in_rect(
        ui,
        state->filtered_samples,
        state->filtered_sample_count,
        plot_left,
        waveform_top,
        plot_width,
        waveform_height,
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
