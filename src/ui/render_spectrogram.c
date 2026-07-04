#include "internal.h"

#include "font.h"

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
    {2000.0, "2K", false},
    {1000.0, "1K", true},
    {500.0, "500", false},
    {200.0, "200", false},
    {100.0, "100", true},
    {50.0, "50", false},
    {20.0, "20", false},
};

int sound_ui_spectrogram_row_for_frequency(const SoundUi *ui, double hz) {
    double log_min = log2(ui->min_hz);
    double log_max = log2(ui->max_hz);

    if (log_max <= log_min || hz <= 0.0) {
        return -1;
    }

    double unit = (log_max - log2(hz)) / (log_max - log_min);
    int row = (int)lrint(unit * (double)ui->spectrogram_height - 0.5);

    return row >= 0 && row < ui->spectrogram_height ? row : -1;
}

void sound_ui_draw_frequency_band_fill(
    SoundUi *ui,
    double low_hz,
    double high_hz,
    uint32_t color
) {
    int low_row = sound_ui_spectrogram_row_for_frequency(ui, low_hz);
    int high_row = sound_ui_spectrogram_row_for_frequency(ui, high_hz);

    if (low_row < 0 || high_row < 0) {
        return;
    }

    int top = high_row < low_row ? high_row : low_row;
    int bottom = high_row > low_row ? high_row : low_row;
    int left = ui->spectrogram_left;
    int width = ui->width - left;

    for (int row = top; row <= bottom; ++row) {
        uint32_t *pixels = sound_ui_row(ui, ui->spectrogram_top + row);

        for (int x = left; x < left + width; ++x) {
            pixels[x] = sound_ui_blend_color(pixels[x], color, 0.40F);
        }
    }
}

void sound_ui_draw_axis(SoundUi *ui) {
    int top = ui->spectrogram_top;
    int gutter = ui->spectrogram_left;
    int scale = ui->text_scale;

    for (int y = top; y < top + ui->spectrogram_height; ++y) {
        uint32_t *pixels = sound_ui_row(ui, y);

        for (int x = 0; x < gutter; ++x) {
            pixels[x] = SOUND_UI_AXIS_BACKGROUND_COLOR;
        }
    }

    memset(ui->grid_flags, 0, (size_t)ui->spectrogram_height);

    int label_gap = 2 * scale;
    int last_label_bottom = top - label_gap;
    size_t tick_count = sizeof(frequency_ticks) / sizeof(frequency_ticks[0]);

    for (size_t i = 0; i < tick_count; ++i) {
        const FrequencyTick *tick = &frequency_ticks[i];

        if (tick->hz < ui->min_hz || tick->hz > ui->max_hz) {
            continue;
        }

        int row = sound_ui_spectrogram_row_for_frequency(ui, tick->hz);
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

        if (tick->gridline) {
            ui->grid_flags[row] = 1;
        }

        int text_height = SOUND_UI_GLYPH_HEIGHT * scale;
        int text_top = y - text_height / 2;

        if (text_top < top) {
            text_top = top;
        }

        if (text_top + text_height > top + ui->spectrogram_height) {
            text_top = top + ui->spectrogram_height - text_height;
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

    for (int y = 0; y < ui->spectrogram_height; ++y) {
        float db = vertically_smoothed_column(column, (uint64_t)ui->spectrogram_height, (uint64_t)y);
        ui->bands[y] += smoothing * (db - ui->bands[y]);

        uint32_t color = sound_ui_color_for_db(ui, ui->bands[y]);

        if (ui->grid_flags[y]) {
            color = sound_ui_blend_color(color, SOUND_UI_GRIDLINE_COLOR, 0.25F);
        }

        sound_ui_row(ui, ui->spectrogram_top + y)[x] = color;
    }
}

void sound_ui_prepare_resized_buffer(SoundUi *ui) {
    for (int i = 0; i < ui->spectrogram_height; ++i) {
        ui->bands[i] = sound_ui_floor_db;
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
    if (!ui->pixels || !ui->bands) {
        return;
    }

    for (int i = 0; i < ui->spectrogram_height; ++i) {
        ui->bands[i] = sound_ui_floor_db;
    }

    ui->spectrogram_origin = 0;

    for (int y = 0; y < ui->spectrogram_height; ++y) {
        uint32_t *row = sound_ui_row(ui, ui->spectrogram_top + y);

        for (int x = 0; x < ui->width; ++x) {
            row[x] = SOUND_UI_BACKGROUND_COLOR;
        }
    }

    sound_ui_draw_axis(ui);
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
