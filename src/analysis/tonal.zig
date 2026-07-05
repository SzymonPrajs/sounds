//! Tonal wavelet SST live mode.
//! Rewrite target; C reference: src_c/src/analysis/tonal.c

const std = @import("std");
const wavelet = @import("wavelet.zig");
const ring_buffer = @import("../audio/ring_buffer.zig");

pub const Error = error{
    OutputFull,
};

pub const Algorithm = struct {
    allocator: std.mem.Allocator,
    wavelet_analyzer: wavelet.Analyzer,
    samples: []f32,
    column_samples: u64,
    analyzed_samples: u64,

    pub fn init(
        allocator: std.mem.Allocator,
        sample_rate: f64,
        column_samples: u64,
    ) !Algorithm {
        const samples = try allocator.alloc(f32, @intCast(column_samples));
        errdefer allocator.free(samples);

        var analyzer = try wavelet.Analyzer.init(allocator, sample_rate);
        errdefer analyzer.deinit();

        return .{
            .allocator = allocator,
            .wavelet_analyzer = analyzer,
            .samples = samples,
            .column_samples = column_samples,
            .analyzed_samples = 0,
        };
    }

    pub fn deinit(self: *Algorithm) void {
        self.wavelet_analyzer.deinit();
        self.allocator.free(self.samples);
        self.* = undefined;
    }

    pub fn minFrequency(self: *const Algorithm) f64 {
        return self.wavelet_analyzer.minFrequency();
    }

    pub fn maxFrequency(self: *const Algorithm) f64 {
        return self.wavelet_analyzer.maxFrequency();
    }

    pub fn setFrequencyRange(self: *Algorithm, min_hz: f64, max_hz: f64) !void {
        try self.wavelet_analyzer.setFrequencyRange(min_hz, max_hz);
    }

    pub fn toggleSst(self: *Algorithm) void {
        self.wavelet_analyzer.setSynchrosqueezed(!self.wavelet_analyzer.isSynchrosqueezed());
    }

    pub fn sstEnabled(self: *const Algorithm) bool {
        return self.wavelet_analyzer.isSynchrosqueezed();
    }

    pub fn reset(self: *Algorithm, written_samples: u64) void {
        self.analyzed_samples = written_samples;
    }

    pub fn update(
        self: *Algorithm,
        ring: *const ring_buffer.RingBuffer,
        written_samples: u64,
        ring_capacity: u64,
        row_count: usize,
        column_limit: usize,
        columns: []f32,
        column_count: *usize,
    ) !void {
        if (row_count == 0 or column_limit == 0) return;

        // C could underflow here if a caller reset the timeline backwards.
        if (written_samples < self.analyzed_samples) self.analyzed_samples = written_samples;

        if (written_samples > self.analyzed_samples + ring_capacity) {
            self.analyzed_samples = written_samples - ring_capacity;
        }

        var pending = (written_samples - self.analyzed_samples) / self.column_samples;
        pending = @min(pending, @as(u64, @intCast(column_limit)));

        for (0..@intCast(pending)) |_| {
            const read_count = ring.readEndingAt(
                self.analyzed_samples + self.column_samples,
                self.samples,
            );

            if (read_count != self.samples.len) {
                self.analyzed_samples = written_samples;
                break;
            }

            try self.wavelet_analyzer.push(self.samples);
            self.analyzed_samples += read_count;

            const dbfs_rows = try appendColumn(columns, row_count, column_limit, column_count);
            try self.wavelet_analyzer.snapshotDb(dbfs_rows);
        }
    }
};

fn appendColumn(columns: []f32, row_count: usize, capacity: usize, column_count: *usize) ![]f32 {
    if (column_count.* >= capacity) return Error.OutputFull;

    const start = column_count.* * row_count;
    column_count.* += 1;
    return columns[start..][0..row_count];
}
