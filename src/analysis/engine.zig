//! Live analysis engine and mode registry.
//! Each mode owns its concrete analyzer while the engine manages shared output
//! storage, selected frequency range, and timeline resets.

const std = @import("std");
const tonal = @import("tonal.zig");
const spectral_mode = @import("spectral_mode.zig");
const spectrum = @import("spectrum.zig");
const ring_buffer = @import("../audio/ring_buffer.zig");
const frequency_band = @import("../support/frequency_band.zig");

pub const Error = error{
    InvalidRequest,
    MissingMode,
    TooLarge,
};

const minimum_column_samples = 32;
const maximum_analysis_sample_rate = 512000.0;

pub const Mode = enum(usize) {
    transient,
    tonal,
    reassigned,
    squeezed,
    superlet,
    multitaper,
    s_transform,
    sparse,

    pub fn index(self: Mode) usize {
        return @intFromEnum(self);
    }
};

pub const mode_count = 8;

pub const Frame = struct {
    columns: []const f32 = &.{},
    column_count: usize = 0,
    row_count: usize = 0,
};

const Algorithm = union(Mode) {
    transient: spectral_mode.Algorithm,
    tonal: tonal.Algorithm,
    reassigned: spectral_mode.Algorithm,
    squeezed: spectral_mode.Algorithm,
    superlet: spectral_mode.Algorithm,
    multitaper: spectral_mode.Algorithm,
    s_transform: spectral_mode.Algorithm,
    sparse: spectral_mode.Algorithm,

    fn deinit(self: *Algorithm) void {
        switch (self.*) {
            inline else => |*algorithm| algorithm.deinit(),
        }
    }

    fn minFrequency(self: *const Algorithm) f64 {
        return switch (self.*) {
            inline else => |*algorithm| algorithm.minFrequency(),
        };
    }

    fn maxFrequency(self: *const Algorithm) f64 {
        return switch (self.*) {
            inline else => |*algorithm| algorithm.maxFrequency(),
        };
    }

    fn setFrequencyRange(self: *Algorithm, min_hz: f64, max_hz: f64) !void {
        switch (self.*) {
            inline else => |*algorithm| try algorithm.setFrequencyRange(min_hz, max_hz),
        }
    }

    fn reset(self: *Algorithm, written_samples: u64) void {
        switch (self.*) {
            inline else => |*algorithm| algorithm.reset(written_samples),
        }
    }

    fn toggleSst(self: *Algorithm) void {
        switch (self.*) {
            .tonal => |*algorithm| algorithm.toggleSst(),
            else => {},
        }
    }

    fn sstEnabled(self: *const Algorithm) bool {
        return switch (self.*) {
            .tonal => |*algorithm| algorithm.sstEnabled(),
            else => false,
        };
    }

    fn update(
        self: *Algorithm,
        ring: *const ring_buffer.RingBuffer,
        written_samples: u64,
        ring_capacity: u64,
        row_count: usize,
        column_limit: usize,
        columns: []f32,
        column_count: *usize,
    ) !void {
        switch (self.*) {
            inline else => |*algorithm| try algorithm.update(
                ring,
                written_samples,
                ring_capacity,
                row_count,
                column_limit,
                columns,
                column_count,
            ),
        }
    }
};

pub const Engine = struct {
    allocator: std.mem.Allocator,
    algorithms: [mode_count]?Algorithm,
    mode: Mode,
    selected_band: frequency_band.FrequencyBand,
    full_min_hz: f64,
    full_max_hz: f64,
    min_hz: f64,
    max_hz: f64,
    columns: []f32,
    column_capacity_rows: usize,
    column_capacity_columns: usize,

    pub fn init(
        allocator: std.mem.Allocator,
        sample_rate: f64,
        columns_per_second: f64,
    ) !Engine {
        if (!std.math.isFinite(sample_rate) or
            sample_rate <= 0.0 or
            sample_rate > maximum_analysis_sample_rate or
            !std.math.isFinite(columns_per_second) or
            columns_per_second <= 0.0)
        {
            return Error.InvalidRequest;
        }

        var engine = Engine{
            .allocator = allocator,
            .algorithms = .{null} ** mode_count,
            .mode = .tonal,
            .selected_band = .whole,
            .full_min_hz = 0.0,
            .full_max_hz = 0.0,
            .min_hz = 0.0,
            .max_hz = 0.0,
            .columns = &.{},
            .column_capacity_rows = 0,
            .column_capacity_columns = 0,
        };
        errdefer engine.deinit();

        try engine.createAlgorithms(sample_rate, columns_per_second);

        const first = engine.algorithmForMode(.transient) orelse return Error.MissingMode;
        engine.full_min_hz = first.minFrequency();
        engine.full_max_hz = first.maxFrequency();
        engine.min_hz = engine.full_min_hz;
        engine.max_hz = engine.full_max_hz;

        try engine.applyFrequencyRange(engine.min_hz, engine.max_hz);
        return engine;
    }

    pub fn deinit(self: *Engine) void {
        for (&self.algorithms) |*slot| {
            if (slot.*) |*algorithm| algorithm.deinit();
            slot.* = null;
        }

        self.allocator.free(self.columns);
        self.* = undefined;
    }

    pub fn minFrequency(self: *const Engine) f64 {
        return self.min_hz;
    }

    pub fn maxFrequency(self: *const Engine) f64 {
        return self.max_hz;
    }

    pub fn fullMinFrequency(self: *const Engine) f64 {
        return self.full_min_hz;
    }

    pub fn fullMaxFrequency(self: *const Engine) f64 {
        return self.full_max_hz;
    }

    pub fn frequencyBand(self: *const Engine) frequency_band.FrequencyBand {
        return self.selected_band;
    }

    pub fn setFrequencyBand(
        self: *Engine,
        band: frequency_band.FrequencyBand,
        custom: frequency_band.Range,
    ) !void {
        const full = frequency_band.Range{
            .min_hz = self.full_min_hz,
            .max_hz = self.full_max_hz,
        };
        const limits = band.limits(full, custom);

        if (limits.max_hz <= limits.min_hz) return Error.InvalidRequest;

        try self.applyFrequencyRange(limits.min_hz, limits.max_hz);
        self.selected_band = band;
    }

    pub fn setMode(self: *Engine, mode: Mode) void {
        self.mode = mode;
    }

    pub fn toggleSst(self: *Engine) void {
        if (self.algorithmForMode(.tonal)) |algorithm| algorithm.toggleSst();
    }

    pub fn sstEnabled(self: *const Engine) bool {
        if (self.constAlgorithmForMode(.tonal)) |algorithm| return algorithm.sstEnabled();
        return false;
    }

    pub fn resetTimeline(self: *Engine, written_samples: u64) void {
        for (&self.algorithms) |*slot| {
            if (slot.*) |*algorithm| algorithm.reset(written_samples);
        }
    }

    pub fn resetModeTimeline(self: *Engine, mode: Mode, written_samples: u64) void {
        if (self.algorithmForMode(mode)) |algorithm| algorithm.reset(written_samples);
    }

    pub fn update(
        self: *Engine,
        ring: *const ring_buffer.RingBuffer,
        written_samples: u64,
        ring_capacity: u64,
        row_count: usize,
        column_limit_arg: usize,
    ) !Frame {
        var frame = Frame{
            .row_count = row_count,
        };

        if (row_count == 0) return frame;

        const column_limit = if (column_limit_arg == 0) 1 else column_limit_arg;
        try self.ensureColumnStorage(row_count, column_limit);

        var column_count: usize = 0;
        const algorithm = self.algorithmForMode(self.mode) orelse return Error.MissingMode;
        try algorithm.update(
            ring,
            written_samples,
            ring_capacity,
            row_count,
            column_limit,
            self.columns,
            &column_count,
        );

        frame.columns = self.columns[0 .. column_count * row_count];
        frame.column_count = column_count;
        return frame;
    }

    fn createAlgorithms(self: *Engine, sample_rate: f64, columns_per_second: f64) !void {
        const column_samples = columnSamplesForRate(sample_rate, columns_per_second);

        self.algorithms[Mode.transient.index()] = .{
            .transient = try spectral_mode.Algorithm.init(
                self.allocator,
                sample_rate,
                columns_per_second,
                column_samples,
                spectrum.Mode.transient,
            ),
        };
        self.algorithms[Mode.tonal.index()] = .{
            .tonal = try tonal.Algorithm.init(self.allocator, sample_rate, column_samples),
        };
        self.algorithms[Mode.reassigned.index()] = .{
            .reassigned = try spectral_mode.Algorithm.init(
                self.allocator,
                sample_rate,
                columns_per_second,
                column_samples,
                spectrum.Mode.reassigned,
            ),
        };
        self.algorithms[Mode.squeezed.index()] = .{
            .squeezed = try spectral_mode.Algorithm.init(
                self.allocator,
                sample_rate,
                columns_per_second,
                column_samples,
                spectrum.Mode.squeezed,
            ),
        };
        self.algorithms[Mode.superlet.index()] = .{
            .superlet = try spectral_mode.Algorithm.init(
                self.allocator,
                sample_rate,
                columns_per_second,
                column_samples,
                spectrum.Mode.superlet,
            ),
        };
        self.algorithms[Mode.multitaper.index()] = .{
            .multitaper = try spectral_mode.Algorithm.init(
                self.allocator,
                sample_rate,
                columns_per_second,
                column_samples,
                spectrum.Mode.multitaper,
            ),
        };
        self.algorithms[Mode.s_transform.index()] = .{
            .s_transform = try spectral_mode.Algorithm.init(
                self.allocator,
                sample_rate,
                columns_per_second,
                column_samples,
                spectrum.Mode.s_transform,
            ),
        };
        self.algorithms[Mode.sparse.index()] = .{
            .sparse = try spectral_mode.Algorithm.init(
                self.allocator,
                sample_rate,
                columns_per_second,
                column_samples,
                spectrum.Mode.sparse,
            ),
        };
    }

    fn applyFrequencyRange(self: *Engine, min_hz: f64, max_hz: f64) !void {
        for (&self.algorithms) |*slot| {
            if (slot.*) |*algorithm| try algorithm.setFrequencyRange(min_hz, max_hz);
        }

        self.min_hz = min_hz;
        self.max_hz = max_hz;
    }

    fn ensureColumnStorage(self: *Engine, row_count: usize, column_count: usize) !void {
        if (self.column_capacity_rows == row_count and
            self.column_capacity_columns == column_count)
        {
            return;
        }

        if (column_count == 0 or row_count > std.math.maxInt(usize) / column_count) {
            return Error.TooLarge;
        }

        self.allocator.free(self.columns);
        self.columns = try self.allocator.alloc(f32, row_count * column_count);
        self.column_capacity_rows = row_count;
        self.column_capacity_columns = column_count;
    }

    fn algorithmForMode(self: *Engine, mode: Mode) ?*Algorithm {
        if (self.algorithms[mode.index()]) |*algorithm| return algorithm;
        return null;
    }

    fn constAlgorithmForMode(self: *const Engine, mode: Mode) ?*const Algorithm {
        if (self.algorithms[mode.index()]) |*algorithm| return algorithm;
        return null;
    }
};

fn columnSamplesForRate(sample_rate: f64, columns_per_second: f64) u64 {
    const samples: u64 = @intFromFloat(@round(sample_rate / columns_per_second));
    return @max(samples, minimum_column_samples);
}

test "analysis engine registry updates a transient impulse frame" {
    const sample_rate = 48000.0;
    const columns_per_second = 240.0;
    const sample_count: usize = @intFromFloat(sample_rate * 3.0);
    const impulse_sample: usize = @intFromFloat(sample_rate);

    var samples = try std.testing.allocator.alloc(f32, sample_count);
    defer std.testing.allocator.free(samples);
    @memset(samples, 0.0);
    samples[impulse_sample] = 1.0;

    var ring = try ring_buffer.RingBuffer.init(std.testing.allocator, sample_count);
    defer ring.deinit();
    ring.write(samples);

    var engine = try Engine.init(std.testing.allocator, sample_rate, columns_per_second);
    defer engine.deinit();

    engine.setMode(.transient);
    const latency_samples = 8192;
    const column_samples = 200;
    const analysis_written = impulse_sample + latency_samples;
    engine.resetModeTimeline(.transient, analysis_written - column_samples);

    const frame = try engine.update(
        &ring,
        analysis_written,
        ring.capacity(),
        128,
        8,
    );

    try std.testing.expect(frame.column_count > 0);
    try std.testing.expectEqual(@as(usize, 128), frame.row_count);
    try std.testing.expectEqual(frame.column_count * frame.row_count, frame.columns.len);

    var maximum: f32 = -1.0e30;
    for (frame.columns) |cell| maximum = @max(maximum, cell);
    try std.testing.expect(maximum > -119.0);
}
