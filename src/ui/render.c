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
static const uint32_t menu_panel_color = 0x0A1018;
static const uint32_t menu_border_color = 0x5F7394;
static const uint32_t menu_selected_color = 0x17243A;
static const uint32_t menu_title_color = 0xD7E8FF;
static const uint32_t menu_recording_color = 0xFF8F70;

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

static uint32_t color_for_db(const SoundUi *ui, float db) {
    float unit = (db - floor_db) / (ceiling_db - floor_db);
    return pack_color(sound_colormap_sample(ui->colormap, unit));
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

static void draw_text_scaled(
    SoundUi *ui,
    const char *text,
    int x,
    int y,
    int scale,
    uint32_t color
) {
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

static void draw_text(SoundUi *ui, const char *text, int x, int y, uint32_t color) {
    draw_text_scaled(ui, text, x, y, ui->text_scale, color);
}

static void fill_rect(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height,
    uint32_t color
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
            row[x] = color;
        }
    }
}

static void draw_rect_outline(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height,
    int thickness,
    uint32_t color
) {
    fill_rect(ui, left, top, width, thickness, color);
    fill_rect(ui, left, top + height - thickness, width, thickness, color);
    fill_rect(ui, left, top, thickness, height, color);
    fill_rect(ui, left + width - thickness, top, thickness, height, color);
}

static void dim_screen(SoundUi *ui) {
    for (int y = 0; y < ui->height; ++y) {
        uint32_t *row = sound_ui_row(ui, y);

        for (int x = 0; x < ui->width; ++x) {
            row[x] = blend_color(row[x], background_color, 0.68F);
        }
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

        uint32_t color = color_for_db(ui, ui->bands[y]);

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

static int menu_text_scale(const SoundUi *ui, int line_count) {
    int scale = ui->text_scale;
    int margin = 16 * scale;

    while (scale > 1 &&
        line_count * (SOUND_UI_GLYPH_HEIGHT + 4) * scale + margin * 2 > ui->height) {
        --scale;
        margin = 16 * scale;
    }

    return scale;
}

static void draw_colormap_preview(
    SoundUi *ui,
    SoundColormap colormap,
    int left,
    int top,
    int width,
    int height
) {
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int x = 0; x < width; ++x) {
        float unit = width == 1 ? 0.0F : (float)x / (float)(width - 1);
        uint32_t color = pack_color(sound_colormap_sample(colormap, unit));

        for (int y = 0; y < height; ++y) {
            int py = top + y;
            int px = left + x;

            if (px >= 0 && px < ui->width && py >= 0 && py < ui->height) {
                sound_ui_row(ui, py)[px] = color;
            }
        }
    }
}

static void draw_menu_line(
    SoundUi *ui,
    const char *text,
    int left,
    int top,
    int width,
    int scale,
    bool selected
) {
    if (selected) {
        fill_rect(
            ui,
            left - 3 * scale,
            top - 2 * scale,
            width + 6 * scale,
            (SOUND_UI_GLYPH_HEIGHT + 4) * scale,
            menu_selected_color
        );
    }

    draw_text_scaled(
        ui,
        text,
        left,
        top,
        scale,
        selected ? menu_title_color : axis_text_color
    );
}

static void draw_menu_tabs(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int scale
) {
    int gap = 6 * scale;
    int tab_width = (width - gap) / 2;

    draw_menu_line(
        ui,
        "ANALYSIS",
        left,
        top,
        tab_width,
        scale,
        ui->menu_tab == SOUND_UI_MENU_ANALYSIS
    );
    draw_menu_line(
        ui,
        "SETTINGS",
        left + tab_width + gap,
        top,
        tab_width,
        scale,
        ui->menu_tab == SOUND_UI_MENU_SETTINGS
    );
}

static int menu_line_count(const SoundUi *ui) {
    if (ui->menu_tab == SOUND_UI_MENU_ANALYSIS) {
        return sound_app_mode_count() + 9;
    }

    return sound_colormap_count() + sound_recording_format_count() + 10;
}

static int draw_analysis_menu(
    SoundUi *ui,
    SoundAppMode mode,
    bool sst_enabled,
    int left,
    int top,
    int width,
    int scale,
    int line_height
) {
    int y = top;
    int mode_count = sound_app_mode_count();

    draw_text_scaled(ui, "ANALYSIS", left, y, scale, menu_title_color);
    y += line_height;

    for (int i = 0; i < mode_count; ++i) {
        SoundAppMode item = sound_app_mode_at(i);
        char line[96];

        (void)snprintf(
            line,
            sizeof(line),
            "%d %s",
            i + 1,
            sound_app_mode_title(item, item == SOUND_APP_MODE_TONAL && sst_enabled)
        );
        draw_menu_line(ui, line, left, y, width, scale, item == mode);
        y += line_height;
    }

    y += line_height;
    draw_text_scaled(ui, "S SST  TAB SETTINGS", left, y, scale, axis_text_color);
    return y + line_height;
}

static int draw_settings_menu(
    SoundUi *ui,
    int panel_left,
    int panel_width,
    int left,
    int top,
    int width,
    int padding,
    int scale,
    int line_height
) {
    int y = top;
    int colormap_count = sound_colormap_count();
    int recording_format_count = sound_recording_format_count();

    draw_text_scaled(ui, "COLORMAP", left, y, scale, menu_title_color);
    y += line_height;

    for (int i = 0; i < colormap_count; ++i) {
        SoundColormap item = sound_colormap_at(i);
        const char *name = sound_colormap_name(item);
        int preview_width = 76 * scale;
        int preview_height = 5 * scale;
        int preview_left = panel_left + panel_width - padding - preview_width;
        int preview_top = y + scale;

        draw_menu_line(ui, name, left, y, width, scale, item == ui->colormap);
        draw_colormap_preview(ui, item, preview_left, preview_top, preview_width, preview_height);
        y += line_height;
    }

    y += line_height;
    draw_text_scaled(ui, "RECORDING FORMAT", left, y, scale, menu_title_color);
    y += line_height;

    for (int i = 0; i < recording_format_count; ++i) {
        SoundRecordingFormat item = sound_recording_format_at(i);

        draw_menu_line(
            ui,
            sound_recording_format_name(item),
            left,
            y,
            width,
            scale,
            item == ui->recording_format
        );
        y += line_height;
    }

    y += line_height;
    draw_text_scaled(ui, "C COLOR  F FORMAT", left, y, scale, axis_text_color);
    return y + line_height;
}

void sound_ui_draw_menu(
    SoundUi *ui,
    SoundAppMode mode,
    bool sst_enabled,
    bool recording_enabled
) {
    if (!ui->menu_open) {
        return;
    }

    int line_count = menu_line_count(ui);
    int scale = menu_text_scale(ui, line_count);
    int line_height = (SOUND_UI_GLYPH_HEIGHT + 4) * scale;
    int padding = 14 * scale;
    int panel_width = ui->width - 24 * scale;

    if (panel_width > 430 * scale) {
        panel_width = 430 * scale;
    }

    int panel_height = line_count * line_height + padding * 2;
    if (panel_height > ui->height - 12 * scale) {
        panel_height = ui->height - 12 * scale;
    }

    int panel_left = (ui->width - panel_width) / 2;
    int panel_top = (ui->height - panel_height) / 2;
    int text_left = panel_left + padding;
    int text_top = panel_top + padding;
    int text_width = panel_width - padding * 2;
    int y = text_top;

    dim_screen(ui);
    fill_rect(ui, panel_left, panel_top, panel_width, panel_height, menu_panel_color);
    draw_rect_outline(ui, panel_left, panel_top, panel_width, panel_height, scale, menu_border_color);

    draw_text_scaled(ui, "SOUNDS MENU", text_left, y, scale, menu_title_color);
    y += line_height;
    draw_text_scaled(ui, "TAB SWITCH  M CLOSE  Q QUIT", text_left, y, scale, axis_text_color);
    y += line_height;

    draw_menu_tabs(ui, text_left, y, text_width, scale);
    y += line_height * 2;

    if (ui->menu_tab == SOUND_UI_MENU_ANALYSIS) {
        y = draw_analysis_menu(
            ui,
            mode,
            sst_enabled,
            text_left,
            y,
            text_width,
            scale,
            line_height
        );
    } else {
        y = draw_settings_menu(
            ui,
            panel_left,
            panel_width,
            text_left,
            y,
            text_width,
            padding,
            scale,
            line_height
        );
    }

    y += line_height;
    draw_text_scaled(
        ui,
        recording_enabled ? "R RECORDING ON" : "R RECORDING OFF",
        text_left,
        y,
        scale,
        recording_enabled ? menu_recording_color : axis_text_color
    );
}

void sound_ui_draw_banner(
    SoundUi *ui,
    SoundAppMode mode,
    bool sst_enabled,
    bool recording_enabled
) {
    const char *text = sound_app_mode_banner(mode, sst_enabled);
    char status[96];
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

    (void)snprintf(
        status,
        sizeof(status),
        "MAP %s  %s  REC %s",
        sound_colormap_name(ui->colormap),
        sound_recording_format_name(ui->recording_format),
        recording_enabled ? "ON" : "OFF"
    );

    int status_width = sound_ui_text_width_pixels(status, scale);
    int status_left = ui->width - status_width - 6 * scale;
    int mode_width = sound_ui_text_width_pixels(text, scale);

    if (status_left > text_left + mode_width + 8 * scale) {
        draw_text(
            ui,
            status,
            status_left,
            text_top,
            recording_enabled ? menu_recording_color : axis_text_color
        );
    }
}

void sound_ui_set_title(SoundUi *ui, const SoundUiTitle *title) {
    char text[192];

    (void)snprintf(
        text,
        sizeof(text),
        "Sounds   RMS %.1f dBFS   peak %.1f dBFS   %.0f Hz   %s   %s   %s   rec %s   %.1f-%.0f Hz",
        amplitude_db(title->rms),
        amplitude_db(title->peak),
        title->sample_rate,
        sound_app_mode_title(title->mode, title->sst_enabled),
        sound_colormap_name(title->colormap),
        sound_recording_format_name(title->recording_format),
        title->recording_enabled ? "on" : "off",
        title->min_hz,
        title->max_hz
    );
    (void)SDL_SetWindowTitle(ui->window, text);
}
