//! Sparse ridges, reassigned, squeezed, superlet, multitaper, S-transform modes.
//! Rewrite target; C reference: src_c/src/analysis/spectral_mode.c

const std = @import("std");
const spectrum = @import("spectrum.zig");
const ring_buffer = @import("../audio/ring_buffer.zig");

pub const Error = error{
    OutputFull,
};

pub const Algorithm = struct {
    spectrum_analyzer: spectrum.Analyzer,
    spectrum_mode: spectrum.Mode,
    column_samples: u64,
    last_center: u64,
    latency_samples: u64,

    pub fn init(
        allocator: std.mem.Allocator,
        sample_rate: f64,
        columns_per_second: f64,
        column_samples: u64,
        mode: spectrum.Mode,
    ) !Algorithm {
        var analyzer = try spectrum.Analyzer.init(allocator, sample_rate, columns_per_second);
        errdefer analyzer.deinit();

        return .{
            .spectrum_analyzer = analyzer,
            .spectrum_mode = mode,
            .column_samples = column_samples,
            .last_center = 0,
            .latency_samples = analyzer.latencySamples(),
        };
    }

    pub fn deinit(self: *Algorithm) void {
        self.spectrum_analyzer.deinit();
        self.* = undefined;
    }

    pub fn minFrequency(self: *const Algorithm) f64 {
        return self.spectrum_analyzer.minFrequency();
    }

    pub fn maxFrequency(self: *const Algorithm) f64 {
        return self.spectrum_analyzer.maxFrequency();
    }

    pub fn setFrequencyRange(self: *Algorithm, min_hz: f64, max_hz: f64) !void {
        try self.spectrum_analyzer.setFrequencyRange(min_hz, max_hz);
    }

    pub fn reset(self: *Algorithm, written_samples: u64) void {
        self.last_center = if (written_samples > self.latency_samples)
            written_samples - self.latency_samples
        else
            0;
    }

    pub fn update(
        self: *Algorithm,
        ring: *const ring_buffer.RingBuffer,
        written_samples: u64,
        row_count: usize,
        column_limit: usize,
        columns: []f32,
        column_count: *usize,
    ) !void {
        if (row_count == 0 or column_limit == 0) return;

        const latest_center = if (written_samples > self.latency_samples)
            written_samples - self.latency_samples
        else
            0;
        var first_center = self.last_center + self.column_samples;

        if (first_center < self.latency_samples) first_center = self.latency_samples;
        if (latest_center < first_center) return;

        var pending = 1 + (latest_center - first_center) / self.column_samples;
        pending = @min(pending, @as(u64, @intCast(column_limit)));

        for (0..@intCast(pending)) |column| {
            const center_sample = first_center + @as(u64, @intCast(column)) * self.column_samples;
            const dbfs_rows = try appendColumn(columns, row_count, column_limit, column_count);

            try self.spectrum_analyzer.columnDb(
                ring,
                center_sample,
                self.spectrum_mode,
                dbfs_rows,
            );
            self.last_center = center_sample;
        }
    }
};

fn appendColumn(columns: []f32, row_count: usize, capacity: usize, column_count: *usize) ![]f32 {
    if (column_count.* >= capacity) return Error.OutputFull;

    const start = column_count.* * row_count;
    column_count.* += 1;
    return columns[start..][0..row_count];
}
