#ifndef SOUNDS_UI_FONT_H
#define SOUNDS_UI_FONT_H

#include <stdint.h>

enum {
    SOUND_UI_GLYPH_WIDTH = 5,
    SOUND_UI_GLYPH_HEIGHT = 7,
};

const uint8_t *sound_ui_glyph_bitmap(char character);
int sound_ui_text_width_pixels(const char *text, int scale);

#endif
