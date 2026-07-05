//! Typed immediate-mode widgets drawn into the software buffer.

const std = @import("std");
const core = @import("core.zig");
const draw = @import("draw.zig");
const font = @import("font.zig");
const layout = @import("layout.zig");

pub fn button(
    ctx: *core.Context,
    buffer: *draw.PixelBuffer,
    name: []const u8,
    label: []const u8,
    bounds: layout.Rect,
) bool {
    const widget = ctx.hitRect(name, bounds);
    if (widget.visible) {
        buffer.fillRect(bounds, buttonColor(widget));
        buffer.outline(bounds, 1, draw.palette.border);
        buffer.drawTextCentered(label, bounds, ctx.text_scale, draw.palette.text);
    }
    return widget.fired;
}

pub fn listRow(
    ctx: *core.Context,
    buffer: *draw.PixelBuffer,
    name: []const u8,
    label: []const u8,
    selected: bool,
    bounds: layout.Rect,
) bool {
    const widget = ctx.hitRect(name, bounds);
    if (widget.visible) {
        const fill = if (selected or widget.hovered)
            draw.palette.selected
        else
            draw.palette.background;
        buffer.fillRect(bounds, fill);
        const y = bounds.y + @divTrunc(bounds.height - font.textHeight(ctx.text_scale), 2);
        buffer.drawText(label, bounds.x + core.padding(ctx.text_scale), y, ctx.text_scale, if (selected) draw.palette.text else draw.palette.text_dim);
    }
    return widget.fired;
}

pub fn textField(
    ctx: *core.Context,
    buffer: *draw.PixelBuffer,
    name: []const u8,
    text: *core.TextBuffer,
    bounds: layout.Rect,
) core.TextFieldResult {
    const result = ctx.textField(name, text, bounds);
    buffer.fillRect(bounds, if (result.focused) draw.palette.panel_alt else draw.palette.panel);
    buffer.outline(bounds, 1, if (result.focused) draw.palette.accent else draw.palette.border);

    const text_y = bounds.y + @divTrunc(bounds.height - font.textHeight(ctx.text_scale), 2);
    buffer.drawTextClipped(
        text.slice(),
        bounds.x + core.padding(ctx.text_scale) - result.scroll_x,
        text_y,
        ctx.text_scale,
        draw.palette.text,
        bounds.inset(2 * ctx.text_scale),
    );
    if (result.focused) {
        buffer.fillRect(
            layout.rect(result.cursor_x, bounds.y + 4 * ctx.text_scale, @max(1, ctx.text_scale), bounds.height - 8 * ctx.text_scale),
            draw.palette.marker,
        );
    }
    return result;
}

pub fn rowRect(left: i32, top: i32, width: i32, scale: i32, line_height: i32) layout.Rect {
    return layout.rect(
        left - 3 * scale,
        top - 2 * scale,
        width + 6 * scale,
        line_height,
    );
}

pub fn controlHeight(scale: i32) i32 {
    return (font.glyph_height + 6) * scale;
}

pub fn controlWidth(label: []const u8, scale: i32) i32 {
    return font.textWidth(label, scale) + 12 * scale;
}

pub fn wideControlWidth(label: []const u8, scale: i32) i32 {
    return font.textWidth(label, scale) + 14 * scale;
}

fn buttonColor(widget: core.Widget) draw.Color {
    if (widget.active) return draw.palette.active;
    if (widget.hovered) return draw.palette.hover;
    return draw.palette.panel;
}

test "control sizing is stable for fixed labels" {
    try std.testing.expectEqual(@as(i32, 13), controlHeight(1));
    try std.testing.expect(controlWidth("REC", 1) < wideControlWidth("REC", 1));
    try std.testing.expect(rowRect(10, 20, 100, 2, 30).width == 112);
}
