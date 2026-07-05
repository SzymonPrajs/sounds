//! Selected recording clip, label, and trim range state.

const std = @import("std");

pub const label_capacity = 64;
const maximum_clip_sample_rate = 512_000.0;

pub const Error = error{
    InvalidClip,
};

pub const Clip = struct {
    allocator: std.mem.Allocator,
    samples: []f32 = &.{},
    trim_start: u64 = 0,
    trim_end: u64 = 0,
    sample_rate: f64 = 0.0,
    label_storage: [label_capacity]u8 = [_]u8{0} ** label_capacity,
    label_len: usize = 0,

    pub fn init(allocator: std.mem.Allocator) Clip {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *Clip) void {
        self.allocator.free(self.samples);
        self.* = undefined;
    }

    pub fn clear(self: *Clip) void {
        self.allocator.free(self.samples);
        self.samples = &.{};
        self.trim_start = 0;
        self.trim_end = 0;
        self.sample_rate = 0.0;
        self.label_len = 0;
    }

    pub fn hasAudio(self: *const Clip) bool {
        return self.samples.len > 0 and self.trim_end > self.trim_start;
    }

    pub fn fullSamples(self: *const Clip) []const f32 {
        return self.samples;
    }

    pub fn activeSamples(self: *const Clip) []const f32 {
        if (!self.hasAudio()) return &.{};
        const start: usize = @intCast(@min(self.trim_start, self.samples.len));
        const end: usize = @intCast(@min(self.trim_end, self.samples.len));
        if (end <= start) return &.{};
        return self.samples[start..end];
    }

    pub fn activeSampleCount(self: *const Clip) u64 {
        return @intCast(self.activeSamples().len);
    }

    pub fn durationSeconds(self: *const Clip) f64 {
        if (self.sample_rate <= 0.0) return 0.0;
        return @as(f64, @floatFromInt(self.activeSampleCount())) / self.sample_rate;
    }

    pub fn trimStartSeconds(self: *const Clip) f64 {
        if (self.sample_rate <= 0.0) return 0.0;
        return @as(f64, @floatFromInt(self.trim_start)) / self.sample_rate;
    }

    pub fn trimEndSeconds(self: *const Clip) f64 {
        if (self.sample_rate <= 0.0 or self.trim_end > self.samples.len) return 0.0;
        return @as(f64, @floatFromInt(self.samples.len - @as(usize, @intCast(self.trim_end)))) / self.sample_rate;
    }

    pub fn label(self: *const Clip) []const u8 {
        return self.label_storage[0..self.label_len];
    }

    pub fn replace(
        self: *Clip,
        samples: []const f32,
        sample_rate: f64,
        label_arg: []const u8,
    ) !void {
        if (samples.len == 0 or
            !std.math.isFinite(sample_rate) or
            sample_rate <= 0.0 or
            sample_rate > maximum_clip_sample_rate)
        {
            return Error.InvalidClip;
        }

        const copy = try self.allocator.dupe(f32, samples);
        errdefer self.allocator.free(copy);

        self.allocator.free(self.samples);
        self.samples = copy;
        self.trim_start = 0;
        self.trim_end = @intCast(copy.len);
        self.sample_rate = sample_rate;
        self.setLabel(label_arg);
    }

    pub fn clearTrim(self: *Clip) void {
        self.trim_start = 0;
        self.trim_end = @intCast(self.samples.len);
    }

    pub fn setTrim(self: *Clip, start_arg: u64, end_arg: u64) void {
        const sample_count: u64 = @intCast(self.samples.len);
        var start = @min(start_arg, sample_count);
        var end = @min(end_arg, sample_count);
        if (end <= start) {
            if (sample_count == 0) {
                start = 0;
                end = 0;
            } else if (start >= sample_count) {
                start = sample_count - 1;
                end = sample_count;
            } else {
                end = start + 1;
            }
        }
        self.trim_start = start;
        self.trim_end = end;
    }

    pub fn setLabel(self: *Clip, label_arg: []const u8) void {
        const source = if (label_arg.len == 0) "clip" else label_arg;
        self.label_len = @min(source.len, self.label_storage.len);
        @memcpy(self.label_storage[0..self.label_len], source[0..self.label_len]);
    }
};

test "clip replace owns samples and resets trim" {
    var clip = Clip.init(std.testing.allocator);
    defer clip.deinit();

    var samples = [_]f32{ 1.0, 2.0, 3.0, 4.0 };
    try clip.replace(&samples, 48_000.0, "test");
    samples[1] = 99.0;

    try std.testing.expect(clip.hasAudio());
    try std.testing.expectEqualSlices(f32, &[_]f32{ 1.0, 2.0, 3.0, 4.0 }, clip.activeSamples());
    try std.testing.expectEqualStrings("test", clip.label());
    try std.testing.expectEqual(@as(u64, 0), clip.trim_start);
    try std.testing.expectEqual(@as(u64, 4), clip.trim_end);
}

test "clip trim preserves at least one sample" {
    var clip = Clip.init(std.testing.allocator);
    defer clip.deinit();

    try clip.replace(&[_]f32{ 1.0, 2.0, 3.0 }, 3.0, "");
    clip.setTrim(99, 99);
    try std.testing.expectEqual(@as(u64, 2), clip.trim_start);
    try std.testing.expectEqual(@as(u64, 3), clip.trim_end);
    clip.setTrim(2, 0);
    try std.testing.expectEqual(@as(u64, 2), clip.trim_start);
    try std.testing.expectEqual(@as(u64, 3), clip.trim_end);
    try std.testing.expectApproxEqAbs(@as(f64, 1.0 / 3.0), clip.durationSeconds(), 0.0001);
}
