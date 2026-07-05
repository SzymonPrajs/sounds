#include "../src/ui/imui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    RECORDING_COMMAND_CAPACITY = 1024,
    RECORDING_TEXT_CAPACITY = 64,
};

typedef enum RecordingCommandKind {
    RECORDING_FILL,
    RECORDING_OUTLINE,
    RECORDING_TEXT,
    RECORDING_PUSH_CLIP,
    RECORDING_POP_CLIP,
} RecordingCommandKind;

typedef struct RecordingCommand {
    RecordingCommandKind kind;
    SoundImuiRect rect;
    char text[RECORDING_TEXT_CAPACITY];
    int scale;
} RecordingCommand;

typedef struct RecordingDraw {
    RecordingCommand commands[RECORDING_COMMAND_CAPACITY];
    int count;
} RecordingDraw;

static bool expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
    }

    return condition;
}

static void recording_append(
    RecordingDraw *recording,
    RecordingCommandKind kind,
    SoundImuiRect rect,
    const char *text,
    int scale
) {
    if (recording->count >= RECORDING_COMMAND_CAPACITY) {
        return;
    }

    RecordingCommand *command = &recording->commands[recording->count];
    ++recording->count;
    command->kind = kind;
    command->rect = rect;
    command->scale = scale;
    command->text[0] = '\0';

    if (text) {
        (void)snprintf(command->text, sizeof(command->text), "%s", text);
    }
}

static void recording_fill_rect(void *data, SoundImuiRect rect, uint32_t color) {
    (void)color;
    recording_append(data, RECORDING_FILL, rect, NULL, 0);
}

static void recording_rect_outline(
    void *data,
    SoundImuiRect rect,
    int thickness,
    uint32_t color
) {
    (void)thickness;
    (void)color;
    recording_append(data, RECORDING_OUTLINE, rect, NULL, 0);
}

static void recording_text(
    void *data,
    const char *text,
    int x,
    int y,
    int scale,
    uint32_t color
) {
    (void)color;
    recording_append(
        data,
        RECORDING_TEXT,
        (SoundImuiRect){.x = x, .y = y, .width = 0, .height = 0},
        text,
        scale
    );
}

static int recording_text_width(void *data, const char *text, int scale) {
    (void)data;
    return (int)strlen(text) * 6 * scale;
}

static int recording_text_height(void *data, int scale) {
    (void)data;
    return 7 * scale;
}

static void recording_push_clip(void *data, SoundImuiRect rect) {
    recording_append(data, RECORDING_PUSH_CLIP, rect, NULL, 0);
}

static void recording_pop_clip(void *data) {
    recording_append(
        data,
        RECORDING_POP_CLIP,
        (SoundImuiRect){.x = 0, .y = 0, .width = 0, .height = 0},
        NULL,
        0
    );
}

static SoundImuiDraw recording_draw(RecordingDraw *recording) {
    recording->count = 0;

    return (SoundImuiDraw){
        .context = recording,
        .fill_rect = recording_fill_rect,
        .rect_outline = recording_rect_outline,
        .text = recording_text,
        .text_width = recording_text_width,
        .text_height = recording_text_height,
        .push_clip = recording_push_clip,
        .pop_clip = recording_pop_clip,
    };
}

static void begin_frame(
    SoundImui *imui,
    RecordingDraw *recording,
    const SoundImuiInput *input
) {
    sound_imui_set_draw(imui, recording_draw(recording));
    sound_imui_begin(imui, input);
}

static int text_command_count(const RecordingDraw *recording, const char *text) {
    int count = 0;

    for (int index = 0; index < recording->count; ++index) {
        const RecordingCommand *command = &recording->commands[index];

        if (command->kind == RECORDING_TEXT && strcmp(command->text, text) == 0) {
            ++count;
        }
    }

    return count;
}

static const RecordingCommand *find_text_command(
    const RecordingDraw *recording,
    const char *text
) {
    for (int index = 0; index < recording->count; ++index) {
        const RecordingCommand *command = &recording->commands[index];

        if (command->kind == RECORDING_TEXT && strcmp(command->text, text) == 0) {
            return command;
        }
    }

    return NULL;
}

static const RecordingCommand *find_cursor_command(const RecordingDraw *recording) {
    for (int index = recording->count - 1; index >= 0; --index) {
        const RecordingCommand *command = &recording->commands[index];

        if (command->kind == RECORDING_FILL && command->rect.width == 1) {
            return command;
        }
    }

    return NULL;
}

static bool check_button(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    SoundImuiRect first = sound_imui_rect(10, 10, 80, 20);
    SoundImuiRect second = sound_imui_rect(120, 10, 80, 20);
    SoundImuiInput input = {.mouse_x = 20, .mouse_y = 15};
    bool ok = true;

    begin_frame(&imui, &recording, &input);
    uint32_t first_id = sound_imui_id(&imui, "play");
    ok = expect(!sound_imui_button_rect(&imui, "play", first), "button hovered frame fired") &&
        expect(sound_imui_hover_id(&imui) == first_id, "button hover id was not set");
    sound_imui_end(&imui);

    input = (SoundImuiInput){
        .mouse_x = 20,
        .mouse_y = 15,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    begin_frame(&imui, &recording, &input);
    ok = expect(!sound_imui_button_rect(&imui, "play", first), "button press fired") &&
        ok;
    sound_imui_end(&imui);
    ok = expect(sound_imui_active_id(&imui) == first_id, "button did not capture mouse") &&
        ok;

    input = (SoundImuiInput){
        .mouse_x = 20,
        .mouse_y = 15,
        .mouse_left_released = true,
    };
    begin_frame(&imui, &recording, &input);
    ok = expect(sound_imui_button_rect(&imui, "play", first), "button release inside did not fire") &&
        ok;
    sound_imui_end(&imui);
    ok = expect(sound_imui_active_id(&imui) == 0U, "button capture did not release") &&
        ok;

    begin_frame(&imui, &recording, &input);
    ok = expect(!sound_imui_button_rect(&imui, "play", first), "button fired twice") &&
        ok;
    sound_imui_end(&imui);

    input = (SoundImuiInput){
        .mouse_x = 20,
        .mouse_y = 15,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    begin_frame(&imui, &recording, &input);
    (void)sound_imui_button_rect(&imui, "play", first);
    sound_imui_end(&imui);

    input = (SoundImuiInput){
        .mouse_x = 130,
        .mouse_y = 15,
        .mouse_left_released = true,
    };
    begin_frame(&imui, &recording, &input);
    ok = expect(!sound_imui_button_rect(&imui, "play", first), "button release outside fired") &&
        ok;
    sound_imui_end(&imui);

    input = (SoundImuiInput){
        .mouse_x = 20,
        .mouse_y = 15,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    begin_frame(&imui, &recording, &input);
    (void)sound_imui_button_rect(&imui, "play", first);
    sound_imui_end(&imui);

    input = (SoundImuiInput){
        .mouse_x = 130,
        .mouse_y = 15,
        .mouse_left_down = true,
    };
    begin_frame(&imui, &recording, &input);
    uint32_t second_id = sound_imui_id(&imui, "stop");
    (void)sound_imui_button_rect(&imui, "play", first);
    (void)sound_imui_button_rect(&imui, "stop", second);
    ok = expect(sound_imui_hover_id(&imui) != second_id, "dragged capture allowed another hover") &&
        ok;
    sound_imui_end(&imui);

    input = (SoundImuiInput){.mouse_x = 130, .mouse_y = 15, .mouse_left_released = true};
    begin_frame(&imui, &recording, &input);
    (void)sound_imui_button_rect(&imui, "play", first);
    sound_imui_end(&imui);

    return ok;
}

static bool check_text_scale(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    SoundImuiInput input = {0};
    SoundImuiRect rect = sound_imui_rect(0, 0, 120, 28);
    bool ok = true;

    sound_imui_set_text_scale(&imui, 2);
    begin_frame(&imui, &recording, &input);
    (void)sound_imui_button_rect(&imui, "scale", rect);
    sound_imui_end(&imui);

    const RecordingCommand *command = find_text_command(&recording, "scale");
    ok = expect(command != NULL, "scaled button label did not draw") && ok;
    ok = expect(command && command->scale == 2, "button label did not use text scale") && ok;

    return ok;
}

static bool check_id_stack(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    SoundImuiRect left_rect = sound_imui_rect(0, 0, 50, 20);
    SoundImuiRect right_rect = sound_imui_rect(60, 0, 50, 20);
    SoundImuiInput input = {
        .mouse_x = 10,
        .mouse_y = 10,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    bool ok = true;

    begin_frame(&imui, &recording, &input);
    sound_imui_push_id(&imui, "left");
    uint32_t left_id = sound_imui_id(&imui, "same");
    (void)sound_imui_button_rect(&imui, "same", left_rect);
    sound_imui_pop_id(&imui);
    sound_imui_push_id(&imui, "right");
    uint32_t right_id = sound_imui_id(&imui, "same");
    (void)sound_imui_button_rect(&imui, "same", right_rect);
    sound_imui_pop_id(&imui);
    sound_imui_end(&imui);

    ok = expect(left_id != right_id, "id stack did not distinguish matching labels") &&
        expect(sound_imui_active_id(&imui) == left_id, "stacked id did not capture correctly") &&
        ok;

    input = (SoundImuiInput){.mouse_x = 70, .mouse_y = 10, .mouse_left_down = true};
    begin_frame(&imui, &recording, &input);
    sound_imui_push_id(&imui, "left");
    (void)sound_imui_button_rect(&imui, "same", left_rect);
    sound_imui_pop_id(&imui);
    sound_imui_push_id(&imui, "right");
    (void)sound_imui_button_rect(&imui, "same", right_rect);
    sound_imui_pop_id(&imui);
    ok = expect(sound_imui_hover_id(&imui) != right_id, "active stacked id did not block hover") &&
        ok;
    sound_imui_end(&imui);

    input = (SoundImuiInput){.mouse_x = 70, .mouse_y = 10, .mouse_left_released = true};
    begin_frame(&imui, &recording, &input);
    sound_imui_push_id(&imui, "left");
    (void)sound_imui_button_rect(&imui, "same", left_rect);
    sound_imui_pop_id(&imui);
    sound_imui_end(&imui);

    return ok;
}

static bool check_tab_bar(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    SoundImuiRect rect = sound_imui_rect(0, 0, 90, 20);
    const char *labels[] = {"one", "two", "three"};
    int selected = 0;
    bool ok = true;

    SoundImuiInput input = {
        .mouse_x = 75,
        .mouse_y = 10,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    begin_frame(&imui, &recording, &input);
    selected = sound_imui_tab_bar_rect(&imui, labels, 3, selected, rect);
    sound_imui_end(&imui);
    ok = expect(selected == 0, "tab press changed selection") && ok;

    input = (SoundImuiInput){.mouse_x = 75, .mouse_y = 10, .mouse_left_released = true};
    begin_frame(&imui, &recording, &input);
    selected = sound_imui_tab_bar_rect(&imui, labels, 3, selected, rect);
    sound_imui_end(&imui);

    return expect(selected == 2, "tab release did not select the clicked tab") && ok;
}

static bool check_list_row_id(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    SoundImuiRect first = sound_imui_rect(0, 0, 100, 20);
    SoundImuiRect second = sound_imui_rect(0, 24, 100, 20);
    SoundImuiInput input = {.mouse_x = 10, .mouse_y = 30};
    bool ok = true;

    begin_frame(&imui, &recording, &input);
    uint32_t first_id = sound_imui_id(&imui, "row-a");
    uint32_t second_id = sound_imui_id(&imui, "row-b");
    (void)sound_imui_list_row_id(&imui, "row-a", "same label", false, first);
    (void)sound_imui_list_row_id(&imui, "row-b", "same label", false, second);
    sound_imui_end(&imui);

    ok = expect(first_id != second_id, "named rows did not get distinct ids") && ok;
    ok = expect(sound_imui_hover_id(&imui) == second_id, "named row hover id was wrong") &&
        ok;

    input = (SoundImuiInput){
        .mouse_x = 10,
        .mouse_y = 10,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    begin_frame(&imui, &recording, &input);
    uint32_t stable_id = sound_imui_id(&imui, "stable-row");
    (void)sound_imui_list_row_id(&imui, "stable-row", "    option", false, first);
    sound_imui_end(&imui);
    ok = expect(sound_imui_active_id(&imui) == stable_id, "named row did not capture") &&
        ok;

    input = (SoundImuiInput){
        .mouse_x = 10,
        .mouse_y = 10,
        .mouse_left_released = true,
    };
    begin_frame(&imui, &recording, &input);
    bool fired = sound_imui_list_row_id(&imui, "stable-row", "SET option", true, first);
    sound_imui_end(&imui);
    ok = expect(fired, "named row did not fire after label changed") && ok;
    ok = expect(sound_imui_active_id(&imui) == 0U, "named row capture did not release") &&
        ok;

    return ok;
}

static int draw_rows(
    SoundImui *imui,
    SoundImuiRect region,
    int row_height,
    int row_count,
    int offset
) {
    int clicked = -1;
    char label[32];

    for (int index = 0; index < row_count; ++index) {
        (void)snprintf(label, sizeof(label), "row %d", index);
        SoundImuiRect row = sound_imui_rect(
            region.x,
            region.y + index * row_height - offset,
            region.width,
            row_height
        );

        if (sound_imui_list_row_rect(imui, label, false, row)) {
            clicked = index;
        }
    }

    return clicked;
}

static bool check_scroll_list(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    SoundImuiRect region = sound_imui_rect(0, 0, 100, 40);
    int offset = 0;
    bool ok = true;

    SoundImuiInput input = {.mouse_x = 10, .mouse_y = 10, .wheel_y = -2};
    begin_frame(&imui, &recording, &input);
    offset = sound_imui_scroll_begin(&imui, "list", region, 100);
    sound_imui_scroll_end(&imui);
    sound_imui_end(&imui);
    ok = expect(offset == 48, "scroll did not move by wheel units") && ok;

    input = (SoundImuiInput){.mouse_x = 10, .mouse_y = 10, .wheel_y = -10};
    begin_frame(&imui, &recording, &input);
    offset = sound_imui_scroll_begin(&imui, "list", region, 100);
    sound_imui_scroll_end(&imui);
    sound_imui_end(&imui);
    ok = expect(offset == 60, "scroll did not clamp at maximum") && ok;

    input = (SoundImuiInput){.mouse_x = 10, .mouse_y = 10, .wheel_y = 10};
    begin_frame(&imui, &recording, &input);
    offset = sound_imui_scroll_begin(&imui, "list", region, 100);
    sound_imui_scroll_end(&imui);
    sound_imui_end(&imui);
    ok = expect(offset == 0, "scroll did not clamp at minimum") && ok;

    input = (SoundImuiInput){.mouse_x = 10, .mouse_y = 10, .wheel_y = -1};
    begin_frame(&imui, &recording, &input);
    offset = sound_imui_scroll_begin(&imui, "list", region, 100);
    sound_imui_scroll_end(&imui);
    sound_imui_end(&imui);
    ok = expect(offset == 24, "scroll did not persist for list hit testing") && ok;

    input = (SoundImuiInput){
        .mouse_x = 10,
        .mouse_y = 7,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    begin_frame(&imui, &recording, &input);
    offset = sound_imui_scroll_begin(&imui, "list", region, 100);
    (void)draw_rows(&imui, region, 10, 10, offset);
    sound_imui_scroll_end(&imui);
    sound_imui_end(&imui);

    input = (SoundImuiInput){.mouse_x = 10, .mouse_y = 7, .mouse_left_released = true};
    begin_frame(&imui, &recording, &input);
    offset = sound_imui_scroll_begin(&imui, "list", region, 100);
    int clicked = draw_rows(&imui, region, 10, 10, offset);
    sound_imui_scroll_end(&imui);
    sound_imui_end(&imui);
    ok = expect(clicked == 3, "scrolled list click selected the wrong row") && ok;

    region = sound_imui_rect(0, 0, 100, 20);
    input = (SoundImuiInput){.mouse_x = 10, .mouse_y = 5};
    begin_frame(&imui, &recording, &input);
    offset = sound_imui_scroll_begin(&imui, "short-list", region, 50);
    (void)draw_rows(&imui, region, 10, 5, offset);
    sound_imui_scroll_end(&imui);
    sound_imui_end(&imui);
    ok = expect(text_command_count(&recording, "row 0") == 1, "visible row 0 did not draw") &&
        expect(text_command_count(&recording, "row 1") == 1, "visible row 1 did not draw") &&
        expect(text_command_count(&recording, "row 2") == 0, "clipped row 2 drew commands") &&
        ok;

    return ok;
}

static bool focus_text_field(
    SoundImui *imui,
    RecordingDraw *recording,
    const char *name,
    char *buffer,
    size_t capacity,
    int mouse_x
) {
    SoundImuiInput input = {
        .mouse_x = mouse_x,
        .mouse_y = 10,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    SoundImuiRect rect = sound_imui_rect(0, 0, 140, 20);

    begin_frame(imui, recording, &input);
    uint32_t id = sound_imui_id(imui, name);
    (void)sound_imui_text_field_rect(imui, name, buffer, capacity, rect);
    sound_imui_end(imui);

    bool focused = expect(sound_imui_focus_id(imui) == id, "text field did not focus on click");

    input = (SoundImuiInput){
        .mouse_x = mouse_x,
        .mouse_y = 10,
        .mouse_left_released = true,
    };
    begin_frame(imui, recording, &input);
    (void)sound_imui_text_field_rect(imui, name, buffer, capacity, rect);
    sound_imui_end(imui);

    return focused;
}

static bool run_text_frame(
    SoundImui *imui,
    RecordingDraw *recording,
    const char *name,
    char *buffer,
    size_t capacity,
    SoundImuiInput input,
    bool *committed
) {
    SoundImuiRect rect = sound_imui_rect(0, 0, 140, 20);

    begin_frame(imui, recording, &input);
    *committed = sound_imui_text_field_rect(imui, name, buffer, capacity, rect);
    sound_imui_end(imui);
    return true;
}

static bool check_text_field_editing(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    char buffer[16] = "";
    bool committed = false;
    bool ok = true;

    ok = focus_text_field(&imui, &recording, "edit", buffer, sizeof(buffer), 80) && ok;

    SoundImuiInput input = {0};
    (void)snprintf(input.text, sizeof(input.text), "ab");
    ok = run_text_frame(
        &imui,
        &recording,
        "edit",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(strcmp(buffer, "ab") == 0, "text input did not insert ASCII") && ok;

    input = (SoundImuiInput){.key_home = true};
    (void)snprintf(input.text, sizeof(input.text), "é");
    ok = run_text_frame(
        &imui,
        &recording,
        "edit",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(strcmp(buffer, "éab") == 0, "UTF-8 text did not insert at cursor") && ok;

    input = (SoundImuiInput){.key_right = true};
    (void)snprintf(input.text, sizeof(input.text), "X");
    ok = run_text_frame(
        &imui,
        &recording,
        "edit",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(strcmp(buffer, "éaXb") == 0, "right arrow did not move by one character") && ok;

    input = (SoundImuiInput){.key_end = true};
    (void)snprintf(input.text, sizeof(input.text), "Z");
    ok = run_text_frame(
        &imui,
        &recording,
        "edit",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(strcmp(buffer, "éaXbZ") == 0, "end key did not move to buffer end") && ok;

    input = (SoundImuiInput){.key_left = true, .key_backspace = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "edit",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(strcmp(buffer, "éaXZ") == 0, "backspace did not delete before cursor") && ok;

    input = (SoundImuiInput){.key_delete = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "edit",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(strcmp(buffer, "éaX") == 0, "delete did not remove cursor character") && ok;

    input = (SoundImuiInput){.key_enter = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "edit",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(committed, "enter did not commit text field") && ok;

    input = (SoundImuiInput){.key_escape = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "edit",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(sound_imui_focus_id(&imui) == 0U, "escape did not drop focus") && ok;

    return ok;
}

static bool check_text_field_boundaries(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    char empty[4] = "";
    char cursor_start[8] = "ab";
    char cursor_end[8] = "ab";
    char limited[6] = "";
    bool committed = false;
    bool ok = true;

    ok = focus_text_field(&imui, &recording, "empty", empty, sizeof(empty), 4) && ok;
    SoundImuiInput input = {.key_backspace = true, .key_delete = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "empty",
        empty,
        sizeof(empty),
        input,
        &committed
    ) && expect(strcmp(empty, "") == 0, "empty boundary edit changed buffer") && ok;

    ok = focus_text_field(&imui, &recording, "start", cursor_start, sizeof(cursor_start), 4) &&
        ok;
    input = (SoundImuiInput){.key_home = true, .key_backspace = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "start",
        cursor_start,
        sizeof(cursor_start),
        input,
        &committed
    ) && expect(strcmp(cursor_start, "ab") == 0, "backspace at cursor 0 changed text") && ok;

    input = (SoundImuiInput){.key_delete = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "start",
        cursor_start,
        sizeof(cursor_start),
        input,
        &committed
    ) && expect(strcmp(cursor_start, "b") == 0, "delete at cursor 0 failed") && ok;

    ok = focus_text_field(&imui, &recording, "end", cursor_end, sizeof(cursor_end), 120) && ok;
    input = (SoundImuiInput){.key_end = true, .key_delete = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "end",
        cursor_end,
        sizeof(cursor_end),
        input,
        &committed
    ) && expect(strcmp(cursor_end, "ab") == 0, "delete at end changed text") && ok;

    input = (SoundImuiInput){.key_backspace = true};
    ok = run_text_frame(
        &imui,
        &recording,
        "end",
        cursor_end,
        sizeof(cursor_end),
        input,
        &committed
    ) && expect(strcmp(cursor_end, "a") == 0, "backspace at end failed") && ok;

    ok = focus_text_field(&imui, &recording, "limited", limited, sizeof(limited), 120) && ok;
    input = (SoundImuiInput){0};
    (void)snprintf(input.text, sizeof(input.text), "abcdefghi");
    ok = run_text_frame(
        &imui,
        &recording,
        "limited",
        limited,
        sizeof(limited),
        input,
        &committed
    ) && expect(strcmp(limited, "abcde") == 0, "text field exceeded buffer capacity") && ok;

    return ok;
}

static bool check_text_field_scaled_cursor(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    char buffer[16] = "abcd";
    bool committed = false;
    bool ok = true;

    sound_imui_set_text_scale(&imui, 2);

    SoundImuiInput input = {
        .mouse_x = 30,
        .mouse_y = 10,
        .mouse_left_down = true,
        .mouse_left_pressed = true,
    };
    begin_frame(&imui, &recording, &input);
    uint32_t id = sound_imui_id(&imui, "scaled");
    (void)sound_imui_text_field_rect(
        &imui,
        "scaled",
        buffer,
        sizeof(buffer),
        sound_imui_rect(0, 0, 140, 20)
    );
    sound_imui_end(&imui);

    const RecordingCommand *cursor = find_cursor_command(&recording);
    ok = expect(sound_imui_focus_id(&imui) == id, "scaled text field did not focus") && ok;
    ok = expect(cursor != NULL, "scaled text cursor did not draw") && ok;
    ok = expect(cursor && cursor->rect.x == 36, "scaled text cursor x was wrong") && ok;

    input = (SoundImuiInput){0};
    (void)snprintf(input.text, sizeof(input.text), "X");
    ok = run_text_frame(
        &imui,
        &recording,
        "scaled",
        buffer,
        sizeof(buffer),
        input,
        &committed
    ) && expect(strcmp(buffer, "abXcd") == 0, "scaled click chose the wrong byte") && ok;

    cursor = find_cursor_command(&recording);
    ok = expect(cursor && cursor->rect.x == 48, "scaled inserted cursor x was wrong") && ok;

    return ok;
}

static bool check_layout(void) {
    SoundImui imui = {0};
    RecordingDraw recording = {0};
    SoundImuiInput input = {0};
    bool ok = true;

    sound_imui_layout_begin(&imui, sound_imui_rect(10, 20, 100, 8), 8, 2);
    SoundImuiRect first = sound_imui_layout_next(&imui);
    SoundImuiRect second = sound_imui_layout_next(&imui);
    int weights[] = {1, 2, 1};
    sound_imui_layout_columns(&imui, 3, weights);
    SoundImuiRect left = sound_imui_layout_next(&imui);
    SoundImuiRect middle = sound_imui_layout_next(&imui);
    SoundImuiRect right = sound_imui_layout_next(&imui);

    ok = expect(first.x == 10 && first.y == 20 && first.width == 100 && first.height == 8,
        "first layout row is wrong") && ok;
    ok = expect(second.y == 30, "layout row did not advance by height plus spacing") && ok;
    ok = expect(left.x == 10 && left.y == 40 && left.width == 24,
        "left weighted column is wrong") && ok;
    ok = expect(middle.x == 36 && middle.width == 48,
        "middle weighted column is wrong") && ok;
    ok = expect(right.x == 86 && right.width == 24,
        "right weighted column is wrong") && ok;

    begin_frame(&imui, &recording, &input);
    sound_imui_layout_begin(&imui, sound_imui_rect(5, 6, 7, 8), 8, 2);
    (void)sound_imui_layout_next(&imui);
    sound_imui_end(&imui);

    begin_frame(&imui, &recording, &input);
    SoundImuiRect reset = sound_imui_layout_next(&imui);
    sound_imui_end(&imui);
    ok = expect(reset.x == 0 && reset.y == 0 && reset.width == 0 && reset.height == 0,
        "begin frame did not reset layout") && ok;

    return ok;
}

int main(void) {
    bool ok = check_button() &&
        check_text_scale() &&
        check_id_stack() &&
        check_tab_bar() &&
        check_list_row_id() &&
        check_scroll_list() &&
        check_text_field_editing() &&
        check_text_field_boundaries() &&
        check_text_field_scaled_cursor() &&
        check_layout();

    if (ok) {
        printf("imui tests passed\n");
    }

    return ok ? 0 : 1;
}
