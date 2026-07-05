#include "imui.h"

#include <limits.h>
#include <string.h>

enum {
    SOUND_IMUI_COLOR_BACKGROUND = 0x0A1018,
    SOUND_IMUI_COLOR_PANEL = 0x101722,
    SOUND_IMUI_COLOR_HOVER = 0x17243A,
    SOUND_IMUI_COLOR_ACTIVE = 0x233752,
    SOUND_IMUI_COLOR_SELECTED = 0x17243A,
    SOUND_IMUI_COLOR_BORDER = 0x5F7394,
    SOUND_IMUI_COLOR_TEXT = 0xD7E8FF,
    SOUND_IMUI_COLOR_TEXT_DIM = 0x8FA3BF,
    SOUND_IMUI_COLOR_ACCENT = 0x97E6B0,
    SOUND_IMUI_COLOR_CURSOR = 0xF4F8FF,
};

static const uint32_t fnv_offset_basis = 2166136261U;
static const uint32_t fnv_prime = 16777619U;

typedef struct SoundImuiWidget {
    bool visible;
    bool hovered;
    bool active;
    bool fired;
} SoundImuiWidget;

static int clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static uint32_t hash_byte(uint32_t hash, unsigned char value) {
    hash ^= value;
    hash *= fnv_prime;
    return hash;
}

static uint32_t hash_string(uint32_t seed, const char *text) {
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    uint32_t hash = seed;

    while (*cursor != '\0') {
        hash = hash_byte(hash, *cursor);
        ++cursor;
    }

    hash = hash_byte(hash, 0U);
    return hash == 0U ? 1U : hash;
}

static uint32_t current_id_seed(const SoundImui *context) {
    if (context->id_depth <= 0) {
        return fnv_offset_basis;
    }

    return context->id_stack[context->id_depth - 1];
}

static int rect_right(SoundImuiRect rect) {
    return rect.x + rect.width;
}

static int rect_bottom(SoundImuiRect rect) {
    return rect.y + rect.height;
}

static bool rect_empty(SoundImuiRect rect) {
    return rect.width <= 0 || rect.height <= 0;
}

bool sound_imui_rect_contains(SoundImuiRect rect, int x, int y) {
    return !rect_empty(rect) &&
        x >= rect.x &&
        y >= rect.y &&
        x < rect_right(rect) &&
        y < rect_bottom(rect);
}

static bool rect_intersects(SoundImuiRect first, SoundImuiRect second) {
    return !rect_empty(first) &&
        !rect_empty(second) &&
        first.x < rect_right(second) &&
        rect_right(first) > second.x &&
        first.y < rect_bottom(second) &&
        rect_bottom(first) > second.y;
}

static SoundImuiRect rect_intersection(SoundImuiRect first, SoundImuiRect second) {
    int left = first.x > second.x ? first.x : second.x;
    int top = first.y > second.y ? first.y : second.y;
    int right = rect_right(first) < rect_right(second) ?
        rect_right(first) :
        rect_right(second);
    int bottom = rect_bottom(first) < rect_bottom(second) ?
        rect_bottom(first) :
        rect_bottom(second);

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

static SoundImuiRect current_clip(const SoundImui *context) {
    if (context->clip_depth <= 0) {
        return (SoundImuiRect){
            .x = -0x10000000,
            .y = -0x10000000,
            .width = 0x20000000,
            .height = 0x20000000,
        };
    }

    return context->clip_stack[context->clip_depth - 1];
}

static bool widget_visible(const SoundImui *context, SoundImuiRect rect) {
    return rect_intersects(rect, current_clip(context));
}

static bool mouse_inside_widget(const SoundImui *context, SoundImuiRect rect) {
    if (!context->input) {
        return false;
    }

    SoundImuiRect clip = current_clip(context);
    return sound_imui_rect_contains(
            rect,
            context->input->mouse_x,
            context->input->mouse_y
        ) &&
        sound_imui_rect_contains(
            clip,
            context->input->mouse_x,
            context->input->mouse_y
        );
}

static int sound_imui_text_scale(const SoundImui *context) {
    int scale = context->text_scale;

    if (scale <= 0) {
        scale = 1;
    }

    return clamp_int(scale, 1, 8);
}

static int sound_imui_padding(const SoundImui *context) {
    return 6 * sound_imui_text_scale(context);
}

static int fallback_text_width(const char *text, int scale) {
    size_t length = strlen(text ? text : "");
    int glyphs = length > (size_t)(0x7FFFFFFF / 6) ? 0x7FFFFFFF / 6 : (int)length;
    return glyphs > 0 ? (glyphs * 6 - 1) * scale : 0;
}

static int draw_text_width(const SoundImui *context, const char *text, int scale) {
    if (context->draw.text_width) {
        return context->draw.text_width(context->draw.context, text ? text : "", scale);
    }

    return fallback_text_width(text, scale);
}

static int draw_text_height(const SoundImui *context, int scale) {
    if (context->draw.text_height) {
        return context->draw.text_height(context->draw.context, scale);
    }

    return 7 * scale;
}

static void draw_fill(SoundImui *context, SoundImuiRect rect, uint32_t color) {
    if (!context->draw.fill_rect || !widget_visible(context, rect)) {
        return;
    }

    context->draw.fill_rect(context->draw.context, rect, color);
}

static void draw_outline(
    SoundImui *context,
    SoundImuiRect rect,
    int thickness,
    uint32_t color
) {
    if (!context->draw.rect_outline || !widget_visible(context, rect)) {
        return;
    }

    context->draw.rect_outline(context->draw.context, rect, thickness, color);
}

static void draw_text(
    SoundImui *context,
    const char *text,
    int x,
    int y,
    int scale,
    uint32_t color
) {
    if (!context->draw.text) {
        return;
    }

    SoundImuiRect bounds = {
        .x = x,
        .y = y,
        .width = draw_text_width(context, text, scale),
        .height = draw_text_height(context, scale),
    };

    if (!widget_visible(context, bounds)) {
        return;
    }

    context->draw.text(context->draw.context, text ? text : "", x, y, scale, color);
}

static SoundImuiState *state_for_id(SoundImui *context, uint32_t id) {
    int start = (int)(id % SOUND_IMUI_STATE_CAPACITY);

    for (int offset = 0; offset < SOUND_IMUI_STATE_CAPACITY; ++offset) {
        int index = (start + offset) % SOUND_IMUI_STATE_CAPACITY;

        if (context->states[index].id == id) {
            return &context->states[index];
        }

        if (context->states[index].id == 0U) {
            context->states[index].id = id;
            return &context->states[index];
        }
    }

    return NULL;
}

static SoundImuiWidget widget_update(
    SoundImui *context,
    uint32_t id,
    SoundImuiRect rect
) {
    SoundImuiWidget widget = {
        .visible = widget_visible(context, rect),
        .hovered = false,
        .active = context->active_id == id,
        .fired = false,
    };

    bool interactive = context->input != NULL;
    widget.active = interactive && widget.active;

    bool captures_mouse =
        interactive && (context->active_id == 0U || context->active_id == id);
    if (captures_mouse &&
        mouse_inside_widget(context, rect) &&
        (context->hover_id == 0U || context->hover_id == id)) {
        context->hover_id = id;
        widget.hovered = true;
    }

    if (widget.hovered &&
        context->input->mouse_left_pressed &&
        context->active_id == 0U) {
        context->active_id = id;
        widget.active = true;
    }

    if (interactive && context->active_id == id) {
        widget.active = true;
    }

    if (widget.active &&
        widget.hovered &&
        context->input->mouse_left_released) {
        widget.fired = true;
    }

    return widget;
}

static uint32_t button_color(SoundImuiWidget widget) {
    if (widget.active) {
        return SOUND_IMUI_COLOR_ACTIVE;
    }

    if (widget.hovered) {
        return SOUND_IMUI_COLOR_HOVER;
    }

    return SOUND_IMUI_COLOR_PANEL;
}

static void draw_text_center(
    SoundImui *context,
    SoundImuiRect rect,
    const char *text,
    uint32_t color
) {
    int scale = sound_imui_text_scale(context);
    int padding = sound_imui_padding(context);
    int text_width = draw_text_width(context, text, scale);
    int text_height = draw_text_height(context, scale);
    int x = rect.x + (rect.width - text_width) / 2;
    int y = rect.y + (rect.height - text_height) / 2;

    if (x < rect.x + padding) {
        x = rect.x + padding;
    }

    draw_text(context, text, x, y, scale, color);
}

static size_t bounded_length(char *buffer, size_t capacity) {
    size_t length = 0;

    while (length < capacity && buffer[length] != '\0') {
        ++length;
    }

    if (length == capacity && capacity > 0U) {
        buffer[capacity - 1U] = '\0';
        length = capacity - 1U;
    }

    return length;
}

static int utf8_step(const char *text, size_t length, size_t offset) {
    if (offset >= length) {
        return 0;
    }

    unsigned char lead = (unsigned char)text[offset];
    int size = 1;

    if ((lead & 0x80U) == 0U) {
        size = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
        size = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
        size = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
        size = 4;
    }

    if (offset + (size_t)size > length) {
        return 1;
    }

    return size;
}

static int utf8_next(const char *text, size_t length, int cursor) {
    size_t offset = cursor < 0 ? 0U : (size_t)cursor;
    return (int)(offset + (size_t)utf8_step(text, length, offset));
}

static int utf8_previous(const char *text, int cursor) {
    int previous = cursor - 1;

    while (previous > 0 && (((unsigned char)text[previous] & 0xC0U) == 0x80U)) {
        --previous;
    }

    return previous < 0 ? 0 : previous;
}

static int utf8_codepoint_count(const char *text, int bytes) {
    int count = 0;
    int cursor = 0;
    size_t length = bytes < 0 ? 0U : (size_t)bytes;

    while ((size_t)cursor < length) {
        cursor = utf8_next(text, length, cursor);
        ++count;
    }

    return count;
}

static int text_prefix_width(SoundImui *context, const char *text, int bytes) {
    size_t length = bytes < 0 ? 0U : (size_t)bytes;
    int scale = sound_imui_text_scale(context);

    if (length >= SOUND_IMUI_TEXT_SCRATCH_CAPACITY) {
        /* Long prefixes are measured at the scratch limit. */
        length = SOUND_IMUI_TEXT_SCRATCH_CAPACITY - 1U;
    }

    memcpy(context->text_scratch, text ? text : "", length);
    context->text_scratch[length] = '\0';

    if (context->draw.text_width) {
        return context->draw.text_width(
            context->draw.context,
            context->text_scratch,
            scale
        );
    }

    return utf8_codepoint_count(context->text_scratch, (int)length) * 6 * scale;
}

static int cursor_from_x(
    SoundImui *context,
    const char *buffer,
    size_t length,
    int text_scroll,
    SoundImuiRect rect,
    int mouse_x
) {
    int relative_x = mouse_x - rect.x - sound_imui_padding(context) + text_scroll;
    int cursor = 0;

    if (relative_x <= 0) {
        return 0;
    }

    while ((size_t)cursor < length) {
        int next = utf8_next(buffer, length, cursor);
        int cursor_width = text_prefix_width(context, buffer, cursor);
        int next_width = text_prefix_width(context, buffer, next);
        int midpoint = cursor_width + (next_width - cursor_width) / 2;

        if (relative_x < midpoint) {
            return cursor;
        }

        cursor = next;
    }

    return (int)length;
}

static void delete_range(char *buffer, size_t length, int first, int last) {
    if (first < 0) {
        first = 0;
    }

    if (last < first) {
        last = first;
    }

    if ((size_t)last > length) {
        last = (int)length;
    }

    memmove(
        buffer + first,
        buffer + last,
        length - (size_t)last + 1U
    );
}

static bool append_codepoint(
    char *buffer,
    size_t capacity,
    int *cursor,
    const char *text,
    int bytes
) {
    size_t length = bounded_length(buffer, capacity);

    if (bytes <= 0 || (size_t)bytes >= capacity || length + (size_t)bytes >= capacity) {
        return false;
    }

    memmove(
        buffer + *cursor + bytes,
        buffer + *cursor,
        length - (size_t)*cursor + 1U
    );
    memcpy(buffer + *cursor, text, (size_t)bytes);
    *cursor += bytes;
    return true;
}

static void insert_text(char *buffer, size_t capacity, int *cursor, const char *text) {
    size_t source_length = strlen(text ? text : "");
    size_t source_offset = 0;

    while (source_offset < source_length) {
        int bytes = utf8_step(text, source_length, source_offset);

        if (!append_codepoint(buffer, capacity, cursor, text + source_offset, bytes)) {
            return;
        }

        source_offset += (size_t)bytes;
    }
}

static void scroll_cursor_into_view(
    SoundImui *context,
    SoundImuiState *state,
    SoundImuiRect rect,
    const char *buffer
) {
    int padding = sound_imui_padding(context);
    int view_width = rect.width - padding * 2;
    int cursor_x = text_prefix_width(context, buffer, state->text_cursor);

    if (view_width < 1) {
        view_width = 1;
    }

    if (cursor_x - state->text_scroll > view_width) {
        state->text_scroll = cursor_x - view_width;
    }

    if (cursor_x < state->text_scroll) {
        state->text_scroll = cursor_x;
    }

    if (state->text_scroll < 0) {
        state->text_scroll = 0;
    }
}

void sound_imui_input_begin_frame(SoundImuiInput *input) {
    if (!input) {
        return;
    }

    input->wheel_y = 0;
    input->mouse_left_pressed = false;
    input->mouse_left_released = false;
    input->key_left = false;
    input->key_right = false;
    input->key_home = false;
    input->key_end = false;
    input->key_backspace = false;
    input->key_delete = false;
    input->key_enter = false;
    input->key_escape = false;
    input->key_tab = false;
    input->key_up = false;
    input->key_down = false;
    input->text[0] = '\0';
}

void sound_imui_input_append_text(SoundImuiInput *input, const char *text) {
    if (!input || !text) {
        return;
    }

    size_t length = strlen(input->text);
    size_t source_length = strlen(text);
    size_t source_offset = 0;

    while (source_offset < source_length) {
        int bytes = utf8_step(text, source_length, source_offset);

        if (length + (size_t)bytes >= sizeof(input->text)) {
            return;
        }

        memcpy(input->text + length, text + source_offset, (size_t)bytes);
        length += (size_t)bytes;
        input->text[length] = '\0';
        source_offset += (size_t)bytes;
    }
}

SoundImuiRect sound_imui_rect(int x, int y, int width, int height) {
    return (SoundImuiRect){
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
}

void sound_imui_set_draw(SoundImui *context, SoundImuiDraw draw) {
    context->draw = draw;
}

void sound_imui_set_text_scale(SoundImui *context, int scale) {
    if (scale <= 0) {
        scale = 1;
    }

    context->text_scale = clamp_int(scale, 1, 8);
}

void sound_imui_begin(SoundImui *context, const SoundImuiInput *input) {
    context->input = input;
    context->hover_id = 0U;
    context->clip_depth = 0;

    if (input && input->key_escape) {
        context->focus_id = 0U;
    }
}

void sound_imui_end(SoundImui *context) {
    if (context->input && context->input->mouse_left_released) {
        context->active_id = 0U;
    }

    context->clip_depth = 0;
    context->input = NULL;
}

uint32_t sound_imui_id(SoundImui *context, const char *name) {
    return hash_string(current_id_seed(context), name);
}

void sound_imui_push_id(SoundImui *context, const char *name) {
    if (context->id_depth >= SOUND_IMUI_ID_STACK_CAPACITY) {
        return;
    }

    context->id_stack[context->id_depth] = sound_imui_id(context, name);
    ++context->id_depth;
}

void sound_imui_pop_id(SoundImui *context) {
    if (context->id_depth > 0) {
        --context->id_depth;
    }
}

uint32_t sound_imui_hover_id(const SoundImui *context) {
    return context->hover_id;
}

uint32_t sound_imui_active_id(const SoundImui *context) {
    return context->active_id;
}

uint32_t sound_imui_focus_id(const SoundImui *context) {
    return context->focus_id;
}

void sound_imui_focus_text_field(
    SoundImui *context,
    const char *name,
    const char *text
) {
    uint32_t id = sound_imui_id(context, name);
    SoundImuiState *state = state_for_id(context, id);
    size_t length = strlen(text ? text : "");

    context->focus_id = id;
    if (!state) {
        return;
    }

    state->text_cursor = length > (size_t)INT_MAX ? INT_MAX : (int)length;
    state->text_scroll = 0;
}

static void sound_imui_push_clip(SoundImui *context, SoundImuiRect rect) {
    SoundImuiRect clip = rect_intersection(current_clip(context), rect);

    if (context->clip_depth < SOUND_IMUI_CLIP_STACK_CAPACITY) {
        context->clip_stack[context->clip_depth] = clip;
        ++context->clip_depth;
    }

    if (context->draw.push_clip) {
        context->draw.push_clip(context->draw.context, clip);
    }
}

static void sound_imui_pop_clip(SoundImui *context) {
    if (context->clip_depth > 0) {
        --context->clip_depth;
    }

    if (context->draw.pop_clip) {
        context->draw.pop_clip(context->draw.context);
    }
}

static bool button_rect_id(
    SoundImui *context,
    const char *name,
    const char *label,
    SoundImuiRect rect
) {
    uint32_t id = sound_imui_id(context, name);
    SoundImuiWidget widget = widget_update(context, id, rect);

    if (widget.visible) {
        draw_fill(context, rect, button_color(widget));
        draw_outline(context, rect, 1, SOUND_IMUI_COLOR_BORDER);
        draw_text_center(context, rect, label, SOUND_IMUI_COLOR_TEXT);
    }

    return widget.fired;
}

bool sound_imui_button_rect_id(
    SoundImui *context,
    const char *name,
    const char *label,
    SoundImuiRect rect
) {
    return button_rect_id(context, name, label, rect);
}

bool sound_imui_hit_rect(
    SoundImui *context,
    const char *name,
    SoundImuiRect rect,
    bool *hovered,
    bool *active
) {
    uint32_t id = sound_imui_id(context, name);
    SoundImuiWidget widget = widget_update(context, id, rect);

    if (hovered) {
        *hovered = widget.hovered;
    }

    if (active) {
        *active = widget.active;
    }

    return widget.fired;
}

bool sound_imui_list_row_id(
    SoundImui *context,
    const char *name,
    const char *label,
    bool selected,
    SoundImuiRect rect
) {
    uint32_t id = sound_imui_id(context, name);
    SoundImuiWidget widget = widget_update(context, id, rect);

    if (widget.visible) {
        uint32_t fill = SOUND_IMUI_COLOR_BACKGROUND;
        int scale = sound_imui_text_scale(context);
        int text_y = rect.y + (rect.height - draw_text_height(context, scale)) / 2;

        if (selected) {
            fill = SOUND_IMUI_COLOR_SELECTED;
        } else if (widget.hovered) {
            fill = SOUND_IMUI_COLOR_HOVER;
        }

        draw_fill(context, rect, fill);
        draw_text(
            context,
            label,
            rect.x + 3 * scale,
            text_y,
            scale,
            selected ? SOUND_IMUI_COLOR_TEXT : SOUND_IMUI_COLOR_TEXT_DIM
        );
    }

    return widget.fired;
}

bool sound_imui_text_field_rect(
    SoundImui *context,
    const char *name,
    char *buffer,
    size_t capacity,
    SoundImuiRect rect
) {
    uint32_t id = sound_imui_id(context, name);
    SoundImuiState *state = state_for_id(context, id);
    SoundImuiWidget widget = widget_update(context, id, rect);
    bool committed = false;

    if (!buffer || capacity == 0U || !state) {
        return false;
    }

    size_t length = bounded_length(buffer, capacity);
    state->text_cursor = clamp_int(state->text_cursor, 0, (int)length);

    if (widget.hovered &&
        context->input &&
        context->input->mouse_left_pressed) {
        context->focus_id = id;
        state->text_cursor = cursor_from_x(
            context,
            buffer,
            length,
            state->text_scroll,
            rect,
            context->input->mouse_x
        );
    }

    if (context->focus_id == id && context->input) {
        if (context->input->key_home) {
            state->text_cursor = 0;
        }

        if (context->input->key_end) {
            state->text_cursor = (int)length;
        }

        if (context->input->key_left) {
            state->text_cursor = utf8_previous(buffer, state->text_cursor);
        }

        if (context->input->key_right) {
            state->text_cursor = utf8_next(buffer, length, state->text_cursor);
        }

        if (context->input->key_backspace && state->text_cursor > 0) {
            int previous = utf8_previous(buffer, state->text_cursor);
            delete_range(buffer, length, previous, state->text_cursor);
            state->text_cursor = previous;
            length = bounded_length(buffer, capacity);
        }

        if (context->input->key_delete && (size_t)state->text_cursor < length) {
            int next = utf8_next(buffer, length, state->text_cursor);
            delete_range(buffer, length, state->text_cursor, next);
            length = bounded_length(buffer, capacity);
        }

        if (context->input->text[0] != '\0') {
            insert_text(buffer, capacity, &state->text_cursor, context->input->text);
            length = bounded_length(buffer, capacity);
        }

        if (context->input->key_enter) {
            committed = true;
        }

        if (context->input->key_escape) {
            context->focus_id = 0U;
        }
    }

    state->text_cursor = clamp_int(state->text_cursor, 0, (int)length);
    scroll_cursor_into_view(context, state, rect, buffer);

    if (widget.visible) {
        bool focused = context->focus_id == id;
        int scale = sound_imui_text_scale(context);
        int padding = sound_imui_padding(context);
        SoundImuiRect inner = {
            .x = rect.x + padding,
            .y = rect.y + 2,
            .width = rect.width - padding * 2,
            .height = rect.height - 4,
        };
        int text_y = rect.y + (rect.height - draw_text_height(context, scale)) / 2;
        int cursor_x = rect.x + padding -
            state->text_scroll +
            text_prefix_width(context, buffer, state->text_cursor);
        SoundImuiRect cursor_rect = {
            .x = cursor_x,
            .y = rect.y + 3,
            .width = 1,
            .height = rect.height - 6,
        };

        draw_fill(context, rect, SOUND_IMUI_COLOR_PANEL);
        draw_outline(
            context,
            rect,
            1,
            focused ? SOUND_IMUI_COLOR_ACCENT : SOUND_IMUI_COLOR_BORDER
        );
        sound_imui_push_clip(context, inner);
        draw_text(
            context,
            buffer,
            rect.x + padding - state->text_scroll,
            text_y,
            scale,
            SOUND_IMUI_COLOR_TEXT
        );

        if (focused) {
            draw_fill(context, cursor_rect, SOUND_IMUI_COLOR_CURSOR);
        }

        sound_imui_pop_clip(context);
    }

    return committed;
}
