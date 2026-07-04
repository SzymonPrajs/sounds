#ifndef SOUNDS_UI_INTERNAL_H
#define SOUNDS_UI_INTERNAL_H

#include "sounds/ui.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>

struct SoundUi {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t *pixels;
    float *bands;
    uint8_t *grid_flags;
    double min_hz;
    double max_hz;
    int width;
    int height;
    int banner_height;
    int waveform_height;
    int spectrogram_top;
    int spectrogram_height;
    int spectrogram_left;
    int spectrogram_origin;
    int text_scale;
    bool sdl_ready;
    bool vsync;
};

uint32_t *sound_ui_row(SoundUi *ui, int y);
void sound_ui_prepare_resized_buffer(SoundUi *ui);

#endif
