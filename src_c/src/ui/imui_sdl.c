#include "imui_sdl.h"

#include "font.h"
#include "internal.h"

static int rounded_float(float value) {
    if (value >= 0.0F) {
        return (int)(value + 0.5F);
    }

    return (int)(value - 0.5F);
}

static int wheel_units(float value) {
    int units = rounded_float(value);

    if (units == 0 && value > 0.0F) {
        return 1;
    }

    if (units == 0 && value < 0.0F) {
        return -1;
    }

    return units;
}

static SoundImuiRect clipped_rect(SoundImuiSdlDraw *adapter, SoundImuiRect rect) {
    if (adapter->clip_count <= 0) {
        return rect;
    }

    SoundImuiRect clip = adapter->clips[adapter->clip_count - 1];
    int left = rect.x > clip.x ? rect.x : clip.x;
    int top = rect.y > clip.y ? rect.y : clip.y;
    int right = rect.x + rect.width < clip.x + clip.width ?
        rect.x + rect.width :
        clip.x + clip.width;
    int bottom = rect.y + rect.height < clip.y + clip.height ?
        rect.y + rect.height :
        clip.y + clip.height;

    if (right <= left || bottom <= top) {
        return (SoundImuiRect){.x = left, .y = top, .width = 0, .height = 0};
    }

    return (SoundImuiRect){
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };
}

static bool rect_visible(SoundImuiRect rect) {
    return rect.width > 0 && rect.height > 0;
}

static void sdl_fill_rect(void *data, SoundImuiRect rect, uint32_t color) {
    SoundImuiSdlDraw *adapter = data;
    SoundImuiRect clipped = clipped_rect(adapter, rect);

    if (!adapter->ui || !rect_visible(clipped)) {
        return;
    }

    sound_ui_fill_rect(
        adapter->ui,
        clipped.x,
        clipped.y,
        clipped.width,
        clipped.height,
        color
    );
}

static void sdl_rect_outline(
    void *data,
    SoundImuiRect rect,
    int thickness,
    uint32_t color
) {
    SoundImuiSdlDraw *adapter = data;

    if (!adapter->ui || thickness <= 0) {
        return;
    }

    if (adapter->clip_count <= 0) {
        sound_ui_draw_rect_outline(
            adapter->ui,
            rect.x,
            rect.y,
            rect.width,
            rect.height,
            thickness,
            color
        );
        return;
    }

    sdl_fill_rect(
        data,
        (SoundImuiRect){.x = rect.x, .y = rect.y, .width = rect.width, .height = thickness},
        color
    );
    sdl_fill_rect(
        data,
        (SoundImuiRect){
            .x = rect.x,
            .y = rect.y + rect.height - thickness,
            .width = rect.width,
            .height = thickness,
        },
        color
    );
    sdl_fill_rect(
        data,
        (SoundImuiRect){.x = rect.x, .y = rect.y, .width = thickness, .height = rect.height},
        color
    );
    sdl_fill_rect(
        data,
        (SoundImuiRect){
            .x = rect.x + rect.width - thickness,
            .y = rect.y,
            .width = thickness,
            .height = rect.height,
        },
        color
    );
}

static void sdl_text(
    void *data,
    const char *text,
    int x,
    int y,
    int scale,
    uint32_t color
) {
    SoundImuiSdlDraw *adapter = data;

    if (!adapter->ui) {
        return;
    }

    SoundImuiRect bounds = {
        .x = x,
        .y = y,
        .width = sound_ui_text_width_pixels(text, scale),
        .height = SOUND_UI_GLYPH_HEIGHT * scale,
    };
    SoundImuiRect clipped = clipped_rect(adapter, bounds);

    if (!rect_visible(clipped)) {
        return;
    }

    sound_ui_draw_text_scaled(adapter->ui, text, x, y, scale, color);
}

static int sdl_text_width(void *data, const char *text, int scale) {
    (void)data;
    return sound_ui_text_width_pixels(text, scale);
}

static int sdl_text_height(void *data, int scale) {
    (void)data;
    return SOUND_UI_GLYPH_HEIGHT * scale;
}

static void sdl_push_clip(void *data, SoundImuiRect rect) {
    SoundImuiSdlDraw *adapter = data;

    if (adapter->clip_count >= SOUND_IMUI_CLIP_STACK_CAPACITY) {
        return;
    }

    adapter->clips[adapter->clip_count] = rect;
    ++adapter->clip_count;
}

static void sdl_pop_clip(void *data) {
    SoundImuiSdlDraw *adapter = data;

    if (adapter->clip_count > 0) {
        --adapter->clip_count;
    }
}

void sound_imui_sdl_input_begin_frame(SoundImuiInput *input) {
    sound_imui_input_begin_frame(input);
}

void sound_imui_sdl_handle_event(SoundImuiInput *input, const SDL_Event *event) {
    if (!input || !event) {
        return;
    }

    switch (event->type) {
        case SDL_EVENT_MOUSE_MOTION:
            input->mouse_x = rounded_float(event->motion.x);
            input->mouse_y = rounded_float(event->motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            input->mouse_x = rounded_float(event->button.x);
            input->mouse_y = rounded_float(event->button.y);
            if (event->button.button == SDL_BUTTON_LEFT) {
                input->mouse_left_down = true;
                input->mouse_left_pressed = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            input->mouse_x = rounded_float(event->button.x);
            input->mouse_y = rounded_float(event->button.y);
            if (event->button.button == SDL_BUTTON_LEFT) {
                input->mouse_left_down = false;
                input->mouse_left_released = true;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            input->wheel_y += wheel_units(event->wheel.y);
            break;
        case SDL_EVENT_TEXT_INPUT:
            sound_imui_input_append_text(input, event->text.text);
            break;
        case SDL_EVENT_KEY_DOWN:
            switch (event->key.key) {
                case SDLK_LEFT:
                    input->key_left = true;
                    break;
                case SDLK_RIGHT:
                    input->key_right = true;
                    break;
                case SDLK_HOME:
                    input->key_home = true;
                    break;
                case SDLK_END:
                    input->key_end = true;
                    break;
                case SDLK_BACKSPACE:
                    input->key_backspace = true;
                    break;
                case SDLK_DELETE:
                    input->key_delete = true;
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    input->key_enter = true;
                    break;
                case SDLK_ESCAPE:
                    input->key_escape = true;
                    break;
                case SDLK_TAB:
                    input->key_tab = true;
                    break;
                case SDLK_UP:
                    input->key_up = true;
                    break;
                case SDLK_DOWN:
                    input->key_down = true;
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

SoundImuiDraw sound_imui_sdl_draw(SoundImuiSdlDraw *adapter, SoundUi *ui) {
    adapter->ui = ui;
    adapter->clip_count = 0;

    return (SoundImuiDraw){
        .context = adapter,
        .fill_rect = sdl_fill_rect,
        .rect_outline = sdl_rect_outline,
        .text = sdl_text,
        .text_width = sdl_text_width,
        .text_height = sdl_text_height,
        .push_clip = sdl_push_clip,
        .pop_clip = sdl_pop_clip,
    };
}
