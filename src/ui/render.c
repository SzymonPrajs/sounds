#include "internal.h"

#include "sounds/colormap.h"
#include "font.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const float floor_db = -95.0F;
static const float ceiling_db = -15.0F;

static const uint32_t background_color = 0x05070C;
static const uint32_t separator_color = 0x10141C;
static const uint32_t midline_color = 0x1B2433;
static const uint32_t waveform_color = 0x95DDFF;
static const uint32_t axis_background_color = 0x0B0F16;
static const uint32_t axis_text_color = 0x8FA3BF;
static const uint32_t axis_tick_color = 0x415068;
static const uint32_t gridline_color = 0x3E4A66;

static const int separator_height = 2;

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

uint32_t *sound_ui_row(SoundUi *ui, int y) {
    return ui->pixels + (size_t)y * (size_t)ui->width;
}

static uint32_t pack_color(SoundColor color) {
    uint32_t red = (uint32_t)lrintf(color.red * 255.0F);
    uint32_t green = (uint32_t)lrintf(color.green * 255.0F);
    uint32_t blue = (uint32_t)lrintf(color.blue * 255.0F);

    return (red << 16) | (green << 8) | blue;
}

static uint32_t color_for_db(float db) {
    float unit = (db - floor_db) / (ceiling_db - floor_db);
    return pack_color(sound_colormap_viridis(unit));
}

static double amplitude_db(double value) {
    return 20.0 * log10(fmax(value, 1.0e-12));
}

static uint32_t blend_color(uint32_t base, uint32_t over, float amount) {
    float keep = 1.0F - amount;
    uint32_t red = (uint32_t)lrintf(
        (float)((base >> 16) & 0xFFU) * keep + (float)((over >> 16) & 0xFFU) * amount
    );
    uint32_t green = (uint32_t)lrintf(
        (float)((base >> 8) & 0xFFU) * keep + (float)((over >> 8) & 0xFFU) * amount
    );
    uint32_t blue = (uint32_t)lrintf(
        (float)(base & 0xFFU) * keep + (float)(over & 0xFFU) * amount
    );

    return (red << 16) | (green << 8) | blue;
}

static void fill_rows(SoundUi *ui, int from, int rows, uint32_t color) {
    for (int y = from; y < from + rows; ++y) {
        uint32_t *row = sound_ui_row(ui, y);

        for (int x = 0; x < ui->width; ++x) {
            row[x] = color;
        }
    }
}

static void draw_text(SoundUi *ui, const char *text, int x, int y, uint32_t color) {
    int scale = ui->text_scale;

    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        const uint8_t *glyph = sound_ui_glyph_bitmap(*cursor);

        for (int row = 0; row < SOUND_UI_GLYPH_HEIGHT; ++row) {
            uint8_t bits = glyph[row];

            for (int column = 0; column < SOUND_UI_GLYPH_WIDTH; ++column) {
                if ((bits & (uint8_t)(1U << (SOUND_UI_GLYPH_WIDTH - 1 - column))) == 0U) {
                    continue;
                }

                for (int dy = 0; dy < scale; ++dy) {
                    int py = y + row * scale + dy;

                    if (py < 0 || py >= ui->height) {
                        continue;
                    }

                    uint32_t *pixels = sound_ui_row(ui, py);

                    for (int dx = 0; dx < scale; ++dx) {
                        int px = x + column * scale + dx;

                        if (px >= 0 && px < ui->width) {
                            pixels[px] = color;
                        }
                    }
                }
            }
        }

        x += (SOUND_UI_GLYPH_WIDTH + 1) * scale;
    }
}

static int spectrogram_row_for_frequency(const SoundUi *ui, double hz) {
    double log_min = log2(ui->min_hz);
    double log_max = log2(ui->max_hz);

    if (log_max <= log_min || hz <= 0.0) {
        return -1;
    }

    double unit = (log_max - log2(hz)) / (log_max - log_min);
    int row = (int)lrint(unit * (double)ui->spectrogram_height - 0.5);

    return row >= 0 && row < ui->spectrogram_height ? row : -1;
}

static void draw_axis(SoundUi *ui) {
    int top = ui->spectrogram_top;
    int gutter = ui->spectrogram_left;
    int scale = ui->text_scale;

    for (int y = top; y < top + ui->spectrogram_height; ++y) {
        uint32_t *pixels = sound_ui_row(ui, y);

        for (int x = 0; x < gutter; ++x) {
            pixels[x] = axis_background_color;
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

        int row = spectrogram_row_for_frequency(ui, tick->hz);
        if (row < 0) {
            continue;
        }

        int y = top + row;
        uint32_t *pixels = sound_ui_row(ui, y);

        for (int x = gutter - 4 * scale; x < gutter; ++x) {
            if (x >= 0) {
                pixels[x] = axis_tick_color;
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

        draw_text(ui, tick->label, text_x, text_top, axis_text_color);
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

        uint32_t color = color_for_db(ui->bands[y]);

        if (ui->grid_flags[y]) {
            color = blend_color(color, gridline_color, 0.25F);
        }

        sound_ui_row(ui, ui->spectrogram_top + y)[x] = color;
    }
}

static int waveform_y(double value, double gain, int rows) {
    double scaled = value * gain;

    if (scaled > 1.0) {
        scaled = 1.0;
    } else if (scaled < -1.0) {
        scaled = -1.0;
    }

    int center = rows / 2;
    int y = center - (int)lrint(scaled * (double)center);

    if (y < 0) {
        return 0;
    }

    if (y >= rows) {
        return rows - 1;
    }

    return y;
}

void sound_ui_prepare_resized_buffer(SoundUi *ui) {
    for (int i = 0; i < ui->spectrogram_height; ++i) {
        ui->bands[i] = floor_db;
    }

    ui->spectrogram_origin = 0;

    fill_rows(ui, 0, ui->height, background_color);
    fill_rows(ui, 0, ui->banner_height, axis_background_color);
    fill_rows(
        ui,
        ui->banner_height + ui->waveform_height,
        separator_height,
        separator_color
    );
    draw_axis(ui);
}

void sound_ui_clear_spectrogram(SoundUi *ui) {
    if (!ui->pixels || !ui->bands) {
        return;
    }

    for (int i = 0; i < ui->spectrogram_height; ++i) {
        ui->bands[i] = floor_db;
    }

    ui->spectrogram_origin = 0;

    for (int y = 0; y < ui->spectrogram_height; ++y) {
        uint32_t *row = sound_ui_row(ui, ui->spectrogram_top + y);

        for (int x = 0; x < ui->width; ++x) {
            row[x] = background_color;
        }
    }

    draw_axis(ui);
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

void sound_ui_draw_waveform(
    SoundUi *ui,
    const float *samples,
    uint64_t sample_count,
    double peak
) {
    int rows = ui->waveform_height;
    int waveform_top = ui->banner_height;

    fill_rows(ui, waveform_top, rows, background_color);
    fill_rows(ui, waveform_top + rows / 2, 1, midline_color);

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

        int top = waveform_y(high, gain, rows);
        int bottom = waveform_y(low, gain, rows);

        for (int y = top; y <= bottom; ++y) {
            sound_ui_row(ui, waveform_top + y)[x] = waveform_color;
        }
    }
}

void sound_ui_draw_banner(
    SoundUi *ui,
    SoundAppMode mode,
    bool sst_enabled
) {
    const char *text = sound_app_mode_banner(mode, sst_enabled);
    int scale = ui->text_scale;
    int text_top = (ui->banner_height - SOUND_UI_GLYPH_HEIGHT * scale) / 2;
    int text_left = 6 * scale;

    for (int y = 0; y < ui->banner_height; ++y) {
        uint32_t *row = sound_ui_row(ui, y);

        for (int x = 0; x < ui->width; ++x) {
            row[x] = axis_background_color;
        }
    }

    draw_text(ui, text, text_left, text_top, axis_text_color);
}

void sound_ui_set_title(SoundUi *ui, const SoundUiTitle *title) {
    char text[192];

    (void)snprintf(
        text,
        sizeof(text),
        "Sounds   RMS %.1f dBFS   peak %.1f dBFS   %.0f Hz   %s   %.1f-%.0f Hz",
        amplitude_db(title->rms),
        amplitude_db(title->peak),
        title->sample_rate,
        sound_app_mode_title(title->mode, title->sst_enabled),
        title->min_hz,
        title->max_hz
    );
    (void)SDL_SetWindowTitle(ui->window, text);
}
