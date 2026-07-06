//! Software drawing primitives over a 32-bit ARGB pixel buffer.

const std = @import("std");
const font = @import("font.zig");
const layout = @import("layout.zig");
const colormap = @import("../support/colormap.zig");

pub const Color = u32;

pub const Palette = struct {
    background: Color = rgb(5, 7, 12),
    panel: Color = rgb(10, 16, 24),
    panel_alt: Color = rgb(13, 20, 30),
    banner: Color = rgb(11, 15, 22),
    separator: Color = rgb(16, 20, 28),
    line: Color = rgb(40, 51, 70),
    grid: Color = rgb(62, 74, 102),
    hover: Color = rgb(23, 36, 58),
    active: Color = rgb(35, 55, 82),
    selected: Color = rgb(23, 36, 58),
    border: Color = rgb(95, 115, 148),
    text: Color = rgb(215, 232, 255),
    text_dim: Color = rgb(143, 163, 191),
    accent: Color = rgb(151, 230, 176),
    record: Color = rgb(255, 143, 112),
    playhead: Color = rgb(255, 242, 168),
    waveform: Color = rgb(149, 221, 255),
    marker: Color = rgb(244, 248, 255),
    marker_dim: Color = rgb(132, 151, 178),
};

pub const palette = Palette{};
pub const floor_db: f32 = -95.0;
pub const ceiling_db: f32 = -15.0;

pub fn rgb(red: u8, green: u8, blue: u8) Color {
    return 0xFF000000 |
        (@as(u32, red) << 16) |
        (@as(u32, green) << 8) |
        @as(u32, blue);
}

pub fn packColor(color: colormap.Color) Color {
    return rgb(unitByte(color.red), unitByte(color.green), unitByte(color.blue));
}

/// dB -> color goes through comptime lookup tables: the spectrogram fills
/// millions of pixels per frame, and interpolating colormap stops per pixel
/// is what made the live view crawl. 256 levels is finer than the 8-bit
/// display resolution of the ramps.
const color_lut_size = 256;
const db_index_scale: f32 = @as(f32, color_lut_size - 1) / (ceiling_db - floor_db);

const color_luts: [colormap.count][color_lut_size]Color = blk: {
    @setEvalBranchQuota(200_000);
    var tables: [colormap.count][color_lut_size]Color = undefined;
    for (colormap.order, 0..) |map, m| {
        for (0..color_lut_size) |i| {
            const unit = @as(f32, @floatFromInt(i)) / @as(f32, color_lut_size - 1);
            tables[m][i] = packColor(map.sample(unit));
        }
    }
    break :blk tables;
};

/// Same tables pre-blended with the gridline tint, so gridline rows cost a
/// table swap instead of a per-pixel blend.
const gridline_luts: [colormap.count][color_lut_size]Color = blk: {
    @setEvalBranchQuota(200_000);
    var tables: [colormap.count][color_lut_size]Color = undefined;
    for (0..colormap.count) |m| {
        for (0..color_lut_size) |i| {
            tables[m][i] = blend(color_luts[m][i], (Palette{}).grid, 0.25);
        }
    }
    break :blk tables;
};

pub fn colormapLut(map: colormap.Colormap) *const [color_lut_size]Color {
    return &color_luts[map.index()];
}

pub fn gridlineLut(map: colormap.Colormap) *const [color_lut_size]Color {
    return &gridline_luts[map.index()];
}

pub inline fn dbIndex(db: f32) usize {
    const scaled = (db - floor_db) * db_index_scale;
    const clamped = std.math.clamp(scaled, 0.0, @as(f32, color_lut_size - 1));
    return @intFromFloat(clamped);
}

pub fn colorForDb(map: colormap.Colormap, db: f32) Color {
    return color_luts[map.index()][dbIndex(db)];
}

pub fn blend(base: Color, over: Color, amount_arg: f32) Color {
    const amount = std.math.clamp(amount_arg, 0.0, 1.0);
    const keep = 1.0 - amount;
    const red = @as(f32, @floatFromInt((base >> 16) & 0xFF)) * keep +
        @as(f32, @floatFromInt((over >> 16) & 0xFF)) * amount;
    const green = @as(f32, @floatFromInt((base >> 8) & 0xFF)) * keep +
        @as(f32, @floatFromInt((over >> 8) & 0xFF)) * amount;
    const blue = @as(f32, @floatFromInt(base & 0xFF)) * keep +
        @as(f32, @floatFromInt(over & 0xFF)) * amount;
    return rgb(
        @intFromFloat(@round(red)),
        @intFromFloat(@round(green)),
        @intFromFloat(@round(blue)),
    );
}

pub const PixelBuffer = struct {
    pixels: []Color,
    width: i32,
    height: i32,

    pub fn row(self: *PixelBuffer, y: i32) []Color {
        const start: usize = @intCast(y * self.width);
        return self.pixels[start .. start + @as(usize, @intCast(self.width))];
    }

    pub fn fill(self: *PixelBuffer, color: Color) void {
        fillSpan(self.pixels, color);
    }

    pub fn fillRect(self: *PixelBuffer, rect_arg: layout.Rect, color: Color) void {
        const clipped = self.clipRect(rect_arg);
        if (clipped.empty()) return;

        var y = clipped.y;
        while (y < clipped.bottom()) : (y += 1) {
            fillSpan(self.row(y)[@intCast(clipped.x)..@intCast(clipped.right())], color);
        }
    }

    pub fn blendRect(self: *PixelBuffer, rect_arg: layout.Rect, color: Color, amount: f32) void {
        const clipped = self.clipRect(rect_arg);
        if (clipped.empty()) return;

        var y = clipped.y;
        while (y < clipped.bottom()) : (y += 1) {
            const span = self.row(y)[@intCast(clipped.x)..@intCast(clipped.right())];
            blendSpan(span, color, amount);
        }
    }

    pub fn dim(self: *PixelBuffer) void {
        blendSpan(self.pixels, palette.background, 0.68);
    }

    pub fn outline(self: *PixelBuffer, bounds: layout.Rect, thickness_arg: i32, color: Color) void {
        const thickness = @max(1, thickness_arg);
        self.fillRect(layout.rect(bounds.x, bounds.y, bounds.width, thickness), color);
        self.fillRect(layout.rect(bounds.x, bounds.bottom() - thickness, bounds.width, thickness), color);
        self.fillRect(layout.rect(bounds.x, bounds.y, thickness, bounds.height), color);
        self.fillRect(layout.rect(bounds.right() - thickness, bounds.y, thickness, bounds.height), color);
    }

    pub fn drawText(self: *PixelBuffer, text: []const u8, x_arg: i32, y: i32, scale_arg: i32, color: Color) void {
        const scale = @max(1, scale_arg);
        var x = x_arg;
        var offset: usize = 0;
        while (offset < text.len) {
            const step = font.utf8Step(text, offset);
            if (step == 0) break;
            const character = if (step == 1) text[offset] else '?';
            self.drawGlyph(font.glyph(character).*, x, y, scale, color);
            x += font.glyph_advance * scale;
            offset += step;
        }
    }

    pub fn drawTextCentered(self: *PixelBuffer, text: []const u8, bounds: layout.Rect, scale: i32, color: Color) void {
        const text_width = font.textWidth(text, scale);
        const text_height = font.textHeight(scale);
        const x = bounds.x + @divTrunc(bounds.width - text_width, 2);
        const y = bounds.y + @divTrunc(bounds.height - text_height, 2);
        self.drawText(text, @max(bounds.x + scale, x), @max(bounds.y + scale, y), scale, color);
    }

    pub fn drawTextClipped(self: *PixelBuffer, text: []const u8, x_arg: i32, y: i32, scale_arg: i32, color: Color, clip_arg: layout.Rect) void {
        const scale = @max(1, scale_arg);
        const clip = self.clipRect(clip_arg);
        if (clip.empty()) return;

        var x = x_arg;
        var offset: usize = 0;
        while (offset < text.len) {
            const step = font.utf8Step(text, offset);
            if (step == 0) break;
            const character = if (step == 1) text[offset] else '?';
            const glyph_bounds = layout.rect(x, y, font.glyph_width * scale, font.glyph_height * scale);
            if (glyph_bounds.right() >= clip.x and glyph_bounds.x < clip.right()) {
                self.drawGlyphClipped(font.glyph(character).*, x, y, scale, color, clip);
            }
            x += font.glyph_advance * scale;
            offset += step;
        }
    }

    pub fn drawWaveform(self: *PixelBuffer, samples: []const f32, bounds: layout.Rect, color: Color) void {
        const clipped = self.clipRect(bounds);
        if (clipped.empty()) return;

        self.fillRect(layout.rect(clipped.x, clipped.y + @divTrunc(clipped.height, 2), clipped.width, 1), palette.line);
        if (samples.len == 0) return;

        const peak = absolutePeak(samples);
        const gain: f32 = if (peak > 1.0e-7) @min(0.90 / peak, 32.0) else 1.0;

        var x: i32 = 0;
        while (x < clipped.width) : (x += 1) {
            const begin = @as(usize, @intCast(x)) * samples.len / @as(usize, @intCast(clipped.width));
            var end = @as(usize, @intCast(x + 1)) * samples.len / @as(usize, @intCast(clipped.width));
            if (end <= begin) end = begin + 1;
            end = @min(end, samples.len);

            const range = sampleRange(samples[begin..end]);

            const y_high = waveformY(range.high, gain, clipped.height);
            const y_low = waveformY(range.low, gain, clipped.height);
            self.fillRect(layout.rect(clipped.x + x, clipped.y + y_high, 1, y_low - y_high + 1), color);
        }
    }

    pub fn drawSpectrogram(
        self: *PixelBuffer,
        columns: []const f32,
        column_count: usize,
        row_count: usize,
        bounds: layout.Rect,
        map: colormap.Colormap,
        min_hz: f64,
        max_hz: f64,
    ) void {
        const clipped = self.clipRect(bounds);
        if (clipped.empty() or column_count == 0 or row_count == 0 or columns.len < column_count * row_count) return;

        const lut = colormapLut(map);
        const grid_lut = gridlineLut(map);
        var y: i32 = 0;
        while (y < clipped.height) : (y += 1) {
            const source_row = @min(row_count - 1, @as(usize, @intCast(y)) * row_count / @as(usize, @intCast(clipped.height)));
            const table = if (gridlineForRow(min_hz, max_hz, @intCast(source_row), @intCast(row_count))) grid_lut else lut;
            var row_pixels = self.row(clipped.y + y);
            var x: i32 = 0;
            while (x < clipped.width) : (x += 1) {
                const source_col = @min(column_count - 1, @as(usize, @intCast(x)) * column_count / @as(usize, @intCast(clipped.width)));
                const db = columns[source_col * row_count + source_row];
                row_pixels[@intCast(clipped.x + x)] = table[dbIndex(db)];
            }
        }
    }

    /// Draw one spectrogram column into a single pixel column at `x` using a
    /// prebuilt row mapping (see buildRowMapping). The per-pixel work must
    /// stay a clamp and two table lookups: the live view draws thousands of
    /// these columns per frame.
    pub fn drawMappedSpectrogramColumn(
        self: *PixelBuffer,
        column: []const f32,
        x: i32,
        top: i32,
        mapping: RowMapping,
        lut: *const [color_lut_size]Color,
        grid_lut: *const [color_lut_size]Color,
    ) void {
        if (column.len == 0 or x < 0 or x >= self.width) return;
        const height = @min(
            mapping.source_rows.len,
            @as(usize, @intCast(@max(0, self.height - top))),
        );
        const column_index: usize = @intCast(x);
        for (0..height) |y| {
            const source_row = mapping.source_rows[y];
            if (source_row >= column.len) continue;
            const table = if (mapping.gridlines[y] != 0) grid_lut else lut;
            self.row(top + @as(i32, @intCast(y)))[column_index] = table[dbIndex(column[source_row])];
        }
    }

    pub fn drawSpectrumBars(
        self: *PixelBuffer,
        rows: []const f32,
        bounds: layout.Rect,
        map: colormap.Colormap,
        min_hz: f64,
        max_hz: f64,
    ) void {
        const clipped = self.clipRect(bounds);
        if (clipped.empty() or rows.len == 0) return;

        self.fillRect(clipped, palette.panel);
        var y: i32 = 0;
        while (y < clipped.height) : (y += 1) {
            const source_row = @min(rows.len - 1, @as(usize, @intCast(y)) * rows.len / @as(usize, @intCast(clipped.height)));
            const db = rows[source_row];
            const unit = std.math.clamp((db - floor_db) / (ceiling_db - floor_db), 0.0, 1.0);
            const bar_width: i32 = @intFromFloat(@round(unit * @as(f32, @floatFromInt(clipped.width))));
            const color = colorForDb(map, db);
            self.fillRect(layout.rect(clipped.x, clipped.y + y, bar_width, 1), color);
            if (gridlineForRow(min_hz, max_hz, @intCast(source_row), @intCast(rows.len))) {
                self.blendRect(layout.rect(clipped.x, clipped.y + y, clipped.width, 1), palette.grid, 0.25);
            }
        }
    }

    pub fn drawAmplitudeEnvelope(self: *PixelBuffer, amplitudes: []const f32, bounds: layout.Rect, color: Color) void {
        const clipped = self.clipRect(bounds);
        if (clipped.empty()) return;

        self.fillRect(clipped, blend(palette.panel, palette.background, 0.25));
        const baseline_y = clipped.bottom() - 2;
        self.fillRect(layout.rect(clipped.x, baseline_y, clipped.width, 1), palette.line);
        if (amplitudes.len == 0 or clipped.height <= 2) return;

        var x: i32 = 0;
        while (x < clipped.width) : (x += 1) {
            const source = @min(
                amplitudes.len - 1,
                @as(usize, @intCast(x)) * amplitudes.len / @as(usize, @intCast(clipped.width)),
            );
            const unit = std.math.clamp(amplitudes[source], 0.0, 1.0);
            const bar_height: i32 = @intFromFloat(@round(unit * @as(f32, @floatFromInt(clipped.height - 3))));
            if (bar_height <= 0) continue;
            self.fillRect(
                layout.rect(clipped.x + x, baseline_y - bar_height, 1, bar_height),
                blend(palette.panel, color, 0.76),
            );
        }
    }

    pub fn drawColormapPreview(self: *PixelBuffer, map: colormap.Colormap, bounds: layout.Rect) void {
        const clipped = self.clipRect(bounds);
        if (clipped.empty()) return;

        var x: i32 = 0;
        while (x < clipped.width) : (x += 1) {
            const unit: f32 = if (clipped.width <= 1) 0.0 else @as(f32, @floatFromInt(x)) / @as(f32, @floatFromInt(clipped.width - 1));
            self.fillRect(layout.rect(clipped.x + x, clipped.y, 1, clipped.height), packColor(map.sample(unit)));
        }
    }

    fn drawGlyph(self: *PixelBuffer, glyph_bits: [font.glyph_height]u8, x: i32, y: i32, scale: i32, color: Color) void {
        for (glyph_bits, 0..) |bits, row_index| {
            for (0..font.glyph_width) |column_index| {
                const mask: u8 = @as(u8, 1) << @intCast(font.glyph_width - 1 - column_index);
                if ((bits & mask) == 0) continue;
                self.fillRect(layout.rect(
                    x + @as(i32, @intCast(column_index)) * scale,
                    y + @as(i32, @intCast(row_index)) * scale,
                    scale,
                    scale,
                ), color);
            }
        }
    }

    fn drawGlyphClipped(self: *PixelBuffer, glyph_bits: [font.glyph_height]u8, x: i32, y: i32, scale: i32, color: Color, clip: layout.Rect) void {
        for (glyph_bits, 0..) |bits, row_index| {
            for (0..font.glyph_width) |column_index| {
                const mask: u8 = @as(u8, 1) << @intCast(font.glyph_width - 1 - column_index);
                if ((bits & mask) == 0) continue;
                const cell = layout.rect(
                    x + @as(i32, @intCast(column_index)) * scale,
                    y + @as(i32, @intCast(row_index)) * scale,
                    scale,
                    scale,
                ).intersection(clip);
                self.fillRect(cell, color);
            }
        }
    }

    fn clipRect(self: *const PixelBuffer, bounds: layout.Rect) layout.Rect {
        return bounds.intersection(layout.rect(0, 0, self.width, self.height));
    }
};

pub fn fillSpan(span: []Color, color: Color) void {
    const lanes = comptime (std.simd.suggestVectorLength(Color) orelse 8);
    const Vec = @Vector(lanes, Color);
    const splat: Vec = @splat(color);
    var index: usize = 0;
    while (index + lanes <= span.len) : (index += lanes) {
        span[index..][0..lanes].* = splat;
    }
    while (index < span.len) : (index += 1) {
        span[index] = color;
    }
}

pub fn blendSpan(span: []Color, color: Color, amount_arg: f32) void {
    const amount = std.math.clamp(amount_arg, 0.0, 1.0);
    const keep = 1.0 - amount;
    const lanes = comptime (std.simd.suggestVectorLength(Color) orelse 8);
    const Vec = @Vector(lanes, Color);
    const FVec = @Vector(lanes, f32);
    const alpha: Vec = @splat(0xFF000000);
    const red_shift: Vec = @splat(16);
    const green_shift: Vec = @splat(8);
    const byte_mask: Vec = @splat(0xFF);
    const keep_vec: FVec = @splat(keep);
    const amount_vec: FVec = @splat(amount);
    const over_red: FVec = @splat(@floatFromInt((color >> 16) & 0xFF));
    const over_green: FVec = @splat(@floatFromInt((color >> 8) & 0xFF));
    const over_blue: FVec = @splat(@floatFromInt(color & 0xFF));

    var index: usize = 0;
    while (index + lanes <= span.len) : (index += lanes) {
        const base: Vec = span[index..][0..lanes].*;
        const base_red: FVec = @floatFromInt((base >> red_shift) & byte_mask);
        const base_green: FVec = @floatFromInt((base >> green_shift) & byte_mask);
        const base_blue: FVec = @floatFromInt(base & byte_mask);
        const red: Vec = @intFromFloat(@round(base_red * keep_vec + over_red * amount_vec));
        const green: Vec = @intFromFloat(@round(base_green * keep_vec + over_green * amount_vec));
        const blue: Vec = @intFromFloat(@round(base_blue * keep_vec + over_blue * amount_vec));
        span[index..][0..lanes].* = alpha |
            ((red & byte_mask) << red_shift) |
            ((green & byte_mask) << green_shift) |
            (blue & byte_mask);
    }
    while (index < span.len) : (index += 1) {
        span[index] = blend(span[index], color, amount);
    }
}

pub fn absolutePeak(samples: []const f32) f32 {
    const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
    const Vec = @Vector(lanes, f32);
    var max_vec: Vec = @splat(0.0);
    var index: usize = 0;
    while (index + lanes <= samples.len) : (index += lanes) {
        const values: Vec = samples[index..][0..lanes].*;
        max_vec = @max(max_vec, @abs(values));
    }

    var peak = @reduce(.Max, max_vec);
    while (index < samples.len) : (index += 1) {
        peak = @max(peak, @abs(samples[index]));
    }
    return peak;
}

const SampleRange = struct {
    low: f32,
    high: f32,
};

fn sampleRange(samples: []const f32) SampleRange {
    std.debug.assert(samples.len > 0);

    const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
    const Vec = @Vector(lanes, f32);
    var low_vec: Vec = @splat(samples[0]);
    var high_vec: Vec = @splat(samples[0]);
    var index: usize = 0;
    while (index + lanes <= samples.len) : (index += lanes) {
        const values: Vec = samples[index..][0..lanes].*;
        low_vec = @min(low_vec, values);
        high_vec = @max(high_vec, values);
    }

    var low = @reduce(.Min, low_vec);
    var high = @reduce(.Max, high_vec);
    while (index < samples.len) : (index += 1) {
        low = @min(low, samples[index]);
        high = @max(high, samples[index]);
    }
    return .{ .low = low, .high = high };
}

pub fn waveformY(value: f32, gain: f32, rows: i32) i32 {
    if (rows <= 1) return 0;
    const scaled = std.math.clamp(value * gain, -1.0, 1.0);
    const center = @divTrunc(rows, 2);
    const y = center - @as(i32, @intFromFloat(@round(scaled * @as(f32, @floatFromInt(center)))));
    return std.math.clamp(y, 0, rows - 1);
}

/// Precomputed display-row -> source-row and gridline flags for one plot
/// height, so column blits do no per-pixel frequency math.
pub const RowMapping = struct {
    source_rows: []u32,
    gridlines: []u8,
};

pub fn buildRowMapping(mapping: RowMapping, row_count: usize, min_hz: f64, max_hz: f64) void {
    std.debug.assert(mapping.source_rows.len == mapping.gridlines.len);
    if (row_count == 0) return;
    const height = mapping.source_rows.len;
    for (0..height) |y| {
        const source_row = @min(row_count - 1, y * row_count / height);
        mapping.source_rows[y] = @intCast(source_row);
        mapping.gridlines[y] = @intFromBool(
            gridlineForRow(min_hz, max_hz, @intCast(source_row), @intCast(row_count)),
        );
    }
}

pub fn gridlineForRow(min_hz: f64, max_hz: f64, row: i32, row_count: i32) bool {
    for (layout.frequency_ticks) |tick| {
        if (!tick.gridline or tick.hz < min_hz or tick.hz > max_hz) continue;
        if (layout.rowForFrequency(min_hz, max_hz, tick.hz, row_count)) |tick_row| {
            if (tick_row == row) return true;
        }
    }
    return false;
}

fn unitByte(value: f32) u8 {
    const clamped = std.math.clamp(value, 0.0, 1.0);
    return @intFromFloat(@round(clamped * 255.0));
}

test "vector fill writes a complete span and scalar tail" {
    var pixels: [19]Color = undefined;
    fillSpan(&pixels, rgb(1, 2, 3));
    for (pixels) |pixel| try std.testing.expectEqual(rgb(1, 2, 3), pixel);
}

test "waveform peak uses absolute values and clamps y" {
    const samples = [_]f32{ -0.25, 0.50, -0.75, 0.10, 1.25 };
    try std.testing.expectApproxEqAbs(@as(f32, 1.25), absolutePeak(&samples), 0.0001);
    try std.testing.expectEqual(@as(i32, 0), waveformY(2.0, 1.0, 20));
    try std.testing.expectEqual(@as(i32, 19), waveformY(-2.0, 1.0, 20));
}

test "text rendering touches glyph pixels" {
    var pixels = [_]Color{rgb(0, 0, 0)} ** (32 * 16);
    var buffer = PixelBuffer{ .pixels = &pixels, .width = 32, .height = 16 };
    buffer.drawText("A", 1, 1, 1, rgb(255, 255, 255));

    var lit = false;
    for (pixels) |pixel| {
        if (pixel == rgb(255, 255, 255)) lit = true;
    }
    try std.testing.expect(lit);
}

test "clipped text stays inside the requested rectangle" {
    var pixels = [_]Color{rgb(0, 0, 0)} ** (24 * 12);
    var buffer = PixelBuffer{ .pixels = &pixels, .width = 24, .height = 12 };
    buffer.drawTextClipped("AAAA", -4, 1, 1, rgb(255, 255, 255), layout.rect(6, 0, 8, 12));

    for (0..12) |y| {
        for (0..24) |x| {
            const pixel = pixels[y * 24 + x];
            if (pixel == rgb(255, 255, 255)) {
                try std.testing.expect(x >= 6 and x < 14);
            }
        }
    }
}
