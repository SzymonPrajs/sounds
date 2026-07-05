//! Sample ring buffer between the capture IOProc and analysis.
//! Rewrite target; C reference: src_c/src/audio/ring_buffer.c

const std = @import("std");

pub const Error = error{
    InvalidCapacity,
};

pub const RingBuffer = struct {
    allocator: std.mem.Allocator,
    samples: []f32,
    write_index: std.atomic.Value(u64),

    pub fn init(allocator: std.mem.Allocator, sample_capacity: usize) !RingBuffer {
        if (sample_capacity == 0) return Error.InvalidCapacity;

        const samples = try allocator.alloc(f32, sample_capacity);
        errdefer allocator.free(samples);
        @memset(samples, 0.0);

        return .{
            .allocator = allocator,
            .samples = samples,
            .write_index = std.atomic.Value(u64).init(0),
        };
    }

    pub fn deinit(self: *RingBuffer) void {
        self.allocator.free(self.samples);
        self.* = undefined;
    }

    pub fn capacity(self: *const RingBuffer) usize {
        return self.samples.len;
    }

    pub fn written(self: *const RingBuffer) u64 {
        return self.write_index.load(.acquire);
    }

    pub fn write(self: *RingBuffer, input: []const f32) void {
        if (input.len == 0) return;

        var samples = input;
        if (samples.len > self.samples.len) {
            samples = samples[samples.len - self.samples.len ..];
        }

        const start = self.write_index.load(.monotonic);
        self.copyInto(start, samples);
        self.write_index.store(start +% @as(u64, @intCast(samples.len)), .release);
    }

    pub fn readLatest(self: *const RingBuffer, out: []f32) usize {
        if (out.len == 0) return 0;

        const written_count = self.write_index.load(.acquire);
        const available = @min(written_count, self.capacityU64());
        const to_read = @min(@as(u64, @intCast(out.len)), available);
        const start = written_count - to_read;

        self.copyFrom(start, out[0..@intCast(to_read)]);
        return @intCast(to_read);
    }

    pub fn readEndingAt(self: *const RingBuffer, end_index: u64, out: []f32) usize {
        if (out.len == 0) return 0;

        const requested: u64 = @intCast(out.len);
        const written_count = self.write_index.load(.acquire);
        const available = @min(written_count, self.capacityU64());
        const oldest = written_count - available;

        if (end_index > written_count) return 0;
        if (end_index < requested or end_index - requested < oldest) return 0;

        const start = end_index - requested;
        self.copyFrom(start, out);
        return out.len;
    }

    pub fn readAvailableEndingAt(self: *const RingBuffer, end_index: u64, out: []f32) usize {
        if (out.len == 0) return 0;

        const requested: u64 = @intCast(out.len);
        const written_count = self.write_index.load(.acquire);
        const available = @min(written_count, self.capacityU64());
        const oldest = written_count - available;

        if (end_index > written_count or end_index <= oldest) return 0;

        const requested_start = if (end_index > requested) end_index - requested else 0;
        const start = @max(requested_start, oldest);
        const to_read = end_index - start;

        self.copyFrom(start, out[0..@intCast(to_read)]);
        return @intCast(to_read);
    }

    fn capacityU64(self: *const RingBuffer) u64 {
        return @intCast(self.samples.len);
    }

    fn copyInto(self: *RingBuffer, index: u64, input: []const f32) void {
        const sample_offset = self.offset(index);
        const first = @min(self.samples.len - sample_offset, input.len);

        @memcpy(self.samples[sample_offset..][0..first], input[0..first]);

        const remaining = input.len - first;
        if (remaining > 0) {
            @memcpy(self.samples[0..remaining], input[first..][0..remaining]);
        }
    }

    fn copyFrom(self: *const RingBuffer, index: u64, out: []f32) void {
        const sample_offset = self.offset(index);
        const first = @min(self.samples.len - sample_offset, out.len);

        @memcpy(out[0..first], self.samples[sample_offset..][0..first]);

        const remaining = out.len - first;
        if (remaining > 0) {
            @memcpy(out[first..][0..remaining], self.samples[0..remaining]);
        }
    }

    fn offset(self: *const RingBuffer, index: u64) usize {
        return @intCast(index % self.capacityU64());
    }
};

test "ring buffer rejects zero capacity and starts empty" {
    try std.testing.expectError(
        Error.InvalidCapacity,
        RingBuffer.init(std.testing.allocator, 0),
    );

    var ring = try RingBuffer.init(std.testing.allocator, 4);
    defer ring.deinit();

    var out: [4]f32 = undefined;
    try std.testing.expectEqual(@as(usize, 4), ring.capacity());
    try std.testing.expectEqual(@as(u64, 0), ring.written());
    try std.testing.expectEqual(@as(usize, 0), ring.readLatest(out[0..]));
}

test "ring buffer reads latest samples without wrapping" {
    var ring = try RingBuffer.init(std.testing.allocator, 16);
    defer ring.deinit();

    const samples = [_]f32{ -1.0, -0.25, 0.25, 1.0 };
    ring.write(&samples);

    var out: [8]f32 = undefined;
    const count = ring.readLatest(out[0..]);

    try std.testing.expectEqual(@as(u64, 4), ring.written());
    try std.testing.expectEqual(@as(usize, 4), count);
    try std.testing.expectEqualSlices(f32, &samples, out[0..count]);
}

test "ring buffer keeps the newest window when writes wrap or exceed capacity" {
    var ring = try RingBuffer.init(std.testing.allocator, 4);
    defer ring.deinit();

    ring.write(&[_]f32{ 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 });

    var out: [4]f32 = undefined;
    try std.testing.expectEqual(@as(u64, 4), ring.written());
    try std.testing.expectEqual(@as(usize, 4), ring.readLatest(out[0..]));
    try std.testing.expectEqualSlices(f32, &[_]f32{ 3.0, 4.0, 5.0, 6.0 }, &out);

    ring.write(&[_]f32{ 7.0, 8.0 });

    try std.testing.expectEqual(@as(u64, 6), ring.written());
    try std.testing.expectEqual(@as(usize, 4), ring.readLatest(out[0..]));
    try std.testing.expectEqualSlices(f32, &[_]f32{ 5.0, 6.0, 7.0, 8.0 }, &out);
}

test "ring buffer strict ending reads require a fully retained window" {
    var ring = try RingBuffer.init(std.testing.allocator, 4);
    defer ring.deinit();

    ring.write(&[_]f32{ 10.0, 11.0, 12.0, 13.0 });
    ring.write(&[_]f32{ 14.0, 15.0 });

    var out: [3]f32 = undefined;

    try std.testing.expectEqual(@as(usize, 3), ring.readEndingAt(6, out[0..]));
    try std.testing.expectEqualSlices(f32, &[_]f32{ 13.0, 14.0, 15.0 }, &out);

    try std.testing.expectEqual(@as(usize, 0), ring.readEndingAt(3, out[0..]));
    try std.testing.expectEqual(@as(usize, 0), ring.readEndingAt(7, out[0..]));
}

test "ring buffer available ending reads preserve stale recording stop semantics" {
    var ring = try RingBuffer.init(std.testing.allocator, 4);
    defer ring.deinit();

    const initial = [_]f32{ -1.0, -0.25, 0.25, 1.0 };
    ring.write(&initial);

    const stopped_at = ring.written();
    ring.write(&[_]f32{5.0});

    var stopped: [4]f32 = undefined;
    const count = ring.readAvailableEndingAt(stopped_at, stopped[0..]);

    try std.testing.expectEqual(@as(usize, 3), count);
    try std.testing.expectEqualSlices(f32, &[_]f32{ -0.25, 0.25, 1.0 }, stopped[0..count]);
    try std.testing.expectEqual(@as(usize, 0), ring.readEndingAt(stopped_at, stopped[0..]));

    var latest: [4]f32 = undefined;
    try std.testing.expectEqual(@as(usize, 4), ring.readLatest(latest[0..]));
    try std.testing.expectEqualSlices(f32, &[_]f32{ -0.25, 0.25, 1.0, 5.0 }, &latest);
}

test "ring buffer supports spectrum-style full-capacity writes and ending reads" {
    var ring = try RingBuffer.init(std.testing.allocator, 480);
    defer ring.deinit();

    var samples: [480]f32 = .{0.0} ** 480;
    samples[160] = 1.0;
    ring.write(&samples);

    var window: [5]f32 = undefined;
    try std.testing.expectEqual(@as(usize, 5), ring.readEndingAt(163, window[0..]));
    try std.testing.expectEqualSlices(f32, &[_]f32{ 0.0, 0.0, 1.0, 0.0, 0.0 }, &window);
}
