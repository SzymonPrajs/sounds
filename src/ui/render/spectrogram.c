#include "../internal.h"

#include "../font.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct FrequencyTick {
    double hz;
    const char *label;
    bool gridline;
} FrequencyTick;

static const FrequencyTick frequency_ticks[] = {
    {20000.0, "20K", false},
    {10000.0, "10K", true},
    {5000.0, "5K", false},
    {2400.0, "2.4K", false},
    {2000.0, "2K", false},
    {1000.0, "1K", true},
    {500.0, "500", false},
    {200.0, "200", false},
    {120.0, "120", false},
    {100.0, "100", true},
    {50.0, "50", false},
    {20.0, "20", false},
    {10.0, "10", true},
};

double sound_ui_spectrogram_frequency_for_row_in_height(
    const SoundUi *ui,
    int row,
    int height
) {
    double log_min = log2(ui->min_hz);
    double log_max = log2(ui->max_hz);

    if (log_max <= log_min || height <= 0) {
        return 0.0;
    }

    if (row < 0) {
        row = 0;
    }
    if (row >= height) {
        row = height - 1;
    }

    double unit = ((double)row + 0.5) / (double)height;
    double log_hz = log_max + (log_min - log_max) * unit;

    return pow(2.0, log_hz);
}

static bool row_is_band_boundary_in_height(
    const SoundUi *ui,
    int row,
    int height
) {
    int low_boundary = sound_ui_spectrogram_row_for_height(
        ui,
        SOUND_FREQUENCY_LOW_MAX_HZ,
        height
    );
    int mid_low_boundary = sound_ui_spectrogram_row_for_height(
        ui,
        SOUND_FREQUENCY_MID_MIN_HZ,
        height
    );
    int mid_high_boundary = sound_ui_spectrogram_row_for_height(
        ui,
        SOUND_FREQUENCY_MID_MAX_HZ,
        height
    );
    int high_boundary = sound_ui_spectrogram_row_for_height(
        ui,
        SOUND_FREQUENCY_HIGH_MIN_HZ,
        height
    );

    return row == low_boundary ||
        row == mid_low_boundary ||
        row == mid_high_boundary ||
        row == high_boundary;
}

static uint32_t apply_band_overlay_in_height(
    const SoundUi *ui,
    int row,
    int height,
    uint32_t color
) {
    if (!sound_frequency_band_shows_all_bands(ui->frequency_band)) {
        return color;
    }

    double hz = sound_ui_spectrogram_frequency_for_row_in_height(ui, row, height);

    if (hz <= SOUND_FREQUENCY_LOW_MAX_HZ) {
        color = sound_ui_blend_color(color, 0x2D5660, 0.10F);
    }

    if (hz >= SOUND_FREQUENCY_MID_MIN_HZ && hz <= SOUND_FREQUENCY_MID_MAX_HZ) {
        color = sound_ui_blend_color(color, 0x463E76, 0.10F);
    }

    if (hz >= SOUND_FREQUENCY_HIGH_MIN_HZ) {
        color = sound_ui_blend_color(color, 0x6A5425, 0.10F);
    }

    if (row_is_band_boundary_in_height(ui, row, height)) {
        color = sound_ui_blend_color(color, SOUND_UI_MARKER_DIM_COLOR, 0.55F);
    }

    return color;
}

static uint32_t apply_band_overlay(const SoundUi *ui, int row, uint32_t color) {
    return apply_band_overlay_in_height(ui, row, ui->spectrogram_height, color);
}

int sound_ui_spectrogram_row_for_height(
    const SoundUi *ui,
    double hz,
    int height
) {
    double log_min = log2(ui->min_hz);
    double log_max = log2(ui->max_hz);

    if (log_max <= log_min || hz <= 0.0) {
        return -1;
    }

    double unit = (log_max - log2(hz)) / (log_max - log_min);
    int row = (int)lrint(unit * (double)height - 0.5);

    return row >= 0 && row < height ? row : -1;
}

int sound_ui_spectrogram_row_for_frequency(const SoundUi *ui, double hz) {
    return sound_ui_spectrogram_row_for_height(
        ui,
        hz,
        ui->spectrogram_height
    );
}

bool sound_ui_spectrogram_gridline_for_row(
    const SoundUi *ui,
    int row,
    int height
) {
    if (height <= 0 || row < 0 || row >= height) {
        return false;
    }

    if (sound_frequency_band_shows_all_bands(ui->frequency_band) &&
        row_is_band_boundary_in_height(ui, row, height)) {
        return true;
    }

    size_t tick_count = sizeof(frequency_ticks) / sizeof(frequency_ticks[0]);
    for (size_t i = 0; i < tick_count; ++i) {
        const FrequencyTick *tick = &frequency_ticks[i];

        if (!tick->gridline || tick->hz < ui->min_hz || tick->hz > ui->max_hz) {
            continue;
        }

        if (sound_ui_spectrogram_row_for_height(ui, tick->hz, height) == row) {
            return true;
        }
    }

    return false;
}

static void draw_axis_in_rect(SoundUi *ui, int top, int height, bool update_grid_flags) {
    int gutter = ui->spectrogram_left;
    int scale = ui->text_scale;

    if (height <= 0 || gutter <= 0) {
        return;
    }

    sound_ui_mark_dirty_rect(ui, 0, top, gutter, height);

    for (int y = top; y < top + height; ++y) {
        uint32_t *pixels = sound_ui_row(ui, y);

        for (int x = 0; x < gutter; ++x) {
            pixels[x] = apply_band_overlay_in_height(
                ui,
                y - top,
                height,
                SOUND_UI_AXIS_BACKGROUND_COLOR
            );
        }
    }

    if (update_grid_flags) {
        memset(ui->grid_flags, 0, (size_t)ui->spectrogram_height);
    }

    if (sound_frequency_band_shows_all_bands(ui->frequency_band)) {
        const double boundaries[] = {
            SOUND_FREQUENCY_MID_MAX_HZ,
            SOUND_FREQUENCY_HIGH_MIN_HZ,
            SOUND_FREQUENCY_LOW_MAX_HZ,
            SOUND_FREQUENCY_MID_MIN_HZ,
        };
        size_t boundary_count = sizeof(boundaries) / sizeof(boundaries[0]);

        for (size_t i = 0; i < boundary_count; ++i) {
            int boundary = sound_ui_spectrogram_row_for_height(
                ui,
                boundaries[i],
                height
            );

            if (update_grid_flags && boundary >= 0) {
                ui->grid_flags[boundary] = 1;
            }
        }
    }

    int label_gap = 2 * scale;
    int last_label_bottom = top - label_gap;
    size_t tick_count = sizeof(frequency_ticks) / sizeof(frequency_ticks[0]);

    for (size_t i = 0; i < tick_count; ++i) {
        const FrequencyTick *tick = &frequency_ticks[i];

        if (tick->hz < ui->min_hz || tick->hz > ui->max_hz) {
            continue;
        }

        int row = sound_ui_spectrogram_row_for_height(ui, tick->hz, height);
        if (row < 0) {
            continue;
        }

        int y = top + row;
        uint32_t *pixels = sound_ui_row(ui, y);

        for (int x = gutter - 4 * scale; x < gutter; ++x) {
            if (x >= 0) {
                pixels[x] = SOUND_UI_AXIS_TICK_COLOR;
            }
        }

        if (update_grid_flags && tick->gridline) {
            ui->grid_flags[row] = 1;
        }

        int text_height = SOUND_UI_GLYPH_HEIGHT * scale;
        int text_top = y - text_height / 2;

        if (text_top < top) {
            text_top = top;
        }

        if (text_top + text_height > top + height) {
            text_top = top + height - text_height;
        }

        if (text_top < last_label_bottom + label_gap) {
            continue;
        }

        int text_x = gutter - 5 * scale -
            sound_ui_text_width_pixels(tick->label, scale);

        if (text_x < scale) {
            text_x = scale;
        }

        sound_ui_draw_text(ui, tick->label, text_x, text_top, SOUND_UI_AXIS_TEXT_COLOR);
        last_label_bottom = text_top + text_height;
    }
}

void sound_ui_draw_axis(SoundUi *ui) {
    draw_axis_in_rect(ui, ui->spectrogram_top, ui->spectrogram_height, true);
}

void sound_ui_draw_axis_in_rect(SoundUi *ui, int top, int height) {
    draw_axis_in_rect(ui, top, height, false);
}

static float vertically_smoothed_column(const float *column, uint64_t rows, uint64_t y) {
    float center = column[y];

    if (rows <= 2) {
        return center;
    }

    if (y == 0) {
        return center * 0.70F + column[y + 1] * 0.30F;
    }

    if (y == rows - 1) {
        return column[y - 1] * 0.30F + center * 0.70F;
    }

    return column[y - 1] * 0.20F + center * 0.60F + column[y + 1] * 0.20F;
}

static int spectrogram_plot_width(const SoundUi *ui) {
    return ui->width - ui->spectrogram_left;
}

static float *stored_spectrogram_db(SoundUi *ui, int plot_x, int row) {
    int plot_width = spectrogram_plot_width(ui);
    return ui->spectrogram_db + (size_t)row * (size_t)plot_width + (size_t)plot_x;
}

static void advance_spectrogram(SoundUi *ui, int columns) {
    int plot_width = spectrogram_plot_width(ui);

    if (columns <= 0 || plot_width <= 0) {
        return;
    }

    ui->spectrogram_origin = (ui->spectrogram_origin + columns) % plot_width;
}

static int spectrogram_physical_x(const SoundUi *ui, int logical_x) {
    int plot_width = spectrogram_plot_width(ui);
    int physical_x = (ui->spectrogram_origin + logical_x) % plot_width;

    return ui->spectrogram_left + physical_x;
}

static void draw_spectrogram_column(
    SoundUi *ui,
    int x,
    const float *column,
    float smoothing
) {
    if (x < ui->spectrogram_left || x >= ui->width) {
        return;
    }

    int plot_x = x - ui->spectrogram_left;
    ui->spectrogram_filled[plot_x] = 1;
    sound_ui_mark_dirty_rect(ui, x, ui->spectrogram_top, 1, ui->spectrogram_height);

    for (int y = 0; y < ui->spectrogram_height; ++y) {
        float db = vertically_smoothed_column(column, (uint64_t)ui->spectrogram_height, (uint64_t)y);
        ui->bands[y] += smoothing * (db - ui->bands[y]);
        *stored_spectrogram_db(ui, plot_x, y) = ui->bands[y];

        uint32_t color = sound_ui_color_for_db(ui, ui->bands[y]);
        color = apply_band_overlay(ui, y, color);

        if (ui->grid_flags[y]) {
            color = sound_ui_blend_color(color, SOUND_UI_GRIDLINE_COLOR, 0.25F);
        }

        sound_ui_row(ui, ui->spectrogram_top + y)[x] = color;
    }
}

void sound_ui_prepare_resized_buffer(SoundUi *ui) {
    ui->spectrogram_wrap_enabled = true;

    for (int i = 0; i < ui->spectrogram_height; ++i) {
        ui->bands[i] = sound_ui_floor_db;
    }

    int plot_width = spectrogram_plot_width(ui);
    for (int x = 0; x < plot_width; ++x) {
        ui->spectrogram_filled[x] = 0;
        for (int y = 0; y < ui->spectrogram_height; ++y) {
            *stored_spectrogram_db(ui, x, y) = sound_ui_floor_db;
        }
    }

    ui->spectrogram_origin = 0;

    sound_ui_fill_rows(ui, 0, ui->height, SOUND_UI_BACKGROUND_COLOR);
    sound_ui_fill_rows(ui, 0, ui->banner_height, SOUND_UI_AXIS_BACKGROUND_COLOR);
    sound_ui_fill_rows(
        ui,
        ui->banner_height + ui->waveform_height,
        sound_ui_separator_height,
        SOUND_UI_SEPARATOR_COLOR
    );
    sound_ui_draw_axis(ui);
}

void sound_ui_clear_spectrogram(SoundUi *ui) {
    if (!ui->pixels || !ui->bands || !ui->spectrogram_db || !ui->spectrogram_filled) {
        return;
    }

    ui->spectrogram_wrap_enabled = true;

    for (int i = 0; i < ui->spectrogram_height; ++i) {
        ui->bands[i] = sound_ui_floor_db;
    }

    int plot_width = spectrogram_plot_width(ui);
    for (int x = 0; x < plot_width; ++x) {
        ui->spectrogram_filled[x] = 0;
        for (int y = 0; y < ui->spectrogram_height; ++y) {
            *stored_spectrogram_db(ui, x, y) = sound_ui_floor_db;
        }
    }

    ui->spectrogram_origin = 0;

    for (int y = 0; y < ui->spectrogram_height; ++y) {
        uint32_t *row = sound_ui_row(ui, ui->spectrogram_top + y);

        for (int x = 0; x < ui->width; ++x) {
            row[x] = SOUND_UI_BACKGROUND_COLOR;
        }
    }

    sound_ui_draw_axis(ui);
    sound_ui_mark_dirty_rect(ui, 0, ui->spectrogram_top, ui->width, ui->spectrogram_height);
}

void sound_ui_recolor_spectrogram(SoundUi *ui) {
    if (!ui->pixels || !ui->spectrogram_db || !ui->spectrogram_filled) {
        return;
    }

    ui->spectrogram_wrap_enabled = true;

    int plot_width = spectrogram_plot_width(ui);
    if (plot_width <= 0) {
        return;
    }

    sound_ui_draw_axis(ui);
    sound_ui_mark_dirty_rect(
        ui,
        ui->spectrogram_left,
        ui->spectrogram_top,
        plot_width,
        ui->spectrogram_height
    );

    for (int x = 0; x < plot_width; ++x) {
        for (int y = 0; y < ui->spectrogram_height; ++y) {
            uint32_t color = SOUND_UI_BACKGROUND_COLOR;

            if (ui->spectrogram_filled[x]) {
                float db = *stored_spectrogram_db(ui, x, y);
                color = sound_ui_color_for_db(ui, db);
                color = apply_band_overlay(ui, y, color);

                if (ui->grid_flags[y]) {
                    color = sound_ui_blend_color(color, SOUND_UI_GRIDLINE_COLOR, 0.25F);
                }
            }

            sound_ui_row(ui, ui->spectrogram_top + y)[ui->spectrogram_left + x] = color;
        }
    }
}

void sound_ui_mark_spectrogram_transition(SoundUi *ui) {
    if (!ui->pixels || !ui->spectrogram_db || !ui->spectrogram_filled) {
        return;
    }

    ui->spectrogram_wrap_enabled = true;

    int plot_width = spectrogram_plot_width(ui);
    if (plot_width <= 0) {
        return;
    }

    advance_spectrogram(ui, 1);

    int x = spectrogram_physical_x(ui, plot_width - 1);
    int plot_x = x - ui->spectrogram_left;
    ui->spectrogram_filled[plot_x] = 0;
    sound_ui_mark_dirty_rect(ui, x, ui->spectrogram_top, 1, ui->spectrogram_height);

    for (int y = 0; y < ui->spectrogram_height; ++y) {
        *stored_spectrogram_db(ui, plot_x, y) = sound_ui_floor_db;
        sound_ui_row(ui, ui->spectrogram_top + y)[x] = SOUND_UI_BACKGROUND_COLOR;
    }
}

void sound_ui_draw_spectrogram_columns(
    SoundUi *ui,
    const float *columns,
    uint64_t column_count,
    uint64_t row_count,
    SoundAppMode mode
) {
    if (!columns || column_count == 0 || row_count != (uint64_t)ui->spectrogram_height) {
        return;
    }

    ui->spectrogram_wrap_enabled = true;

    int plot_width = ui->width - ui->spectrogram_left;
    if (plot_width <= 0) {
        return;
    }

    uint64_t visible_columns = column_count;
    if (visible_columns > (uint64_t)plot_width) {
        columns += (visible_columns - (uint64_t)plot_width) * row_count;
        visible_columns = (uint64_t)plot_width;
    }

    float smoothing = sound_app_mode_column_smoothing(mode);
    advance_spectrogram(ui, (int)visible_columns);

    for (uint64_t column = 0; column < visible_columns; ++column) {
        int logical_x = plot_width - (int)visible_columns + (int)column;
        int x = spectrogram_physical_x(ui, logical_x);
        draw_spectrogram_column(ui, x, columns + column * row_count, smoothing);
    }
}
