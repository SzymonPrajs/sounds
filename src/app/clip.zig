//! Selected recording clip and basic trimming.
//! Rewrite target; C reference: src_c/src/app/clip.c

const std = @import("std");
const ring_buffer = @import("../audio/ring_buffer.zig");

pub const label_capacity = 64;
const maximum_clip_sample_rate = 512_000.0;

pub const Error = error{
    InvalidClip,
    ClipTooLarge,
    NotEnoughAudio,
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

    pub fn replaceFromRing(
        self: *Clip,
        ring: *const ring_buffer.RingBuffer,
        end_sample: u64,
        requested_samples: u64,
        sample_rate: f64,
        label_arg: []const u8,
    ) !void {
        if (requested_samples == 0 or
            !std.math.isFinite(sample_rate) or
            sample_rate <= 0.0 or
            sample_rate > maximum_clip_sample_rate)
        {
            return Error.InvalidClip;
        }

        const wanted_u64 = @min(requested_samples, @as(u64, @intCast(ring.capacity())));
        if (wanted_u64 == 0 or wanted_u64 > std.math.maxInt(usize)) return Error.ClipTooLarge;
        const wanted: usize = @intCast(wanted_u64);

        const buffer = try self.allocator.alloc(f32, wanted);
        defer self.allocator.free(buffer);

        const count = ring.readAvailableEndingAt(end_sample, buffer);
        if (count == 0) return Error.NotEnoughAudio;
        try self.replace(buffer[0..count], sample_rate, label_arg);
    }

    pub fn clearTrim(self: *Clip) void {
        self.trim_start = 0;
        self.trim_end = @intCast(self.samples.len);
    }

    pub fn trimStartBy(self: *Clip, samples: u64) void {
        if (self.trim_end <= self.trim_start + 1) return;
        const limit = self.trim_end - 1;
        self.trim_start = @min(self.trim_start +| samples, limit);
    }

    pub fn trimEndBy(self: *Clip, samples: u64) void {
        if (self.trim_end <= self.trim_start + 1) return;
        const limit = self.trim_start + 1;
        self.trim_end = if (self.trim_end > limit +| samples) self.trim_end - samples else limit;
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
    clip.trimStartBy(99);
    try std.testing.expectEqual(@as(u64, 2), clip.trim_start);
    try std.testing.expectEqual(@as(u64, 3), clip.trim_end);
    clip.trimEndBy(99);
    try std.testing.expectEqual(@as(u64, 2), clip.trim_start);
    try std.testing.expectEqual(@as(u64, 3), clip.trim_end);
    try std.testing.expectApproxEqAbs(@as(f64, 1.0 / 3.0), clip.durationSeconds(), 0.0001);
}

test "clip replacement from ring tolerates stale stop reads" {
    var ring = try ring_buffer.RingBuffer.init(std.testing.allocator, 4);
    defer ring.deinit();

    ring.write(&[_]f32{ -1.0, -0.25, 0.25, 1.0 });
    const stopped_at = ring.written();
    ring.write(&[_]f32{5.0});

    var clip = Clip.init(std.testing.allocator);
    defer clip.deinit();
    try clip.replaceFromRing(&ring, stopped_at, 4, 48_000.0, "stale");

    try std.testing.expectEqualSlices(f32, &[_]f32{ -0.25, 0.25, 1.0 }, clip.activeSamples());
}
