#include "../internal.h"

#include "sounds/colormap.h"
#include "../font.h"

#include <math.h>
#include <string.h>

uint32_t *sound_ui_row(SoundUi *ui, int y) {
    return ui->pixels + (size_t)y * (size_t)ui->width;
}

void sound_ui_mark_dirty_rect(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height
) {
    if (!ui || width <= 0 || height <= 0) {
        return;
    }

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

    int right = left + width;
    int bottom = top + height;

    if (!ui->dirty) {
        ui->dirty_left = left;
        ui->dirty_top = top;
        ui->dirty_right = right;
        ui->dirty_bottom = bottom;
        ui->dirty = true;
        return;
    }

    if (left < ui->dirty_left) {
        ui->dirty_left = left;
    }

    if (top < ui->dirty_top) {
        ui->dirty_top = top;
    }

    if (right > ui->dirty_right) {
        ui->dirty_right = right;
    }

    if (bottom > ui->dirty_bottom) {
        ui->dirty_bottom = bottom;
    }
}

void sound_ui_mark_dirty_all(SoundUi *ui) {
    if (ui) {
        sound_ui_mark_dirty_rect(ui, 0, 0, ui->width, ui->height);
    }
}

uint32_t sound_ui_pack_color(SoundColor color) {
    uint32_t red = (uint32_t)lrintf(color.red * 255.0F);
    uint32_t green = (uint32_t)lrintf(color.green * 255.0F);
    uint32_t blue = (uint32_t)lrintf(color.blue * 255.0F);

    return (red << 16) | (green << 8) | blue;
}

uint32_t sound_ui_color_for_db(const SoundUi *ui, float db) {
    float unit = (db - sound_ui_floor_db) / (sound_ui_ceiling_db - sound_ui_floor_db);
    return sound_ui_pack_color(sound_colormap_sample(ui->colormap, unit));
}

double sound_ui_amplitude_db(double value) {
    return 20.0 * log10(fmax(value, 1.0e-12));
}

uint32_t sound_ui_blend_color(uint32_t base, uint32_t over, float amount) {
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

void sound_ui_fill_rows(SoundUi *ui, int from, int rows, uint32_t color) {
    sound_ui_mark_dirty_rect(ui, 0, from, ui->width, rows);

    uint32_t *first = sound_ui_row(ui, from);
    for (int x = 0; x < ui->width; ++x) {
        first[x] = color;
    }

    size_t row_size = sizeof(uint32_t) * (size_t)ui->width;
    for (int y = from + 1; y < from + rows; ++y) {
        memcpy(sound_ui_row(ui, y), first, row_size);
    }
}

void sound_ui_draw_text_scaled(
    SoundUi *ui,
    const char *text,
    int x,
    int y,
    int scale,
    uint32_t color
) {
    int text_width = sound_ui_text_width_pixels(text, scale);
    sound_ui_mark_dirty_rect(ui, x, y, text_width, SOUND_UI_GLYPH_HEIGHT * scale);

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

void sound_ui_draw_text(SoundUi *ui, const char *text, int x, int y, uint32_t color) {
    sound_ui_draw_text_scaled(ui, text, x, y, ui->text_scale, color);
}

void sound_ui_fill_rect(
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

    sound_ui_mark_dirty_rect(ui, left, top, width, height);

    uint32_t *first = sound_ui_row(ui, top) + left;
    for (int x = 0; x < width; ++x) {
        first[x] = color;
    }

    size_t row_size = sizeof(uint32_t) * (size_t)width;
    for (int y = top + 1; y < top + height; ++y) {
        memcpy(sound_ui_row(ui, y) + left, first, row_size);
    }
}

void sound_ui_draw_rect_outline(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height,
    int thickness,
    uint32_t color
) {
    sound_ui_fill_rect(ui, left, top, width, thickness, color);
    sound_ui_fill_rect(ui, left, top + height - thickness, width, thickness, color);
    sound_ui_fill_rect(ui, left, top, thickness, height, color);
    sound_ui_fill_rect(ui, left + width - thickness, top, thickness, height, color);
}

void sound_ui_dim_screen(SoundUi *ui) {
    sound_ui_mark_dirty_all(ui);

    for (int y = 0; y < ui->height; ++y) {
        uint32_t *row = sound_ui_row(ui, y);

        for (int x = 0; x < ui->width; ++x) {
            row[x] = sound_ui_blend_color(row[x], SOUND_UI_BACKGROUND_COLOR, 0.68F);
        }
    }
}
int sound_ui_waveform_y(double value, double gain, int rows) {
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
