//! Pure UI geometry, workspace metadata, key hints, and small navigation
//! helpers. This module has no SDL dependency.

const std = @import("std");

pub const Rect = struct {
    x: i32,
    y: i32,
    width: i32,
    height: i32,

    pub fn right(self: Rect) i32 {
        return self.x + self.width;
    }

    pub fn bottom(self: Rect) i32 {
        return self.y + self.height;
    }

    pub fn empty(self: Rect) bool {
        return self.width <= 0 or self.height <= 0;
    }

    pub fn contains(self: Rect, x: i32, y: i32) bool {
        return !self.empty() and
            x >= self.x and
            y >= self.y and
            x < self.right() and
            y < self.bottom();
    }

    pub fn intersects(self: Rect, other: Rect) bool {
        return !self.empty() and !other.empty() and
            self.x < other.right() and
            self.right() > other.x and
            self.y < other.bottom() and
            self.bottom() > other.y;
    }

    pub fn intersection(self: Rect, other: Rect) Rect {
        const left = @max(self.x, other.x);
        const top = @max(self.y, other.y);
        const right_edge = @min(self.right(), other.right());
        const bottom_edge = @min(self.bottom(), other.bottom());

        if (right_edge <= left or bottom_edge <= top) {
            return .{ .x = left, .y = top, .width = 0, .height = 0 };
        }

        return .{
            .x = left,
            .y = top,
            .width = right_edge - left,
            .height = bottom_edge - top,
        };
    }

    pub fn inset(self: Rect, amount: i32) Rect {
        return .{
            .x = self.x + amount,
            .y = self.y + amount,
            .width = @max(0, self.width - amount * 2),
            .height = @max(0, self.height - amount * 2),
        };
    }
};

pub fn rect(x: i32, y: i32, width: i32, height: i32) Rect {
    return .{ .x = x, .y = y, .width = width, .height = height };
}

pub const Workspace = enum {
    live,
    recordings,
    trim,
    spectrum,
    band_lab,

    pub fn label(self: Workspace) []const u8 {
        return switch (self) {
            .live => "LIVE",
            .recordings => "RECS",
            .trim => "TRIM",
            .spectrum => "SPECTRUM",
            .band_lab => "BAND LAB",
        };
    }

    pub fn shortcut(self: Workspace) u8 {
        return switch (self) {
            .live => 'L',
            .recordings => 'V',
            .trim => 'T',
            .spectrum => 'O',
            .band_lab => 'B',
        };
    }

    pub fn index(self: Workspace) usize {
        return @intFromEnum(self);
    }
};

pub const workspace_order = [_]Workspace{
    .live,
    .recordings,
    .trim,
    .spectrum,
    .band_lab,
};

pub const Metrics = struct {
    scale: i32,
    gap: i32,
    pad: i32,
    banner_height: i32,
    waveform_height: i32,
    hint_height: i32,
    axis_width: i32,
    separator: i32,

    pub fn forSize(width: i32, height: i32) Metrics {
        const short_edge = @min(width, height);
        const scale: i32 = if (short_edge >= 900 and width >= 1200) 2 else 1;
        return .{
            .scale = scale,
            .gap = 8 * scale,
            .pad = 12 * scale,
            .banner_height = 34 * scale,
            .waveform_height = 58 * scale,
            .hint_height = 22 * scale,
            .axis_width = 54 * scale,
            .separator = @max(1, 2 * scale),
        };
    }
};

pub const FrameLayout = struct {
    bounds: Rect,
    banner: Rect,
    waveform: Rect,
    main: Rect,
    axis: Rect,
    plot: Rect,
    hints: Rect,
};

pub fn frame(width: i32, height: i32) FrameLayout {
    const safe_width = @max(0, width);
    const safe_height = @max(0, height);
    const metrics = Metrics.forSize(safe_width, safe_height);
    const banner_height = @min(metrics.banner_height, safe_height);
    const waveform_top = banner_height;
    const waveform_height = @min(metrics.waveform_height, @max(0, safe_height - waveform_top));
    const hints_height = @min(metrics.hint_height, @max(0, safe_height - waveform_top - waveform_height));
    const main_top = waveform_top + waveform_height + metrics.separator;
    const main_bottom = @max(main_top, safe_height - hints_height);
    const main_height = @max(0, main_bottom - main_top);
    const axis_width = @min(metrics.axis_width, safe_width);

    return .{
        .bounds = rect(0, 0, safe_width, safe_height),
        .banner = rect(0, 0, safe_width, banner_height),
        .waveform = rect(0, waveform_top, safe_width, waveform_height),
        .main = rect(0, main_top, safe_width, main_height),
        .axis = rect(0, main_top, axis_width, main_height),
        .plot = rect(axis_width, main_top, @max(0, safe_width - axis_width), main_height),
        .hints = rect(0, safe_height - hints_height, safe_width, hints_height),
    };
}

pub fn centeredPanel(width: i32, height: i32, wanted_width: i32, wanted_height: i32, margin: i32) Rect {
    const panel_width = @max(0, @min(wanted_width, width - margin * 2));
    const panel_height = @max(0, @min(wanted_height, height - margin * 2));
    return rect(
        @divTrunc(width - panel_width, 2),
        @divTrunc(height - panel_height, 2),
        panel_width,
        panel_height,
    );
}

pub const FrequencyTick = struct {
    hz: f64,
    label: []const u8,
    gridline: bool,
};

pub const frequency_ticks = [_]FrequencyTick{
    .{ .hz = 24_000.0, .label = "24K", .gridline = false },
    .{ .hz = 20_000.0, .label = "20K", .gridline = false },
    .{ .hz = 10_000.0, .label = "10K", .gridline = true },
    .{ .hz = 5_000.0, .label = "5K", .gridline = false },
    .{ .hz = 2_400.0, .label = "2.4K", .gridline = false },
    .{ .hz = 2_000.0, .label = "2K", .gridline = false },
    .{ .hz = 1_000.0, .label = "1K", .gridline = true },
    .{ .hz = 500.0, .label = "500", .gridline = false },
    .{ .hz = 200.0, .label = "200", .gridline = false },
    .{ .hz = 120.0, .label = "120", .gridline = false },
    .{ .hz = 100.0, .label = "100", .gridline = true },
    .{ .hz = 50.0, .label = "50", .gridline = false },
    .{ .hz = 20.0, .label = "20", .gridline = false },
    .{ .hz = 10.0, .label = "10", .gridline = true },
};

pub fn rowForFrequency(min_hz: f64, max_hz: f64, hz: f64, height: i32) ?i32 {
    if (height <= 0 or min_hz <= 0.0 or max_hz <= min_hz or hz <= 0.0) return null;
    if (hz < min_hz or hz > max_hz) return null;

    const log_min = @log2(min_hz);
    const log_max = @log2(max_hz);
    const unit = (log_max - @log2(hz)) / (log_max - log_min);
    const row_float = @round(unit * @as(f64, @floatFromInt(height)) - 0.5);
    const row_value: i32 = @intFromFloat(row_float);
    return std.math.clamp(row_value, 0, height - 1);
}

pub fn frequencyForRow(min_hz: f64, max_hz: f64, row: i32, height: i32) f64 {
    if (height <= 0 or min_hz <= 0.0 or max_hz <= min_hz) return 0.0;

    const clamped_row = std.math.clamp(row, 0, height - 1);
    const unit = (@as(f64, @floatFromInt(clamped_row)) + 0.5) /
        @as(f64, @floatFromInt(height));
    const log_min = @log2(min_hz);
    const log_max = @log2(max_hz);
    return std.math.pow(f64, 2.0, log_max + (log_min - log_max) * unit);
}

pub fn sampleMarkerX(area: Rect, sample: u64, sample_count: u64) i32 {
    if (area.width <= 1 or sample_count == 0) return area.x;
    const clamped = @min(sample, sample_count);
    const unit = @as(f64, @floatFromInt(clamped)) / @as(f64, @floatFromInt(sample_count));
    return area.x + @as(i32, @intFromFloat(@round(unit * @as(f64, @floatFromInt(area.width - 1)))));
}

pub const ListCursor = struct {
    index: usize = 0,

    pub fn clamp(self: *ListCursor, count: usize) void {
        if (count == 0) {
            self.index = 0;
        } else if (self.index >= count) {
            self.index = count - 1;
        }
    }

    pub fn move(self: *ListCursor, delta: isize, count: usize) void {
        if (count == 0) {
            self.index = 0;
            return;
        }

        const current: isize = @intCast(self.index);
        const last: isize = @intCast(count - 1);
        const next = std.math.clamp(current + delta, 0, last);
        self.index = @intCast(next);
    }
};

pub const HintContext = struct {
    workspace: Workspace,
    menu_open: bool = false,
    text_editing: bool = false,
};

pub fn keyHints(context: HintContext) []const u8 {
    if (context.text_editing) return "ENTER apply   ESC cancel   ARROWS move cursor";
    if (context.menu_open) return "M/ESC close   ARROWS move   ENTER set   TAB switch";

    return switch (context.workspace) {
        .live => "M menu   TAB workspace   R rec   SPACE play   1-8 mode",
        .recordings => "UP/DOWN choose   ENTER load   N rename   D delete   M menu",
        .trim => ",/. handle   ARROWS move   / apply   BACKSPACE clear",
        .spectrum => "O spectrum   SPACE play   M menu",
        .band_lab => "A audition   F method   H edge   [ ] - = adjust",
    };
}

pub fn formatDuration(seconds: f64, buffer: []u8) []const u8 {
    const total: u64 = if (seconds > 0.0) @intFromFloat(@floor(seconds)) else 0;
    const minutes = total / 60;
    const remaining = total % 60;

    if (minutes >= 100) {
        const hours = minutes / 60;
        const mins = minutes % 60;
        return std.fmt.bufPrint(buffer, "{d}:{d:0>2}:{d:0>2}", .{ hours, mins, remaining }) catch "";
    }

    return std.fmt.bufPrint(buffer, "{d:0>2}:{d:0>2}", .{ minutes, remaining }) catch "";
}

test "frame layout preserves banner waveform plot and hints" {
    const f = frame(1280, 800);
    try std.testing.expectEqual(@as(i32, 0), f.banner.y);
    try std.testing.expect(f.banner.height > 0);
    try std.testing.expectEqual(f.banner.bottom(), f.waveform.y);
    try std.testing.expect(f.plot.x > f.axis.x);
    try std.testing.expect(f.plot.width > 0);
    try std.testing.expect(f.hints.y >= f.main.bottom());
}

test "log frequency rows include the display endpoints" {
    const height = 512;
    const top = rowForFrequency(10.0, 24_000.0, 24_000.0, height) orelse -1;
    const bottom = rowForFrequency(10.0, 24_000.0, 10.0, height) orelse -1;

    try std.testing.expect(top <= 1);
    try std.testing.expect(bottom >= height - 2);
    try std.testing.expect(rowForFrequency(10.0, 24_000.0, 1_000.0, height) != null);
    try std.testing.expectApproxEqRel(1_000.0, frequencyForRow(10.0, 24_000.0, rowForFrequency(10.0, 24_000.0, 1_000.0, height).?, height), 0.02);
    try std.testing.expectEqual(@as(f64, 24_000.0), frequency_ticks[0].hz);
    try std.testing.expectEqualStrings("10", frequency_ticks[frequency_ticks.len - 1].label);
}

test "list cursor clamps keyboard movement" {
    var cursor = ListCursor{ .index = 2 };
    cursor.move(-10, 5);
    try std.testing.expectEqual(@as(usize, 0), cursor.index);
    cursor.move(10, 5);
    try std.testing.expectEqual(@as(usize, 4), cursor.index);
    cursor.clamp(2);
    try std.testing.expectEqual(@as(usize, 1), cursor.index);
    cursor.move(1, 0);
    try std.testing.expectEqual(@as(usize, 0), cursor.index);
}

test "duration formatting uses compact mm:ss until long recordings" {
    var buffer: [32]u8 = undefined;
    try std.testing.expectEqualStrings("00:00", formatDuration(0.1, &buffer));
    try std.testing.expectEqualStrings("01:05", formatDuration(65.9, &buffer));
    try std.testing.expectEqualStrings("1:40:05", formatDuration(6005.0, &buffer));
}
