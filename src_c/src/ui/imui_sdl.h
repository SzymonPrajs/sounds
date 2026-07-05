#ifndef SOUND_UI_IMUI_SDL_H
#define SOUND_UI_IMUI_SDL_H

#include "imui.h"

#include "sounds/ui.h"

#include <SDL3/SDL.h>

typedef struct SoundImuiSdlDraw {
    SoundUi *ui;
    SoundImuiRect clips[SOUND_IMUI_CLIP_STACK_CAPACITY];
    int clip_count;
} SoundImuiSdlDraw;

void sound_imui_sdl_input_begin_frame(SoundImuiInput *input);
void sound_imui_sdl_handle_event(SoundImuiInput *input, const SDL_Event *event);
SoundImuiDraw sound_imui_sdl_draw(SoundImuiSdlDraw *adapter, SoundUi *ui);

#endif
