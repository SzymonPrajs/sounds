//! Small typed immediate-mode core: frame state, ids, hit testing, list-row
//! focus, and text-field editing. It draws nothing and has no SDL dependency.

const std = @import("std");
const font = @import("font.zig");
const layout = @import("layout.zig");

pub const text_input_capacity = 128;
const id_stack_capacity = 16;
const clip_stack_capacity = 16;
const state_capacity = 128;
const fnv_offset_basis: u32 = 2166136261;
const fnv_prime: u32 = 16777619;

pub const Input = struct {
    mouse_x: i32 = 0,
    mouse_y: i32 = 0,
    wheel_y: i32 = 0,
    mouse_left_down: bool = false,
    mouse_left_pressed: bool = false,
    mouse_left_released: bool = false,
    key_left: bool = false,
    key_right: bool = false,
    key_home: bool = false,
    key_end: bool = false,
    key_backspace: bool = false,
    key_delete: bool = false,
    key_enter: bool = false,
    key_escape: bool = false,
    key_tab: bool = false,
    key_up: bool = false,
    key_down: bool = false,
    text: [text_input_capacity]u8 = [_]u8{0} ** text_input_capacity,
    text_len: usize = 0,

    pub fn beginFrame(self: *Input) void {
        const left_down = self.mouse_left_down;
        const x = self.mouse_x;
        const y = self.mouse_y;
        self.* = .{
            .mouse_x = x,
            .mouse_y = y,
            .mouse_left_down = left_down,
        };
    }

    pub fn appendText(self: *Input, bytes: []const u8) void {
        var offset: usize = 0;
        while (offset < bytes.len) {
            const step = font.utf8Step(bytes, offset);
            if (step == 0 or self.text_len + step > self.text.len) return;
            @memcpy(self.text[self.text_len .. self.text_len + step], bytes[offset .. offset + step]);
            self.text_len += step;
            offset += step;
        }
    }

    pub fn textSlice(self: *const Input) []const u8 {
        return self.text[0..self.text_len];
    }
};

pub const Widget = struct {
    visible: bool = false,
    hovered: bool = false,
    active: bool = false,
    fired: bool = false,
};

const StoredState = struct {
    id: u32 = 0,
    text_cursor: usize = 0,
    text_scroll: i32 = 0,
};

pub const TextBuffer = struct {
    storage: []u8,
    len: usize = 0,

    pub fn set(self: *TextBuffer, text: []const u8) void {
        self.len = @min(self.storage.len, text.len);
        if (self.len > 0) @memcpy(self.storage[0..self.len], text[0..self.len]);
    }

    pub fn clear(self: *TextBuffer) void {
        self.len = 0;
    }

    pub fn slice(self: *const TextBuffer) []const u8 {
        return self.storage[0..self.len];
    }

    fn deleteRange(self: *TextBuffer, first_arg: usize, last_arg: usize) void {
        const first = @min(first_arg, self.len);
        const last = @min(@max(last_arg, first), self.len);
        if (last <= first) return;
        std.mem.copyForwards(u8, self.storage[first .. self.len - (last - first)], self.storage[last..self.len]);
        self.len -= last - first;
    }

    fn insertBytes(self: *TextBuffer, cursor: *usize, bytes: []const u8) bool {
        if (bytes.len == 0 or self.len + bytes.len > self.storage.len) return false;
        const at = @min(cursor.*, self.len);
        std.mem.copyBackwards(u8, self.storage[at + bytes.len .. self.len + bytes.len], self.storage[at..self.len]);
        @memcpy(self.storage[at .. at + bytes.len], bytes);
        self.len += bytes.len;
        cursor.* = at + bytes.len;
        return true;
    }
};

pub const TextFieldResult = struct {
    committed: bool = false,
    focused: bool = false,
    cursor_x: i32 = 0,
    scroll_x: i32 = 0,
};

pub const Context = struct {
    input: ?*const Input = null,
    states: [state_capacity]StoredState = [_]StoredState{.{}} ** state_capacity,
    id_stack: [id_stack_capacity]u32 = [_]u32{0} ** id_stack_capacity,
    clip_stack: [clip_stack_capacity]layout.Rect = undefined,
    hover_id: u32 = 0,
    active_id: u32 = 0,
    focus_id: u32 = 0,
    text_scale: i32 = 1,
    id_depth: usize = 0,
    clip_depth: usize = 0,

    pub fn begin(self: *Context, input: ?*const Input) void {
        self.input = input;
        self.hover_id = 0;
        self.clip_depth = 0;
        if (input) |in| {
            if (in.key_escape) self.focus_id = 0;
        } else {
            self.active_id = 0;
        }
    }

    pub fn end(self: *Context) void {
        if (self.input) |input| {
            if (input.mouse_left_released) self.active_id = 0;
        }
        self.input = null;
        self.clip_depth = 0;
    }

    pub fn setTextScale(self: *Context, scale: i32) void {
        self.text_scale = std.math.clamp(scale, 1, 8);
    }

    pub fn id(self: *const Context, name: []const u8) u32 {
        return hashString(self.currentSeed(), name);
    }

    pub fn pushId(self: *Context, name: []const u8) void {
        if (self.id_depth >= self.id_stack.len) return;
        self.id_stack[self.id_depth] = self.id(name);
        self.id_depth += 1;
    }

    pub fn popId(self: *Context) void {
        if (self.id_depth > 0) self.id_depth -= 1;
    }

    pub fn pushClip(self: *Context, clip: layout.Rect) void {
        if (self.clip_depth >= self.clip_stack.len) return;
        self.clip_stack[self.clip_depth] = self.currentClip().intersection(clip);
        self.clip_depth += 1;
    }

    pub fn popClip(self: *Context) void {
        if (self.clip_depth > 0) self.clip_depth -= 1;
    }

    pub fn hitRect(self: *Context, name: []const u8, bounds: layout.Rect) Widget {
        return self.widgetUpdate(self.id(name), bounds);
    }

    pub fn focusTextField(self: *Context, name: []const u8, text: []const u8) void {
        const field_id = self.id(name);
        self.focus_id = field_id;
        if (self.stateForId(field_id)) |state| {
            state.text_cursor = text.len;
            state.text_scroll = 0;
        }
    }

    pub fn textField(
        self: *Context,
        name: []const u8,
        buffer: *TextBuffer,
        bounds: layout.Rect,
    ) TextFieldResult {
        const field_id = self.id(name);
        var widget = self.widgetUpdate(field_id, bounds);
        const state = self.stateForId(field_id) orelse return .{};

        if (widget.fired or (widget.hovered and self.input != null and self.input.?.mouse_left_pressed)) {
            self.focus_id = field_id;
            state.text_cursor = cursorFromX(buffer.slice(), state.text_scroll, bounds, self.input.?.mouse_x, self.text_scale);
            widget.active = true;
        }

        if (state.text_cursor > buffer.len) state.text_cursor = buffer.len;

        var committed = false;
        if (self.focus_id == field_id) {
            if (self.input) |input| {
                editText(buffer, state, input);
                if (input.key_enter) committed = true;
            }
            scrollCursorIntoView(buffer.slice(), state, bounds, self.text_scale);
        }

        return .{
            .committed = committed,
            .focused = self.focus_id == field_id,
            .cursor_x = bounds.x + padding(self.text_scale) + cursorPrefixWidth(buffer.slice(), state.text_cursor, self.text_scale) - state.text_scroll,
            .scroll_x = state.text_scroll,
        };
    }

    pub fn currentClip(self: *const Context) layout.Rect {
        if (self.clip_depth == 0) {
            return layout.rect(-0x10000000, -0x10000000, 0x20000000, 0x20000000);
        }
        return self.clip_stack[self.clip_depth - 1];
    }

    pub fn hasTextFocus(self: *const Context) bool {
        return self.focus_id != 0;
    }

    pub fn clearFocus(self: *Context) void {
        self.focus_id = 0;
    }

    fn currentSeed(self: *const Context) u32 {
        if (self.id_depth == 0) return fnv_offset_basis;
        return self.id_stack[self.id_depth - 1];
    }

    fn stateForId(self: *Context, state_id: u32) ?*StoredState {
        const start: usize = state_id % state_capacity;
        for (0..state_capacity) |offset| {
            const index = (start + offset) % state_capacity;
            if (self.states[index].id == state_id) return &self.states[index];
            if (self.states[index].id == 0) {
                self.states[index].id = state_id;
                return &self.states[index];
            }
        }
        return null;
    }

    fn widgetUpdate(self: *Context, widget_id: u32, bounds: layout.Rect) Widget {
        var widget = Widget{
            .visible = bounds.intersects(self.currentClip()),
            .active = self.active_id == widget_id,
        };

        const input = self.input orelse {
            widget.active = false;
            return widget;
        };

        const captures_mouse = self.active_id == 0 or self.active_id == widget_id;
        if (captures_mouse and
            bounds.contains(input.mouse_x, input.mouse_y) and
            self.currentClip().contains(input.mouse_x, input.mouse_y) and
            (self.hover_id == 0 or self.hover_id == widget_id))
        {
            self.hover_id = widget_id;
            widget.hovered = true;
        }

        if (widget.hovered and input.mouse_left_pressed and self.active_id == 0) {
            self.active_id = widget_id;
            widget.active = true;
        }

        if (self.active_id == widget_id) widget.active = true;
        if (widget.active and widget.hovered and input.mouse_left_released) widget.fired = true;
        return widget;
    }
};

pub fn padding(scale: i32) i32 {
    return 6 * @max(1, scale);
}

fn hashString(seed: u32, text: []const u8) u32 {
    var hash = seed;
    for (text) |byte| hash = hashByte(hash, byte);
    hash = hashByte(hash, 0);
    return if (hash == 0) 1 else hash;
}

fn hashByte(hash: u32, value: u8) u32 {
    return (hash ^ value) *% fnv_prime;
}

fn utf8Next(text: []const u8, cursor: usize) usize {
    return @min(text.len, cursor + font.utf8Step(text, cursor));
}

fn utf8Previous(text: []const u8, cursor: usize) usize {
    if (cursor == 0) return 0;
    var previous = cursor - 1;
    while (previous > 0 and (text[previous] & 0xC0) == 0x80) previous -= 1;
    return previous;
}

fn cursorFromX(text: []const u8, scroll: i32, bounds: layout.Rect, mouse_x: i32, scale: i32) usize {
    const relative_x = mouse_x - bounds.x - padding(scale) + scroll;
    if (relative_x <= 0) return 0;

    var cursor: usize = 0;
    while (cursor < text.len) {
        const next = utf8Next(text, cursor);
        const current_width = cursorPrefixWidth(text, cursor, scale);
        const next_width = cursorPrefixWidth(text, next, scale);
        const midpoint = current_width + @divTrunc(next_width - current_width, 2);
        if (relative_x < midpoint) return cursor;
        cursor = next;
    }
    return text.len;
}

fn editText(buffer: *TextBuffer, state: *StoredState, input: *const Input) void {
    if (input.key_home) state.text_cursor = 0;
    if (input.key_end) state.text_cursor = buffer.len;
    if (input.key_left) state.text_cursor = utf8Previous(buffer.slice(), state.text_cursor);
    if (input.key_right) state.text_cursor = utf8Next(buffer.slice(), state.text_cursor);

    if (input.key_backspace and state.text_cursor > 0) {
        const previous = utf8Previous(buffer.slice(), state.text_cursor);
        buffer.deleteRange(previous, state.text_cursor);
        state.text_cursor = previous;
    }

    if (input.key_delete and state.text_cursor < buffer.len) {
        buffer.deleteRange(state.text_cursor, utf8Next(buffer.slice(), state.text_cursor));
    }

    var offset: usize = 0;
    const text = input.textSlice();
    while (offset < text.len) {
        const step = font.utf8Step(text, offset);
        if (step == 0) break;
        if (!buffer.insertBytes(&state.text_cursor, text[offset .. offset + step])) break;
        offset += step;
    }
}

fn scrollCursorIntoView(text: []const u8, state: *StoredState, bounds: layout.Rect, scale: i32) void {
    const pad = padding(scale);
    const view_width = @max(1, bounds.width - pad * 2);
    const cursor_x = cursorPrefixWidth(text, state.text_cursor, scale);

    if (cursor_x - state.text_scroll > view_width) {
        state.text_scroll = cursor_x - view_width;
    }
    if (cursor_x < state.text_scroll) state.text_scroll = cursor_x;
    if (state.text_scroll < 0) state.text_scroll = 0;
}

fn cursorPrefixWidth(text: []const u8, bytes: usize, scale: i32) i32 {
    const count: i32 = @intCast(font.codepointCount(text[0..@min(bytes, text.len)]));
    return count * font.glyph_advance * @max(1, scale);
}

test "button hit state captures and fires on release inside" {
    var ctx = Context{};
    const bounds = layout.rect(10, 10, 80, 20);
    var input = Input{ .mouse_x = 20, .mouse_y = 15 };

    ctx.begin(&input);
    const id = ctx.id("play");
    try std.testing.expect(!ctx.hitRect("play", bounds).fired);
    ctx.end();
    try std.testing.expectEqual(id, ctx.hover_id);

    input = .{ .mouse_x = 20, .mouse_y = 15, .mouse_left_down = true, .mouse_left_pressed = true };
    ctx.begin(&input);
    _ = ctx.hitRect("play", bounds);
    ctx.end();
    try std.testing.expectEqual(id, ctx.active_id);

    input = .{ .mouse_x = 20, .mouse_y = 15, .mouse_left_released = true };
    ctx.begin(&input);
    try std.testing.expect(ctx.hitRect("play", bounds).fired);
    ctx.end();
    try std.testing.expectEqual(@as(u32, 0), ctx.active_id);
}

test "id stack distinguishes matching labels" {
    var ctx = Context{};
    const input = Input{ .mouse_x = 10, .mouse_y = 10, .mouse_left_down = true, .mouse_left_pressed = true };
    ctx.begin(&input);
    ctx.pushId("left");
    const left = ctx.id("same");
    _ = ctx.hitRect("same", layout.rect(0, 0, 50, 20));
    ctx.popId();
    ctx.pushId("right");
    const right = ctx.id("same");
    _ = ctx.hitRect("same", layout.rect(60, 0, 50, 20));
    ctx.popId();
    ctx.end();

    try std.testing.expect(left != right);
    try std.testing.expectEqual(left, ctx.active_id);
}

test "text field edits UTF-8, movement, deletion, commit, and focus drop" {
    var ctx = Context{};
    var storage: [16]u8 = undefined;
    var buffer = TextBuffer{ .storage = &storage };
    const bounds = layout.rect(0, 0, 140, 20);

    var input = Input{ .mouse_x = 80, .mouse_y = 10, .mouse_left_down = true, .mouse_left_pressed = true };
    ctx.begin(&input);
    const id = ctx.id("edit");
    _ = ctx.textField("edit", &buffer, bounds);
    ctx.end();
    try std.testing.expectEqual(id, ctx.focus_id);

    input = .{};
    input.appendText("ab");
    ctx.begin(&input);
    _ = ctx.textField("edit", &buffer, bounds);
    ctx.end();
    try std.testing.expectEqualStrings("ab", buffer.slice());

    input = .{ .key_home = true };
    input.appendText("é");
    ctx.begin(&input);
    _ = ctx.textField("edit", &buffer, bounds);
    ctx.end();
    try std.testing.expectEqualStrings("éab", buffer.slice());

    input = .{ .key_right = true };
    input.appendText("X");
    ctx.begin(&input);
    _ = ctx.textField("edit", &buffer, bounds);
    ctx.end();
    try std.testing.expectEqualStrings("éaXb", buffer.slice());

    input = .{ .key_end = true };
    input.appendText("Z");
    ctx.begin(&input);
    _ = ctx.textField("edit", &buffer, bounds);
    ctx.end();
    try std.testing.expectEqualStrings("éaXbZ", buffer.slice());

    input = .{ .key_left = true, .key_backspace = true };
    ctx.begin(&input);
    _ = ctx.textField("edit", &buffer, bounds);
    ctx.end();
    try std.testing.expectEqualStrings("éaXZ", buffer.slice());

    input = .{ .key_delete = true };
    ctx.begin(&input);
    _ = ctx.textField("edit", &buffer, bounds);
    ctx.end();
    try std.testing.expectEqualStrings("éaX", buffer.slice());

    input = .{ .key_enter = true };
    ctx.begin(&input);
    try std.testing.expect(ctx.textField("edit", &buffer, bounds).committed);
    ctx.end();

    input = .{ .key_escape = true };
    ctx.begin(&input);
    _ = ctx.textField("edit", &buffer, bounds);
    ctx.end();
    try std.testing.expectEqual(@as(u32, 0), ctx.focus_id);
}

test "scaled text field click maps to the same byte cursor as the C intent" {
    var ctx = Context{};
    ctx.setTextScale(2);
    var storage: [16]u8 = undefined;
    var buffer = TextBuffer{ .storage = &storage };
    buffer.set("abcd");

    var input = Input{ .mouse_x = 30, .mouse_y = 10, .mouse_left_down = true, .mouse_left_pressed = true };
    ctx.begin(&input);
    const result = ctx.textField("scaled", &buffer, layout.rect(0, 0, 140, 20));
    ctx.end();
    try std.testing.expectEqual(@as(i32, 36), result.cursor_x);

    input = .{};
    input.appendText("X");
    ctx.begin(&input);
    const inserted = ctx.textField("scaled", &buffer, layout.rect(0, 0, 140, 20));
    ctx.end();
    try std.testing.expectEqualStrings("abXcd", buffer.slice());
    try std.testing.expectEqual(@as(i32, 48), inserted.cursor_x);
}
