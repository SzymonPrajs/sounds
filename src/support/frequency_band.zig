//! Frequency band definitions (whole/low/mid/high/custom) and range math.

const std = @import("std");

pub const full_min_hz: f64 = 10.0;
pub const full_max_hz: f64 = 24_000.0;
pub const low_max_hz: f64 = 200.0;
pub const mid_min_hz: f64 = 100.0;
pub const mid_max_hz: f64 = 2_400.0;
pub const high_min_hz: f64 = 2_000.0;

pub const Range = struct {
    min_hz: f64,
    max_hz: f64,

    pub fn whole() Range {
        return .{ .min_hz = full_min_hz, .max_hz = full_max_hz };
    }

    pub fn zero() Range {
        return .{ .min_hz = 0.0, .max_hz = 0.0 };
    }

    pub fn isValidFull(self: Range) bool {
        return self.min_hz > 0.0 and self.max_hz > self.min_hz;
    }
};

pub const default_custom = Range{
    .min_hz = mid_min_hz,
    .max_hz = mid_max_hz,
};

pub const FrequencyBand = enum {
    whole,
    low,
    mid,
    high,
    custom,
    bands,

    pub fn index(self: FrequencyBand) usize {
        return @intFromEnum(self);
    }

    pub fn name(self: FrequencyBand) []const u8 {
        return metadata[@intFromEnum(self)].name;
    }

    pub fn title(self: FrequencyBand) []const u8 {
        return metadata[@intFromEnum(self)].title;
    }

    pub fn rangeLabel(self: FrequencyBand) []const u8 {
        return metadata[@intFromEnum(self)].range_label;
    }

    pub fn showsAllBands(self: FrequencyBand) bool {
        return self == .bands;
    }

    pub fn limits(self: FrequencyBand, full: Range, custom: Range) Range {
        // Reject non-finite or inverted full ranges before deriving sub-bands.
        if (!full.isValidFull()) return .zero();

        var low = full.min_hz;
        var high = full.max_hz;

        switch (self) {
            .whole, .bands => {},
            .low => high = @min(full.max_hz, low_max_hz),
            .mid => {
                low = @max(full.min_hz, mid_min_hz);
                high = @min(full.max_hz, mid_max_hz);
            },
            .high => low = @max(full.min_hz, high_min_hz),
            .custom => if (isCustomRangeValid(full, custom)) {
                low = @max(full.min_hz, custom.min_hz);
                high = @min(full.max_hz, custom.max_hz);
            },
        }

        if (high <= low) return full;
        return .{ .min_hz = low, .max_hz = high };
    }
};

pub const Selection = union(enum) {
    whole,
    low,
    mid,
    high,
    custom: Range,
    bands,

    pub fn band(self: Selection) FrequencyBand {
        return switch (self) {
            .whole => .whole,
            .low => .low,
            .mid => .mid,
            .high => .high,
            .custom => .custom,
            .bands => .bands,
        };
    }

    pub fn limits(self: Selection, full: Range) Range {
        return switch (self) {
            .custom => |custom| FrequencyBand.custom.limits(full, custom),
            inline else => self.band().limits(full, default_custom),
        };
    }
};

pub const order = [_]FrequencyBand{
    .whole,
    .low,
    .mid,
    .high,
    .custom,
    .bands,
};

pub const count = order.len;

const Metadata = struct {
    name: []const u8,
    title: []const u8,
    range_label: []const u8,
};

const metadata = [_]Metadata{
    .{ .name = "whole", .title = "whole spectrogram", .range_label = "10 Hz-24 kHz" },
    .{ .name = "low", .title = "low frequencies", .range_label = "10-200 Hz" },
    .{ .name = "mid", .title = "mid frequencies", .range_label = "100 Hz-2.4 kHz" },
    .{ .name = "high", .title = "high frequencies", .range_label = "2-24 kHz" },
    .{ .name = "custom", .title = "custom range", .range_label = "typed low / high" },
    .{ .name = "bands", .title = "banded spectrogram", .range_label = "overlapping bands" },
};

pub fn at(index: isize) FrequencyBand {
    if (index < 0) return .whole;

    const i: usize = @intCast(index);
    if (i >= order.len) return .whole;
    return order[i];
}

pub fn fromInt(value: i32) ?FrequencyBand {
    return switch (value) {
        0 => .whole,
        1 => .low,
        2 => .mid,
        3 => .high,
        4 => .custom,
        5 => .bands,
        else => null,
    };
}

pub fn isCustomRangeValid(full: Range, custom: Range) bool {
    return full.isValidFull() and
        custom.min_hz >= full.min_hz and
        custom.max_hz <= full.max_hz and
        custom.max_hz > custom.min_hz * 1.01;
}

test "frequency band order and metadata are stable" {
    try std.testing.expectEqual(@as(usize, 6), count);
    try std.testing.expectEqual(FrequencyBand.whole, at(-1));
    try std.testing.expectEqual(FrequencyBand.whole, at(6));

    for (order, 0..) |band, i| {
        try std.testing.expectEqual(i, band.index());
        try std.testing.expectEqual(band, at(@intCast(i)));
    }

    try std.testing.expectEqualStrings("whole", FrequencyBand.whole.name());
    try std.testing.expectEqualStrings("low frequencies", FrequencyBand.low.title());
    try std.testing.expectEqualStrings("100 Hz-2.4 kHz", FrequencyBand.mid.rangeLabel());
    try std.testing.expect(FrequencyBand.bands.showsAllBands());
    try std.testing.expect(!FrequencyBand.high.showsAllBands());
    try std.testing.expectEqual(@as(?FrequencyBand, null), fromInt(-1));
    try std.testing.expectEqual(@as(?FrequencyBand, FrequencyBand.bands), fromInt(5));
}

test "frequency band limits match reference ranges and fallback behavior" {
    const full = Range.whole();

    try std.testing.expectEqual(Range{ .min_hz = 10.0, .max_hz = 24_000.0 }, FrequencyBand.whole.limits(full, default_custom));
    try std.testing.expectEqual(Range{ .min_hz = 10.0, .max_hz = 200.0 }, FrequencyBand.low.limits(full, default_custom));
    try std.testing.expectEqual(Range{ .min_hz = 100.0, .max_hz = 2_400.0 }, FrequencyBand.mid.limits(full, default_custom));
    try std.testing.expectEqual(Range{ .min_hz = 2_000.0, .max_hz = 24_000.0 }, FrequencyBand.high.limits(full, default_custom));
    try std.testing.expectEqual(Range{ .min_hz = 10.0, .max_hz = 24_000.0 }, FrequencyBand.bands.limits(full, default_custom));

    try std.testing.expectEqual(
        Range{ .min_hz = 250.0, .max_hz = 750.0 },
        FrequencyBand.custom.limits(full, .{ .min_hz = 250.0, .max_hz = 750.0 }),
    );
    try std.testing.expectEqual(
        full,
        FrequencyBand.custom.limits(full, .{ .min_hz = 100.0, .max_hz = 101.0 }),
    );
    try std.testing.expectEqual(
        Range.zero(),
        FrequencyBand.low.limits(.{ .min_hz = 0.0, .max_hz = 24_000.0 }, default_custom),
    );
    try std.testing.expectEqual(
        Range.zero(),
        FrequencyBand.low.limits(.{ .min_hz = std.math.nan(f64), .max_hz = 24_000.0 }, default_custom),
    );

    const out_of_low = Range{ .min_hz = 500.0, .max_hz = 1_000.0 };
    try std.testing.expectEqual(out_of_low, FrequencyBand.low.limits(out_of_low, default_custom));
}

test "typed custom selections carry their own range" {
    const full = Range.whole();
    const selected = Selection{ .custom = .{ .min_hz = 120.0, .max_hz = 1_000.0 } };

    try std.testing.expectEqual(FrequencyBand.custom, selected.band());
    try std.testing.expectEqual(
        Range{ .min_hz = 120.0, .max_hz = 1_000.0 },
        selected.limits(full),
    );
    try std.testing.expectEqual(
        Range{ .min_hz = 2_000.0, .max_hz = 24_000.0 },
        (@as(Selection, .high)).limits(full),
    );
}
