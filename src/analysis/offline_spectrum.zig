//! Whole-clip frequency view.
//! Rewrite target; C reference: src_c/src/analysis/offline_spectrum.c

const std = @import("std");
const vdsp = @import("../apple/vdsp.zig");

pub const Error = error{
    InvalidRequest,
    InvalidRange,
    SetupFailed,
    TooLarge,
};

const pi_value = std.math.pi;
const log_two = 0.693147180559945309417232121458176568;
const display_db_floor = -140.0;
const minimum_amplitude = 1.0e-7;
const maximum_offline_sample_rate = 512000.0;
const spectrogram_min_window = 1024;
const spectrogram_max_window = 16384;

const RealDft = struct {
    allocator: std.mem.Allocator,
    length: usize,
    half_length: usize,
    time: []f32,
    even: []f32,
    odd: []f32,
    real: []f32,
    imag: []f32,
    setup: vdsp.DFTSetup,

    fn init(allocator: std.mem.Allocator, length: usize) !RealDft {
        if (length < 2 or (length & 1) != 0) return Error.InvalidRequest;

        const half = length / 2;
        var dft = RealDft{
            .allocator = allocator,
            .length = length,
            .half_length = half,
            .time = &.{},
            .even = &.{},
            .odd = &.{},
            .real = &.{},
            .imag = &.{},
            .setup = null,
        };
        errdefer dft.deinit();

        dft.setup = vdsp.createDft(null, length, .forward) catch return Error.SetupFailed;
        dft.time = try allocator.alloc(f32, length);
        dft.even = try allocator.alloc(f32, half);
        dft.odd = try allocator.alloc(f32, half);
        dft.real = try allocator.alloc(f32, half);
        dft.imag = try allocator.alloc(f32, half);
        @memset(dft.time, 0.0);

        return dft;
    }

    fn deinit(self: *RealDft) void {
        vdsp.destroyDft(self.setup);
        if (self.time.len > 0) self.allocator.free(self.time);
        if (self.even.len > 0) self.allocator.free(self.even);
        if (self.odd.len > 0) self.allocator.free(self.odd);
        if (self.real.len > 0) self.allocator.free(self.real);
        if (self.imag.len > 0) self.allocator.free(self.imag);
        self.* = undefined;
    }

    fn forward(self: *RealDft) void {
        for (0..self.half_length) |i| {
            self.even[i] = self.time[i * 2];
            self.odd[i] = self.time[i * 2 + 1];
        }

        vdsp.executeDft(self.setup, self.even, self.odd, self.real, self.imag);
    }

    fn binPower(self: *const RealDft, bin: usize) f64 {
        if (bin == 0) {
            return @as(f64, self.real[0]) * @as(f64, self.real[0]);
        }

        if (bin >= self.half_length) {
            return @as(f64, self.imag[0]) * @as(f64, self.imag[0]);
        }

        return @as(f64, self.real[bin]) * @as(f64, self.real[bin]) +
            @as(f64, self.imag[bin]) * @as(f64, self.imag[bin]);
    }
};

pub fn frequencyForRow(min_hz: f64, max_hz: f64, row: usize, row_count: usize) f64 {
    if (min_hz <= 0.0 or max_hz <= min_hz or row_count == 0) return 0.0;

    const unit = if (row_count == 1)
        0.5
    else
        (@as(f64, @floatFromInt(row)) + 0.5) / @as(f64, @floatFromInt(row_count));
    const log_min = log2Double(min_hz);
    const log_max = log2Double(max_hz);
    const log_hz = log_max + (log_min - log_max) * clamp(unit, 0.0, 1.0);

    return std.math.pow(f64, 2.0, log_hz);
}

pub fn spectrumDb(
    allocator: std.mem.Allocator,
    samples: []const f32,
    sample_rate: f64,
    min_hz: f64,
    max_hz_arg: f64,
    dbfs_rows: []f32,
) !void {
    if (samples.len == 0 or
        dbfs_rows.len == 0 or
        !std.math.isFinite(sample_rate) or
        sample_rate <= 0.0 or
        sample_rate > maximum_offline_sample_rate or
        min_hz <= 0.0 or
        max_hz_arg <= min_hz)
    {
        return Error.InvalidRequest;
    }

    const nyquist = sample_rate * 0.5;
    const max_hz = @min(max_hz_arg, nyquist);
    if (min_hz >= max_hz) return Error.InvalidRange;

    const fft_length = try nextPowerOfTwo(samples.len);
    var dft = try RealDft.init(allocator, fft_length);
    defer dft.deinit();

    @memset(dft.time, 0.0);
    var window_sum: f64 = 0.0;
    if (samples.len == 1) {
        dft.time[0] = samples[0];
        window_sum = 1.0;
    } else {
        for (samples, 0..) |sample, i| {
            const unit = @as(f64, @floatFromInt(i)) /
                @as(f64, @floatFromInt(samples.len - 1));
            const window = 0.5 - 0.5 * @cos(2.0 * pi_value * unit);

            dft.time[i] = @floatCast(@as(f64, sample) * window);
            window_sum += window;
        }
    }

    dft.forward();
    rowsFromDft(&dft, sample_rate, min_hz, max_hz, window_sum, dbfs_rows);
}

pub fn spectrogramDb(
    allocator: std.mem.Allocator,
    samples: []const f32,
    sample_rate: f64,
    min_hz: f64,
    max_hz_arg: f64,
    dbfs_columns: []f32,
    row_count: usize,
    column_count: usize,
) !void {
    if (samples.len == 0 or
        row_count == 0 or
        column_count == 0 or
        dbfs_columns.len < row_count * column_count or
        !std.math.isFinite(sample_rate) or
        sample_rate <= 0.0 or
        sample_rate > maximum_offline_sample_rate or
        min_hz <= 0.0 or
        max_hz_arg <= min_hz)
    {
        return Error.InvalidRequest;
    }

    if (column_count > std.math.maxInt(usize) / row_count) return Error.TooLarge;

    const nyquist = sample_rate * 0.5;
    const max_hz = @min(max_hz_arg, nyquist);
    if (min_hz >= max_hz) return Error.InvalidRange;

    const fft_length = try spectrogramWindowLength(samples.len, column_count);
    var dft = try RealDft.init(allocator, fft_length);
    defer dft.deinit();

    for (0..column_count) |column| {
        const center = spectrogramColumnCenter(samples.len, column, column_count);
        const window_sum = fillCenteredWindow(&dft, samples, center);
        dft.forward();
        rowsFromDft(
            &dft,
            sample_rate,
            min_hz,
            max_hz,
            window_sum,
            dbfs_columns[column * row_count ..][0..row_count],
        );
    }
}

fn rowsFromDft(
    dft: *const RealDft,
    sample_rate: f64,
    min_hz: f64,
    max_hz: f64,
    window_sum: f64,
    dbfs_rows: []f32,
) void {
    for (dbfs_rows, 0..) |*db, row| {
        const high_hz = rowEdgeFrequency(min_hz, max_hz, row, dbfs_rows.len);
        const low_hz = rowEdgeFrequency(min_hz, max_hz, row + 1, dbfs_rows.len);
        var first_bin: usize = @intFromFloat(@floor(low_hz * @as(f64, @floatFromInt(dft.length)) / sample_rate));
        var last_bin: usize = @intFromFloat(@ceil(high_hz * @as(f64, @floatFromInt(dft.length)) / sample_rate));

        if (first_bin > dft.half_length) first_bin = dft.half_length;
        if (last_bin > dft.half_length) last_bin = dft.half_length;
        if (last_bin < first_bin) last_bin = first_bin;

        var best_power: f64 = 0.0;
        var best_bin = first_bin;
        var bin = first_bin;
        while (bin <= last_bin) : (bin += 1) {
            const power = dft.binPower(bin);
            if (power > best_power) {
                best_power = power;
                best_bin = bin;
            }

            if (bin == dft.half_length) break;
        }

        db.* = powerToDb(best_power, window_sum, best_bin, dft.half_length);
    }
}

fn powerToDb(power: f64, window_sum: f64, bin: usize, half_length: usize) f32 {
    var amplitude = @sqrt(@max(power, 0.0)) / @max(window_sum, 1.0e-12);

    if (bin > 0 and bin < half_length) amplitude *= 2.0;

    return @floatCast(@max(
        display_db_floor,
        20.0 * @log10(@max(amplitude, minimum_amplitude)),
    ));
}

fn rowEdgeFrequency(min_hz: f64, max_hz: f64, edge: usize, row_count: usize) f64 {
    const unit = if (row_count == 0)
        0.0
    else
        @as(f64, @floatFromInt(edge)) / @as(f64, @floatFromInt(row_count));
    const log_min = log2Double(min_hz);
    const log_max = log2Double(max_hz);
    const log_hz = log_max + (log_min - log_max) * clamp(unit, 0.0, 1.0);

    return std.math.pow(f64, 2.0, log_hz);
}

fn spectrogramWindowLength(sample_count: usize, column_count: usize) !usize {
    if (sample_count > std.math.maxInt(usize) - column_count + 1) return Error.TooLarge;

    const samples_per_column = (sample_count + column_count - 1) / column_count;
    if (samples_per_column > std.math.maxInt(usize) / 4) return Error.TooLarge;

    const wanted = clampUsize(
        samples_per_column * 4,
        spectrogram_min_window,
        spectrogram_max_window,
    );
    var length = try nextPowerOfTwo(wanted);

    if (length > spectrogram_max_window) length = spectrogram_max_window;
    return length;
}

fn spectrogramColumnCenter(sample_count: usize, column: usize, column_count: usize) usize {
    const numerator =
        (@as(f128, @floatFromInt(column)) * 2.0 + 1.0) *
        @as(f128, @floatFromInt(sample_count));
    const denominator = 2.0 * @as(f128, @floatFromInt(column_count));
    const center = numerator / denominator;

    if (center >= @as(f128, @floatFromInt(sample_count))) {
        return if (sample_count > 0) sample_count - 1 else 0;
    }

    return @intFromFloat(center);
}

fn fillCenteredWindow(dft: *RealDft, samples: []const f32, center: usize) f64 {
    @memset(dft.time, 0.0);

    const half = dft.length / 2;
    var window_sum: f64 = 0.0;

    for (0..dft.length) |i| {
        const unit = if (dft.length == 1)
            0.0
        else
            @as(f64, @floatFromInt(i)) / @as(f64, @floatFromInt(dft.length - 1));
        const window = 0.5 - 0.5 * @cos(2.0 * pi_value * unit);
        var sample_index: usize = 0;
        var in_range = false;

        if (i >= half) {
            const offset = i - half;
            if (center <= std.math.maxInt(usize) - offset) {
                sample_index = center + offset;
                in_range = sample_index < samples.len;
            }
        } else {
            const offset = half - i;
            if (center >= offset) {
                sample_index = center - offset;
                in_range = true;
            }
        }

        if (in_range) {
            dft.time[i] = @floatCast(@as(f64, samples[sample_index]) * window);
        }

        window_sum += window;
    }

    return window_sum;
}

fn nextPowerOfTwo(value: usize) !usize {
    var power: usize = 2;

    while (power < value) {
        if (power > std.math.maxInt(usize) / 2) return Error.TooLarge;
        power *= 2;
    }

    return power;
}

fn clamp(value: f64, minimum: f64, maximum: f64) f64 {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

fn clampUsize(value: usize, minimum: usize, maximum: usize) usize {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

fn log2Double(value: f64) f64 {
    return @log(value) / log_two;
}

test "offline spectrum maps rows and produces finite tone energy" {
    const sample_rate = 48000.0;
    const sample_count = 4096;
    var input: [sample_count]f32 = undefined;
    var rows: [96]f32 = undefined;

    for (&input, 0..) |*sample, i| {
        const t = @as(f64, @floatFromInt(i)) / sample_rate;
        sample.* = @floatCast(@sin(2.0 * pi_value * 500.0 * t));
    }

    try spectrumDb(std.testing.allocator, &input, sample_rate, 20.0, 24000.0, &rows);

    var maximum: f32 = -1.0e30;
    for (rows) |row| {
        try std.testing.expect(std.math.isFinite(row));
        maximum = @max(maximum, row);
    }

    try std.testing.expect(maximum > -20.0);
    try std.testing.expect(frequencyForRow(20.0, 24000.0, 0, 96) > frequencyForRow(20.0, 24000.0, 95, 96));
}
