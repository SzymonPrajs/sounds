#ifndef SOUND_UI_IMUI_H
#define SOUND_UI_IMUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SOUND_IMUI_TEXT_INPUT_CAPACITY = 128,
    SOUND_IMUI_TEXT_SCRATCH_CAPACITY = 256,
    SOUND_IMUI_ID_STACK_CAPACITY = 16,
    SOUND_IMUI_CLIP_STACK_CAPACITY = 16,
    /* Persistent widget state stores this many id entries. */
    SOUND_IMUI_STATE_CAPACITY = 128,
    SOUND_IMUI_LAYOUT_COLUMN_CAPACITY = 8,
};

typedef struct SoundImuiRect {
    int x;
    int y;
    int width;
    int height;
} SoundImuiRect;

typedef struct SoundImuiInput {
    int mouse_x;
    int mouse_y;
    int wheel_y;
    bool mouse_left_down;
    bool mouse_left_pressed;
    bool mouse_left_released;
    bool key_left;
    bool key_right;
    bool key_home;
    bool key_end;
    bool key_backspace;
    bool key_delete;
    bool key_enter;
    bool key_escape;
    bool key_tab;
    bool key_up;
    bool key_down;
    char text[SOUND_IMUI_TEXT_INPUT_CAPACITY];
} SoundImuiInput;

typedef struct SoundImuiDraw {
    void *context;
    void (*fill_rect)(void *context, SoundImuiRect rect, uint32_t color);
    void (*rect_outline)(void *context, SoundImuiRect rect, int thickness, uint32_t color);
    void (*text)(void *context, const char *text, int x, int y, int scale, uint32_t color);
    int (*text_width)(void *context, const char *text, int scale);
    int (*text_height)(void *context, int scale);
    void (*push_clip)(void *context, SoundImuiRect rect);
    void (*pop_clip)(void *context);
} SoundImuiDraw;

typedef struct SoundImuiState {
    uint32_t id;
    int scroll_offset;
    int text_cursor;
    int text_scroll;
} SoundImuiState;

typedef struct SoundImuiLayout {
    SoundImuiRect rect;
    int row_height;
    int spacing;
    int next_y;
    int column_count;
    int column_index;
    int column_weights[SOUND_IMUI_LAYOUT_COLUMN_CAPACITY];
} SoundImuiLayout;

typedef struct SoundImui {
    const SoundImuiInput *input;
    SoundImuiDraw draw;
    SoundImuiLayout layout;
    SoundImuiState states[SOUND_IMUI_STATE_CAPACITY];
    uint32_t id_stack[SOUND_IMUI_ID_STACK_CAPACITY];
    SoundImuiRect clip_stack[SOUND_IMUI_CLIP_STACK_CAPACITY];
    uint32_t hover_id;
    uint32_t active_id;
    uint32_t focus_id;
    char text_scratch[SOUND_IMUI_TEXT_SCRATCH_CAPACITY];
    int text_scale;
    int id_depth;
    int clip_depth;
} SoundImui;

void sound_imui_input_begin_frame(SoundImuiInput *input);
void sound_imui_input_append_text(SoundImuiInput *input, const char *text);

SoundImuiRect sound_imui_rect(int x, int y, int width, int height);

void sound_imui_set_draw(SoundImui *context, SoundImuiDraw draw);
void sound_imui_set_text_scale(SoundImui *context, int scale);
void sound_imui_begin(SoundImui *context, const SoundImuiInput *input);
void sound_imui_end(SoundImui *context);

uint32_t sound_imui_id(SoundImui *context, const char *name);
void sound_imui_push_id(SoundImui *context, const char *name);
void sound_imui_pop_id(SoundImui *context);

uint32_t sound_imui_hover_id(const SoundImui *context);
uint32_t sound_imui_active_id(const SoundImui *context);
uint32_t sound_imui_focus_id(const SoundImui *context);

void sound_imui_layout_begin(
    SoundImui *context,
    SoundImuiRect rect,
    int row_height,
    int spacing
);
void sound_imui_layout_columns(SoundImui *context, int count, const int *weights);
SoundImuiRect sound_imui_layout_next(SoundImui *context);

void sound_imui_push_clip(SoundImui *context, SoundImuiRect rect);
void sound_imui_pop_clip(SoundImui *context);

void sound_imui_label(SoundImui *context, const char *text);
void sound_imui_label_rect(SoundImui *context, const char *text, SoundImuiRect rect);

bool sound_imui_button(SoundImui *context, const char *label);
bool sound_imui_button_rect(
    SoundImui *context,
    const char *label,
    SoundImuiRect rect
);
bool sound_imui_button_rect_id(
    SoundImui *context,
    const char *name,
    const char *label,
    SoundImuiRect rect
);
bool sound_imui_hit_rect(
    SoundImui *context,
    const char *name,
    SoundImuiRect rect,
    bool *hovered,
    bool *active
);

bool sound_imui_toggle_row(SoundImui *context, const char *label, bool *value);
bool sound_imui_toggle_row_rect(
    SoundImui *context,
    const char *label,
    bool *value,
    SoundImuiRect rect
);

int sound_imui_tab_bar(
    SoundImui *context,
    const char *const *labels,
    int count,
    int selected
);
int sound_imui_tab_bar_rect(
    SoundImui *context,
    const char *const *labels,
    int count,
    int selected,
    SoundImuiRect rect
);

bool sound_imui_list_row(SoundImui *context, const char *label, bool selected);
bool sound_imui_list_row_id(
    SoundImui *context,
    const char *name,
    const char *label,
    bool selected,
    SoundImuiRect rect
);
bool sound_imui_list_row_rect(
    SoundImui *context,
    const char *label,
    bool selected,
    SoundImuiRect rect
);

int sound_imui_scroll_begin(
    SoundImui *context,
    const char *name,
    SoundImuiRect rect,
    int content_height
);
void sound_imui_scroll_end(SoundImui *context);
int sound_imui_scroll_offset(SoundImui *context, const char *name);

bool sound_imui_text_field(
    SoundImui *context,
    const char *name,
    char *buffer,
    size_t capacity
);
bool sound_imui_text_field_rect(
    SoundImui *context,
    const char *name,
    char *buffer,
    size_t capacity,
    SoundImuiRect rect
);

#endif
