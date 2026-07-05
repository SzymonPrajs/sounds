//! Whole-clip spectrum and spectrogram analysis.

const std = @import("std");
const vdsp = @import("../apple/vdsp.zig");

pub const Error = error{
    InvalidRequest,
    InvalidRange,
    SetupFailed,
    TooLarge,
};

const pi_value = std.math.pi;
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
        self.allocator.free(self.time);
        self.allocator.free(self.even);
        self.allocator.free(self.odd);
        self.allocator.free(self.real);
        self.allocator.free(self.imag);
        self.* = undefined;
    }

    fn forward(self: *RealDft) void {
        const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
        const Vec = @Vector(lanes, f32);
        var index: usize = 0;
        while (index + lanes <= self.half_length) : (index += lanes) {
            var even: Vec = undefined;
            var odd: Vec = undefined;
            inline for (0..lanes) |lane| {
                even[lane] = self.time[(index + lane) * 2];
                odd[lane] = self.time[(index + lane) * 2 + 1];
            }
            self.even[index..][0..lanes].* = even;
            self.odd[index..][0..lanes].* = odd;
        }
        while (index < self.half_length) : (index += 1) {
            self.even[index] = self.time[index * 2];
            self.odd[index] = self.time[index * 2 + 1];
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
    const log_min = @log2(min_hz);
    const log_max = @log2(max_hz);
    const log_hz = log_max + (log_min - log_max) * std.math.clamp(unit, 0.0, 1.0);

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

pub fn bandAmplitudeOverTime(
    allocator: std.mem.Allocator,
    samples: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz_arg: f64,
    amplitudes: []f32,
) !void {
    if (samples.len == 0 or
        amplitudes.len == 0 or
        !std.math.isFinite(sample_rate) or
        sample_rate <= 0.0 or
        sample_rate > maximum_offline_sample_rate or
        !std.math.isFinite(low_hz) or
        !std.math.isFinite(high_hz_arg) or
        low_hz <= 0.0 or
        high_hz_arg <= low_hz)
    {
        return Error.InvalidRequest;
    }

    const nyquist = sample_rate * 0.5;
    const high_hz = @min(high_hz_arg, nyquist);
    if (low_hz >= high_hz) return Error.InvalidRange;

    const fft_length = try spectrogramWindowLength(samples.len, amplitudes.len);
    var dft = try RealDft.init(allocator, fft_length);
    defer dft.deinit();

    const bin_range = bandBinRange(fft_length, dft.half_length, sample_rate, low_hz, high_hz);
    for (amplitudes, 0..) |*amplitude, column| {
        const center = spectrogramColumnCenter(samples.len, column, amplitudes.len);
        const window_sum = fillCenteredWindow(&dft, samples, center);
        dft.forward();
        amplitude.* = integratedBandAmplitude(&dft, bin_range.first, bin_range.last, window_sum);
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

fn bandBinRange(
    fft_length: usize,
    half_length: usize,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
) struct { first: usize, last: usize } {
    const length_f = @as(f64, @floatFromInt(fft_length));
    var first: usize = @intFromFloat(@floor(low_hz * length_f / sample_rate));
    var last: usize = @intFromFloat(@ceil(high_hz * length_f / sample_rate));

    if (first > half_length) first = half_length;
    if (last > half_length) last = half_length;
    if (last < first) last = first;
    return .{ .first = first, .last = last };
}

fn integratedBandAmplitude(
    dft: *const RealDft,
    first_bin: usize,
    last_bin: usize,
    window_sum: f64,
) f32 {
    var power: f64 = 0.0;
    var bin = first_bin;
    while (bin <= last_bin) : (bin += 1) {
        const scale: f64 = if (bin > 0 and bin < dft.half_length) 4.0 else 1.0;
        power += dft.binPower(bin) * scale;
        if (bin == dft.half_length) break;
    }

    return @floatCast(@sqrt(@max(power, 0.0)) / @max(window_sum, 1.0e-12));
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
    const log_min = @log2(min_hz);
    const log_max = @log2(max_hz);
    const log_hz = log_max + (log_min - log_max) * std.math.clamp(unit, 0.0, 1.0);

    return std.math.pow(f64, 2.0, log_hz);
}

fn spectrogramWindowLength(sample_count: usize, column_count: usize) !usize {
    if (sample_count > std.math.maxInt(usize) - column_count + 1) return Error.TooLarge;

    const samples_per_column = (sample_count + column_count - 1) / column_count;
    if (samples_per_column > std.math.maxInt(usize) / 4) return Error.TooLarge;

    const wanted = std.math.clamp(
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

test "band amplitude envelope follows in-band burst energy" {
    const sample_rate = 48000.0;
    const seconds = 2.0;
    const sample_count: usize = @intFromFloat(sample_rate * seconds);
    const bucket_count = 96;

    const input = try std.testing.allocator.alloc(f32, sample_count);
    defer std.testing.allocator.free(input);
    var low_band: [bucket_count]f32 = undefined;
    var high_band: [bucket_count]f32 = undefined;

    for (input, 0..) |*sample, i| {
        const t = @as(f64, @floatFromInt(i)) / sample_rate;
        var value = 0.35 * @sin(2.0 * pi_value * 500.0 * t);
        if (i >= sample_count / 3 and i < (sample_count * 2) / 3) {
            value += 0.45 * @sin(2.0 * pi_value * 4000.0 * t);
        }
        sample.* = @floatCast(value);
    }

    try bandAmplitudeOverTime(std.testing.allocator, input, sample_rate, 3800.0, 4200.0, &high_band);
    try bandAmplitudeOverTime(std.testing.allocator, input, sample_rate, 450.0, 550.0, &low_band);

    const Helpers = struct {
        fn mean(values: []const f32, first: usize, last: usize) f64 {
            var sum: f64 = 0.0;
            for (values[first..last]) |value| sum += value;
            return sum / @as(f64, @floatFromInt(last - first));
        }
    };

    const first_high = Helpers.mean(&high_band, 0, bucket_count / 3);
    const middle_high = Helpers.mean(&high_band, bucket_count / 3, (bucket_count * 2) / 3);
    const last_high = Helpers.mean(&high_band, (bucket_count * 2) / 3, bucket_count);
    try std.testing.expect(middle_high > first_high * 5.0);
    try std.testing.expect(middle_high > last_high * 5.0);

    const first_low = Helpers.mean(&low_band, 4, bucket_count / 3);
    const middle_low = Helpers.mean(&low_band, bucket_count / 3, (bucket_count * 2) / 3);
    const last_low = Helpers.mean(&low_band, (bucket_count * 2) / 3, bucket_count - 4);
    try std.testing.expectApproxEqRel(first_low, middle_low, 0.25);
    try std.testing.expectApproxEqRel(last_low, middle_low, 0.25);
}
