//! Analytic Morlet constant-Q pyramid with synchrosqueezing.

const std = @import("std");
const vdsp = @import("../apple/vdsp.zig");
const frequency_band = @import("../support/frequency_band.zig");

pub const Error = error{
    InvalidRequest,
    InvalidRange,
    NoVoices,
    TooLarge,
};

pub const min_hz = frequency_band.full_min_hz;
pub const max_hz = frequency_band.full_max_hz;
pub const voices_per_octave = 48;
pub const morlet_omega0 = 8.0;

const decimator_tap_count = 255;
const level_hop_divisor = 4;
const level_minimum_hop = 2;

const pi_value = std.math.pi;
const maximum_wavelet_sample_rate = 512000.0;
const decimator_cutoff_cycles = 0.24;
const morlet_support_sigmas = 8.0;

const power_smoothing_floor_seconds = 0.020;
const power_smoothing_scales = 6.0;
const frequency_smoothing_floor_seconds = 0.025;
const frequency_smoothing_scales = 4.0;

const raw_ridge_width_octaves = 0.55 / @as(f64, @floatFromInt(voices_per_octave));
const squeezed_width_gain = 0.8;
const squeezed_width_min_octaves = 0.018;
const squeezed_width_max_octaves = 0.22;
const minimum_deposit_sigma_rows = 0.9;
const display_db_floor = -120.0;
const if_clamp_octaves = 0.75;
const if_power_gate = 1.0e-13;
const initial_if_jitter = 0.25;
const initial_power_jitter = 0.5;
const am_incoherence_gain = 8.0;
const bandwidth_incoherence_gain = 4.0;
const bandwidth_ratio_clamp = 3.0;
const raw_deposit_normalization = 0.5;
const squeezed_deposit_normalization = 0.05;

const ComplexValue = struct {
    real: f64,
    imag: f64,
};

const Decimator = struct {
    taps: [decimator_tap_count]f32 = .{0.0} ** decimator_tap_count,
    history: [decimator_tap_count]f32 = .{0.0} ** decimator_tap_count,
    samples_seen: u64 = 0,
    samples_ready: u64 = 0,
    cursor: usize = 0,

    fn init(taps: *const [decimator_tap_count]f32) Decimator {
        return .{ .taps = taps.* };
    }

    fn push(self: *Decimator, sample: f32) ?f32 {
        self.history[self.cursor] = sample;
        self.cursor = (self.cursor + 1) % decimator_tap_count;
        self.samples_seen += 1;

        if (self.samples_ready < decimator_tap_count) self.samples_ready += 1;
        if (self.samples_ready < decimator_tap_count or (self.samples_seen & 1) != 0) return null;

        const latest = (self.cursor + decimator_tap_count - 1) % decimator_tap_count;
        const first_count = latest + 1;
        const second_count = decimator_tap_count - first_count;

        const first = vdsp.dotStrided(
            self.taps[0..].ptr,
            1,
            self.history[latest..].ptr,
            -1,
            first_count,
        );
        const second = if (second_count > 0)
            vdsp.dotStrided(
                self.taps[first_count..].ptr,
                1,
                self.history[decimator_tap_count - 1 ..].ptr,
                -1,
                second_count,
            )
        else
            0.0;

        return first + second;
    }
};

const Voice = struct {
    center_hz: f64 = 0.0,
    center_log2: f64 = 0.0,
    scale_samples: f64 = 0.0,
    half_support: usize = 0,
    kernel_size: usize = 0,
    block_offset: usize = 0,
    kernel_real: []f32 = &.{},
    kernel_imag: []f32 = &.{},
    derivative_real: []f32 = &.{},
    derivative_imag: []f32 = &.{},
    gain: f64 = 0.0,
    power_alpha: f64 = 0.0,
    frequency_alpha: f64 = 0.0,
    ema_power: f64 = 0.0,
    ema_if_log2: f64 = 0.0,
    if_jitter: f64 = 0.0,
    last_if_log2: f64 = 0.0,
    power_jitter: f64 = 0.0,
    previous_power: f64 = 0.0,
    bandwidth_jitter: f64 = 0.0,
    has_output: bool = false,
    active: bool = false,

    fn deinit(self: *Voice, allocator: std.mem.Allocator) void {
        allocator.free(self.kernel_real);
        allocator.free(self.kernel_imag);
        allocator.free(self.derivative_real);
        allocator.free(self.derivative_imag);
        self.* = .{};
    }
};

const Level = struct {
    sample_rate: f64 = 0.0,
    octave_low_hz: f64 = 0.0,
    octave_high_hz: f64 = 0.0,
    voices: []Voice = &.{},
    max_half_support: usize = 0,
    block_size: usize = 0,
    hop: usize = 0,
    samples_since_eval: usize = 0,
    history: []f32 = &.{},
    block: []f32 = &.{},
    history_capacity: usize = 0,
    history_written: u64 = 0,
    history_cursor: usize = 0,
    decimator: Decimator = .{},

    fn init(
        allocator: std.mem.Allocator,
        sample_rate: f64,
        octave_low_hz: f64,
        octave_high_hz: f64,
        range_min_hz: f64,
        range_max_hz: f64,
        decimator_taps: *const [decimator_tap_count]f32,
    ) !Level {
        var level = Level{
            .sample_rate = sample_rate,
            .octave_low_hz = octave_low_hz,
            .octave_high_hz = octave_high_hz,
            .decimator = Decimator.init(decimator_taps),
        };
        errdefer level.deinit(allocator);

        const voice_count = countLevelVoices(octave_low_hz, octave_high_hz, range_min_hz, range_max_hz);
        if (voice_count == 0) {
            level.history_capacity = 1;
            level.history = try allocator.alloc(f32, level.history_capacity);
            @memset(level.history, 0.0);
            return level;
        }

        level.voices = try allocator.alloc(Voice, voice_count);
        for (level.voices) |*voice| voice.* = .{};

        var written_voice: usize = 0;
        var max_half: usize = 0;
        for (0..voices_per_octave) |i| {
            const unit = (@as(f64, @floatFromInt(i)) + 0.5) /
                @as(f64, @floatFromInt(voices_per_octave));
            const center = octave_low_hz * std.math.pow(f64, 2.0, unit);

            if (center < range_min_hz or center > range_max_hz or center >= octave_high_hz) continue;

            const scale_samples = morlet_omega0 * sample_rate / (2.0 * pi_value * center);
            var half: usize = @intFromFloat(@ceil(morlet_support_sigmas * scale_samples));
            if (half < 4) half = 4;

            level.voices[written_voice] = .{
                .center_hz = center,
                .center_log2 = @log2(center),
                .scale_samples = scale_samples,
                .half_support = half,
                .active = true,
            };

            max_half = @max(max_half, half);
            written_voice += 1;
        }

        level.max_half_support = max_half;
        level.block_size = max_half * 2 + 1;
        level.history_capacity = level.block_size;
        level.hop = max_half / level_hop_divisor;
        if (level.hop < level_minimum_hop) level.hop = level_minimum_hop;

        level.history = try allocator.alloc(f32, level.history_capacity);
        level.block = try allocator.alloc(f32, level.block_size);
        @memset(level.history, 0.0);
        @memset(level.block, 0.0);

        const hop_seconds = @as(f64, @floatFromInt(level.hop)) / sample_rate;
        for (level.voices) |*voice| {
            const scale_seconds = voice.scale_samples / sample_rate;

            voice.block_offset = max_half - voice.half_support;
            voice.power_alpha = smoothingAlpha(
                hop_seconds,
                power_smoothing_floor_seconds,
                power_smoothing_scales * scale_seconds,
            );
            voice.frequency_alpha = smoothingAlpha(
                hop_seconds,
                frequency_smoothing_floor_seconds,
                frequency_smoothing_scales * scale_seconds,
            );

            try precomputeVoiceKernel(allocator, level.sample_rate, voice);
        }

        return level;
    }

    fn deinit(self: *Level, allocator: std.mem.Allocator) void {
        for (self.voices) |*voice| voice.deinit(allocator);
        allocator.free(self.voices);
        allocator.free(self.history);
        allocator.free(self.block);
        self.* = .{};
    }

    fn write(self: *Level, sample: f32) void {
        if (self.history_capacity == 0) return;

        self.history[self.history_cursor] = sample;
        self.history_cursor = (self.history_cursor + 1) % self.history_capacity;
        self.history_written += 1;
    }

    fn readLatest(self: *const Level, samples: []f32) bool {
        if (samples.len == 0 or self.history_written < samples.len) return false;

        const start = (self.history_cursor + self.history_capacity - samples.len) %
            self.history_capacity;
        for (samples, 0..) |*sample, i| {
            sample.* = self.history[(start + i) % self.history_capacity];
        }

        return true;
    }

    fn evaluate(self: *Level) void {
        if (!self.readLatest(self.block[0..self.block_size])) return;

        for (self.voices) |*voice| {
            if (!voice.active) continue;

            const segment = self.block[voice.block_offset..][0..voice.kernel_size];
            const coefficient = ComplexValue{
                .real = vdsp.dot(segment, voice.kernel_real),
                .imag = vdsp.dot(segment, voice.kernel_imag),
            };
            const derivative = ComplexValue{
                .real = vdsp.dot(segment, voice.derivative_real),
                .imag = vdsp.dot(segment, voice.derivative_imag),
            };

            const power =
                (coefficient.real * coefficient.real + coefficient.imag * coefficient.imag) /
                (voice.gain * voice.gain);

            if (!voice.has_output) {
                voice.ema_power = power;
                voice.ema_if_log2 = voice.center_log2;
                voice.last_if_log2 = voice.center_log2;
                voice.if_jitter = initial_if_jitter;
                voice.power_jitter = initial_power_jitter;
                voice.previous_power = power;
                voice.bandwidth_jitter = initial_power_jitter;
                voice.has_output = true;
            } else {
                voice.ema_power += voice.power_alpha * (power - voice.ema_power);
            }

            if (power <= if_power_gate) continue;

            const modulation = @abs(power - voice.previous_power) /
                (power + voice.previous_power + 1.0e-15);
            voice.power_jitter += voice.frequency_alpha * (modulation - voice.power_jitter);
            voice.previous_power = power;

            const magnitude_squared =
                coefficient.real * coefficient.real + coefficient.imag * coefficient.imag;
            const instantaneous_bandwidth_hz = @abs(
                (derivative.real * coefficient.real + derivative.imag * coefficient.imag) /
                    (magnitude_squared * 2.0 * pi_value),
            );
            const voice_bandwidth_hz = voice.center_hz / morlet_omega0;
            const bandwidth_ratio = std.math.clamp(
                instantaneous_bandwidth_hz / voice_bandwidth_hz,
                0.0,
                bandwidth_ratio_clamp,
            );

            voice.bandwidth_jitter +=
                voice.frequency_alpha * (bandwidth_ratio - voice.bandwidth_jitter);

            var frequency = instantaneousFrequencyHz(coefficient, derivative);
            if (frequency <= 0.0) frequency = voice.center_hz;

            const if_log2 = std.math.clamp(
                @log2(frequency),
                voice.center_log2 - if_clamp_octaves,
                voice.center_log2 + if_clamp_octaves,
            );
            const delta = @abs(if_log2 - voice.last_if_log2);

            voice.if_jitter += voice.frequency_alpha * (delta - voice.if_jitter);
            voice.ema_if_log2 += voice.frequency_alpha * (if_log2 - voice.ema_if_log2);
            voice.last_if_log2 = if_log2;
        }
    }

    fn feed(self: *Level, sample: f32) void {
        self.write(sample);
        if (self.voices.len == 0) return;

        self.samples_since_eval += 1;
        if (self.history_written >= self.history_capacity and self.samples_since_eval >= self.hop) {
            self.samples_since_eval = 0;
            self.evaluate();
        }
    }
};

pub const Analyzer = struct {
    allocator: std.mem.Allocator,
    sample_rate: f64,
    full_min_hz: f64,
    full_max_hz: f64,
    min_hz: f64,
    max_hz: f64,
    levels: []Level,
    total_voice_count: usize,
    synchrosqueezed: bool,
    power_rows: []f32,
    row_capacity: usize,

    pub fn init(allocator: std.mem.Allocator, sample_rate: f64) !Analyzer {
        if (!std.math.isFinite(sample_rate) or
            sample_rate <= 0.0 or
            sample_rate > maximum_wavelet_sample_rate)
        {
            return Error.InvalidRequest;
        }

        const full_min = min_hz;
        const full_max = @min(max_hz, sample_rate * 0.5);
        const voice_max = @min(full_max, sample_rate * 0.48);

        if (voice_max <= full_min * 2.0) return Error.NoVoices;

        const octave_span = @log2(full_max / full_min);
        const octave_count: usize = @intFromFloat(@ceil(octave_span));
        if (octave_count == 0) return Error.NoVoices;

        var created = Analyzer{
            .allocator = allocator,
            .sample_rate = sample_rate,
            .full_min_hz = full_min,
            .full_max_hz = full_max,
            .min_hz = full_min,
            .max_hz = full_max,
            .levels = try allocator.alloc(Level, octave_count),
            .total_voice_count = 0,
            .synchrosqueezed = true,
            .power_rows = &.{},
            .row_capacity = 0,
        };
        for (created.levels) |*level| level.* = .{};
        errdefer created.deinit();

        var decimator_taps: [decimator_tap_count]f32 = undefined;
        designDecimatorTaps(&decimator_taps);

        for (created.levels, 0..) |*level, octave| {
            const octave_high = full_max / std.math.pow(f64, 2.0, @floatFromInt(octave));
            const octave_low = octave_high * 0.5;
            const level_rate = sample_rate / std.math.pow(f64, 2.0, @floatFromInt(octave));

            level.* = try Level.init(
                allocator,
                level_rate,
                octave_low,
                octave_high,
                full_min,
                voice_max,
                &decimator_taps,
            );
            created.total_voice_count += level.voices.len;
        }

        if (created.total_voice_count == 0) return Error.NoVoices;

        created.updateActiveVoices();
        return created;
    }

    pub fn deinit(self: *Analyzer) void {
        for (self.levels) |*level| level.deinit(self.allocator);
        self.allocator.free(self.levels);
        self.allocator.free(self.power_rows);
        self.* = undefined;
    }

    pub fn minFrequency(self: *const Analyzer) f64 {
        return self.min_hz;
    }

    pub fn maxFrequency(self: *const Analyzer) f64 {
        return self.max_hz;
    }

    pub fn octaveCount(self: *const Analyzer) usize {
        return self.levels.len;
    }

    pub fn voiceCount(self: *const Analyzer) usize {
        return self.total_voice_count;
    }

    pub fn setFrequencyRange(self: *Analyzer, min_hz_arg: f64, max_hz_arg: f64) !void {
        var low = min_hz_arg;
        var high = max_hz_arg;

        if (low < self.full_min_hz) low = self.full_min_hz;
        if (high > self.full_max_hz) high = self.full_max_hz;
        if (low <= 0.0 or high <= low) return Error.InvalidRange;

        self.min_hz = low;
        self.max_hz = high;
        self.updateActiveVoices();
    }

    pub fn setSynchrosqueezed(self: *Analyzer, enabled: bool) void {
        self.synchrosqueezed = enabled;
    }

    pub fn isSynchrosqueezed(self: *const Analyzer) bool {
        return self.synchrosqueezed;
    }

    pub fn push(self: *Analyzer, samples: []const f32) !void {
        for (samples) |sample| self.feedLevelSample(0, sample);
    }

    pub fn snapshotDb(self: *Analyzer, dbfs_rows: []f32) !void {
        if (dbfs_rows.len == 0) return Error.InvalidRequest;
        try self.ensureRowBuffers(dbfs_rows.len);
        @memset(self.power_rows[0..dbfs_rows.len], 0.0);

        for (self.levels) |*level| {
            for (level.voices) |*voice| {
                if (!voice.active or !voice.has_output or voice.ema_power <= 0.0) continue;

                var frequency: f64 = undefined;
                var width_octaves: f64 = undefined;
                var power = voice.ema_power;

                if (self.synchrosqueezed) {
                    frequency = std.math.pow(f64, 2.0, voice.ema_if_log2);
                    width_octaves = std.math.clamp(
                        squeezed_width_gain * voice.if_jitter,
                        squeezed_width_min_octaves,
                        squeezed_width_max_octaves,
                    );
                    power /= 1.0 +
                        am_incoherence_gain * voice.power_jitter * voice.power_jitter +
                        bandwidth_incoherence_gain *
                            voice.bandwidth_jitter * voice.bandwidth_jitter;
                } else {
                    frequency = voice.center_hz;
                    width_octaves = raw_ridge_width_octaves;
                }

                depositGaussian(
                    self.power_rows[0..dbfs_rows.len],
                    self.min_hz,
                    self.max_hz,
                    frequency,
                    power,
                    width_octaves,
                );
            }
        }

        const normalization: f64 = if (self.synchrosqueezed)
            squeezed_deposit_normalization
        else
            raw_deposit_normalization;

        normalizedPowerRowsToDb(dbfs_rows, self.power_rows[0..dbfs_rows.len], normalization);
    }

    pub fn frequencyForRow(self: *const Analyzer, row: usize, row_count: usize) f64 {
        if (row_count == 0) return 0.0;

        const unit = if (row_count == 1)
            0.5
        else
            (@as(f64, @floatFromInt(row)) + 0.5) / @as(f64, @floatFromInt(row_count));
        const log_min = @log2(self.min_hz);
        const log_max = @log2(self.max_hz);
        const log_hz = log_max + (log_min - log_max) * std.math.clamp(unit, 0.0, 1.0);

        return std.math.pow(f64, 2.0, log_hz);
    }

    fn updateActiveVoices(self: *Analyzer) void {
        for (self.levels) |*level| {
            for (level.voices) |*voice| {
                const active = waveletVoiceInRange(voice, self.min_hz, self.max_hz);
                if (voice.active != active) {
                    voice.has_output = false;
                    voice.active = active;
                }
            }
        }
    }

    fn ensureRowBuffers(self: *Analyzer, row_count: usize) !void {
        if (row_count <= self.row_capacity) return;
        self.allocator.free(self.power_rows);
        self.power_rows = try self.allocator.alloc(f32, row_count);
        self.row_capacity = row_count;
    }

    fn feedLevelSample(self: *Analyzer, level_index: usize, sample: f32) void {
        const level = &self.levels[level_index];
        level.feed(sample);

        if (level_index + 1 >= self.levels.len) return;

        if (level.decimator.push(sample)) |decimated| {
            self.feedLevelSample(level_index + 1, decimated);
        }
    }
};

fn designDecimatorTaps(taps: *[decimator_tap_count]f32) void {
    var sum: f64 = 0.0;
    const middle = @as(f64, @floatFromInt(decimator_tap_count - 1)) * 0.5;

    for (taps, 0..) |*tap, i| {
        const offset = @as(f64, @floatFromInt(i)) - middle;
        const sinc = if (@abs(offset) < 1.0e-12)
            2.0 * decimator_cutoff_cycles
        else blk: {
            const angle = 2.0 * pi_value * decimator_cutoff_cycles * offset;
            break :blk @sin(angle) / (pi_value * offset);
        };

        const unit = @as(f64, @floatFromInt(i)) /
            @as(f64, @floatFromInt(decimator_tap_count - 1));
        const window = 0.42 - 0.50 * @cos(2.0 * pi_value * unit) +
            0.08 * @cos(4.0 * pi_value * unit);
        const value = sinc * window;

        tap.* = @floatCast(value);
        sum += value;
    }

    if (sum == 0.0) return;
    for (taps) |*tap| tap.* = @floatCast(@as(f64, tap.*) / sum);
}

fn precomputeVoiceKernel(allocator: std.mem.Allocator, level_sample_rate: f64, voice: *Voice) !void {
    const kernel_size = voice.half_support * 2 + 1;
    voice.kernel_size = kernel_size;
    voice.kernel_real = try allocator.alloc(f32, kernel_size);
    voice.kernel_imag = try allocator.alloc(f32, kernel_size);
    voice.derivative_real = try allocator.alloc(f32, kernel_size);
    voice.derivative_imag = try allocator.alloc(f32, kernel_size);

    const morlet_norm = std.math.pow(f64, pi_value, -0.25) / @sqrt(voice.scale_samples);
    const inverse_scale_seconds = level_sample_rate / voice.scale_samples;
    const theta = 2.0 * pi_value * voice.center_hz / level_sample_rate;
    var positive_real: f64 = 0.0;
    var positive_imag: f64 = 0.0;
    var negative_real: f64 = 0.0;
    var negative_imag: f64 = 0.0;

    for (0..kernel_size) |i| {
        const u = (@as(f64, @floatFromInt(voice.half_support)) -
            @as(f64, @floatFromInt(i))) / voice.scale_samples;
        const envelope = morlet_norm * @exp(-0.5 * u * u);
        const phase = -morlet_omega0 * u;
        const real = envelope * @cos(phase);
        const imag = envelope * @sin(phase);
        const derivative_real = inverse_scale_seconds *
            (u * real - morlet_omega0 * imag);
        const derivative_imag = inverse_scale_seconds *
            (u * imag + morlet_omega0 * real);

        voice.kernel_real[i] = @floatCast(real);
        voice.kernel_imag[i] = @floatCast(imag);
        voice.derivative_real[i] = @floatCast(derivative_real);
        voice.derivative_imag[i] = @floatCast(derivative_imag);

        const m = @as(f64, @floatFromInt(i)) - @as(f64, @floatFromInt(voice.half_support));
        const c = @cos(theta * m);
        const s = @sin(theta * m);

        positive_real += real * c - imag * s;
        positive_imag += real * s + imag * c;
        negative_real += real * c + imag * s;
        negative_imag += imag * c - real * s;
    }

    const positive = @sqrt(positive_real * positive_real + positive_imag * positive_imag);
    const negative = @sqrt(negative_real * negative_real + negative_imag * negative_imag);

    voice.gain = 0.5 * @max(positive, negative);
    if (voice.gain < 1.0e-9) voice.gain = 1.0e-9;
}

fn countLevelVoices(octave_low_hz: f64, octave_high_hz: f64, range_min_hz: f64, range_max_hz: f64) usize {
    var count: usize = 0;

    for (0..voices_per_octave) |i| {
        const unit = (@as(f64, @floatFromInt(i)) + 0.5) /
            @as(f64, @floatFromInt(voices_per_octave));
        const center = octave_low_hz * std.math.pow(f64, 2.0, unit);

        if (center >= range_min_hz and center <= range_max_hz and center < octave_high_hz) {
            count += 1;
        }
    }

    return count;
}

fn waveletVoiceInRange(voice: *const Voice, range_min_hz: f64, range_max_hz: f64) bool {
    const margin = std.math.pow(f64, 2.0, 1.5 / @as(f64, @floatFromInt(voices_per_octave)));

    return voice.center_hz >= range_min_hz / margin and
        voice.center_hz <= range_max_hz * margin;
}

fn smoothingAlpha(hop_seconds: f64, floor_seconds: f64, timescale_seconds: f64) f64 {
    const time_constant = @max(floor_seconds, timescale_seconds);
    const alpha = 1.0 - @exp(-hop_seconds / time_constant);

    return std.math.clamp(alpha, 1.0e-4, 1.0);
}

fn instantaneousFrequencyHz(coefficient: ComplexValue, derivative: ComplexValue) f64 {
    const magnitude_squared =
        coefficient.real * coefficient.real + coefficient.imag * coefficient.imag;

    if (magnitude_squared <= 1.0e-24) return 0.0;

    const imaginary_ratio =
        (derivative.imag * coefficient.real - derivative.real * coefficient.imag) /
        magnitude_squared;

    return imaginary_ratio / (2.0 * pi_value);
}

fn depositGaussian(
    rows: []f32,
    range_min_hz: f64,
    range_max_hz: f64,
    frequency: f64,
    power: f64,
    width_octaves: f64,
) void {
    if (rows.len == 0 or power <= 0.0) return;

    const log_min = @log2(range_min_hz);
    const log_max = @log2(range_max_hz);
    const log_range = log_max - log_min;

    if (log_range <= 0.0) return;
    if (frequency < range_min_hz or frequency > range_max_hz) return;

    const position, const octaves_per_row = if (rows.len == 1)
        .{ @as(f64, 0.0), log_range }
    else
        .{
            (log_max - @log2(frequency)) / log_range *
                @as(f64, @floatFromInt(rows.len - 1)),
            log_range / @as(f64, @floatFromInt(rows.len - 1)),
        };

    const sigma_rows = @max(minimum_deposit_sigma_rows, width_octaves / octaves_per_row);
    var first: isize = @intFromFloat(@floor(position - 3.0 * sigma_rows));
    var last: isize = @intFromFloat(@ceil(position + 3.0 * sigma_rows));

    if (first < 0) first = 0;
    if (last >= @as(isize, @intCast(rows.len))) last = @as(isize, @intCast(rows.len)) - 1;

    var row = first;
    while (row <= last) : (row += 1) {
        const distance = (@as(f64, @floatFromInt(row)) - position) / sigma_rows;
        rows[@intCast(row)] += @floatCast(power * @exp(-0.5 * distance * distance));
    }
}

fn normalizedPowerRowsToDb(dbfs_rows: []f32, power_rows: []const f32, normalization: f64) void {
    const count = @min(dbfs_rows.len, power_rows.len);
    const lanes = comptime (std.simd.suggestVectorLength(f64) orelse 2);
    const Vec = @Vector(lanes, f64);
    const OutVec = @Vector(lanes, f32);
    const normalize: Vec = @splat(normalization);
    const minimum: Vec = @splat(1.0e-12);
    const floor: Vec = @splat(display_db_floor);
    const scale: Vec = @splat(10.0);
    var index: usize = 0;
    while (index + lanes <= count) : (index += lanes) {
        var power: Vec = undefined;
        inline for (0..lanes) |lane| {
            power[lane] = @as(f64, power_rows[index + lane]);
        }
        const value = @max(scale * @log10(@max(power * normalize, minimum)), floor);
        const out: OutVec = @floatCast(value);
        dbfs_rows[index..][0..lanes].* = out;
    }
    while (index < count) : (index += 1) {
        const power = @as(f64, power_rows[index]) * normalization;
        var value = 10.0 * @log10(@max(power, 1.0e-12));
        if (value < display_db_floor) value = display_db_floor;
        dbfs_rows[index] = @floatCast(value);
    }
}

const test_row_count = 1024;
const test_chunk_capacity = 1024;

const Peak = struct {
    row: usize = 0,
    hz: f64 = 0.0,
    db: f32 = -1.0e30,
};

fn snapshotRows(analyzer: *Analyzer, synchrosqueezed: bool, rows: []f32) !void {
    analyzer.setSynchrosqueezed(synchrosqueezed);
    try analyzer.snapshotDb(rows);
}

fn findPeak(analyzer: *const Analyzer, rows: []const f32, low_hz: f64, high_hz: f64) Peak {
    var peak = Peak{};

    for (rows, 0..) |db, row| {
        const hz = analyzer.frequencyForRow(row, rows.len);
        if (hz < low_hz or hz > high_hz) continue;

        if (db > peak.db) {
            peak = .{
                .row = row,
                .hz = hz,
                .db = db,
            };
        }
    }

    return peak;
}

fn peakWidthOctaves(analyzer: *const Analyzer, rows: []const f32, peak: Peak, drop_db: f32) f64 {
    const threshold = peak.db - drop_db;
    var first = peak.row;
    var last = peak.row;

    while (first > 0 and rows[first - 1] >= threshold) first -= 1;
    while (last + 1 < rows.len and rows[last + 1] >= threshold) last += 1;

    const high = analyzer.frequencyForRow(first, rows.len);
    const low = analyzer.frequencyForRow(last, rows.len);

    return @abs(@log2(high / low));
}

fn pushTone(analyzer: *Analyzer, sample_rate: f64, hz: f64, seconds: f64, amplitude: f64) !void {
    var chunk: [test_chunk_capacity]f32 = undefined;
    const total: usize = @intFromFloat(@round(seconds * sample_rate));
    var written: usize = 0;
    var phase: f64 = 0.0;
    const phase_step = 2.0 * pi_value * hz / sample_rate;

    while (written < total) {
        const count = @min(total - written, test_chunk_capacity);
        for (chunk[0..count]) |*sample| {
            sample.* = @floatCast(amplitude * @sin(phase));
            phase += phase_step;
            if (phase > 2.0 * pi_value) phase -= 2.0 * pi_value;
        }

        try analyzer.push(chunk[0..count]);
        written += count;
    }
}

fn pushTwoTone(analyzer: *Analyzer, sample_rate: f64, first_hz: f64, second_hz: f64, seconds: f64) !void {
    var chunk: [test_chunk_capacity]f32 = undefined;
    const total: usize = @intFromFloat(@round(seconds * sample_rate));
    var written: usize = 0;
    var first_phase: f64 = 0.0;
    var second_phase: f64 = 0.0;
    const first_step = 2.0 * pi_value * first_hz / sample_rate;
    const second_step = 2.0 * pi_value * second_hz / sample_rate;

    while (written < total) {
        const count = @min(total - written, test_chunk_capacity);
        for (chunk[0..count]) |*sample| {
            sample.* = @floatCast(0.35 * @sin(first_phase) + 0.35 * @sin(second_phase));
            first_phase += first_step;
            second_phase += second_step;

            if (first_phase > 2.0 * pi_value) first_phase -= 2.0 * pi_value;
            if (second_phase > 2.0 * pi_value) second_phase -= 2.0 * pi_value;
        }

        try analyzer.push(chunk[0..count]);
        written += count;
    }
}

fn checkPeak(expected_hz: f64, tolerance_fraction: f64, peak: Peak) !void {
    const error_fraction = @abs(peak.hz - expected_hz) / expected_hz;

    try std.testing.expect(error_fraction <= tolerance_fraction);
    try std.testing.expect(peak.db >= -100.0);
}

fn runSingleTone(
    sample_rate: f64,
    hz: f64,
    seconds: f64,
    tolerance_fraction: f64,
    search_low: f64,
    search_high: f64,
) !void {
    var analyzer = try Analyzer.init(std.testing.allocator, sample_rate);
    defer analyzer.deinit();

    const rows = try std.testing.allocator.alloc(f32, test_row_count);
    defer std.testing.allocator.free(rows);

    try pushTone(&analyzer, sample_rate, hz, seconds, 0.65);
    try snapshotRows(&analyzer, true, rows);

    const peak = findPeak(&analyzer, rows, search_low, search_high);
    try checkPeak(hz, tolerance_fraction, peak);
}

test "wavelet tracks single tones across low and high ranges" {
    try runSingleTone(12000.0, 10.0, 16.0, 0.08, 8.0, 13.0);
    try runSingleTone(12000.0, 30.0, 8.0, 0.05, 25.0, 36.0);
    try runSingleTone(48000.0, 10000.0, 2.0, 0.04, 8000.0, 12000.0);
}

test "wavelet separates two close low-frequency tones" {
    var analyzer = try Analyzer.init(std.testing.allocator, 12000.0);
    defer analyzer.deinit();

    const rows = try std.testing.allocator.alloc(f32, test_row_count);
    defer std.testing.allocator.free(rows);

    try pushTwoTone(&analyzer, 12000.0, 30.0, 39.0, 12.0);
    try snapshotRows(&analyzer, true, rows);

    const one = findPeak(&analyzer, rows, 27.0, 33.3);
    const two = findPeak(&analyzer, rows, 35.4, 43.2);
    const valley = findPeak(&analyzer, rows, 33.3, 35.4);

    try checkPeak(30.0, 0.06, one);
    try checkPeak(39.0, 0.06, two);
    try std.testing.expect(valley.db <= @min(one.db, two.db) - 3.0);
}

test "wavelet raw mode does not alias a 20 kHz tone into the low band" {
    var analyzer = try Analyzer.init(std.testing.allocator, 48000.0);
    defer analyzer.deinit();

    const rows = try std.testing.allocator.alloc(f32, test_row_count);
    defer std.testing.allocator.free(rows);

    try pushTone(&analyzer, 48000.0, 20000.0, 2.0, 0.65);
    try snapshotRows(&analyzer, false, rows);

    const high = findPeak(&analyzer, rows, 15000.0, 20000.0);
    const low = findPeak(&analyzer, rows, 20.0, 200.0);

    try std.testing.expect(high.db >= -100.0);
    try std.testing.expect(low.db <= high.db - 25.0);
}

test "wavelet synchrosqueezing sharpens a rising chirp" {
    var analyzer = try Analyzer.init(std.testing.allocator, 48000.0);
    defer analyzer.deinit();

    const raw_rows = try std.testing.allocator.alloc(f32, test_row_count);
    defer std.testing.allocator.free(raw_rows);
    const squeezed_rows = try std.testing.allocator.alloc(f32, test_row_count);
    defer std.testing.allocator.free(squeezed_rows);

    var chunk: [test_chunk_capacity]f32 = undefined;
    const sample_rate = 48000.0;
    const seconds = 8.0;
    const start_hz = 250.0;
    const end_hz = 6000.0;
    const total: usize = @intFromFloat(@round(seconds * sample_rate));
    const snapshot_interval: usize = @intFromFloat(@round(sample_rate * 0.25));

    var written: usize = 0;
    var next_snapshot: usize = @intFromFloat(@round(sample_rate * 1.0));
    var phase: f64 = 0.0;
    var previous_peak_hz: f64 = 0.0;
    var raw_width_sum: f64 = 0.0;
    var squeezed_width_sum: f64 = 0.0;
    var snapshots: usize = 0;
    var monotonic_misses: usize = 0;

    while (written < total) {
        const count = @min(total - written, test_chunk_capacity);

        for (chunk[0..count], 0..) |*sample, i| {
            const t = @as(f64, @floatFromInt(written + i)) / sample_rate;
            const unit = t / seconds;
            const hz = start_hz + (end_hz - start_hz) * unit;

            sample.* = @floatCast(0.65 * @sin(phase));
            phase += 2.0 * pi_value * hz / sample_rate;
            if (phase > 2.0 * pi_value) phase -= 2.0 * pi_value;
        }

        try analyzer.push(chunk[0..count]);
        written += count;

        while (written >= next_snapshot and next_snapshot < total) {
            try snapshotRows(&analyzer, false, raw_rows);
            try snapshotRows(&analyzer, true, squeezed_rows);

            const raw_peak = findPeak(&analyzer, raw_rows, 150.0, 7000.0);
            const squeezed_peak = findPeak(&analyzer, squeezed_rows, 150.0, 7000.0);
            const raw_width = peakWidthOctaves(&analyzer, raw_rows, raw_peak, 6.0);
            const squeezed_width = peakWidthOctaves(&analyzer, squeezed_rows, squeezed_peak, 6.0);

            raw_width_sum += raw_width;
            squeezed_width_sum += squeezed_width;

            if (previous_peak_hz > 0.0 and squeezed_peak.hz < previous_peak_hz * 0.92) {
                monotonic_misses += 1;
            }

            previous_peak_hz = squeezed_peak.hz;
            snapshots += 1;
            next_snapshot += snapshot_interval;
        }
    }

    const raw_average = raw_width_sum / @as(f64, @floatFromInt(snapshots));
    const squeezed_average = squeezed_width_sum / @as(f64, @floatFromInt(snapshots));

    try std.testing.expect(snapshots >= 8);
    try std.testing.expect(monotonic_misses <= 2);
    try std.testing.expect(previous_peak_hz >= 3500.0);
    try std.testing.expect(squeezed_average < raw_average * 0.85);
}

test "wavelet focused display ranges keep active tone rows" {
    var analyzer = try Analyzer.init(std.testing.allocator, 48000.0);
    defer analyzer.deinit();

    const rows = try std.testing.allocator.alloc(f32, test_row_count);
    defer std.testing.allocator.free(rows);

    try analyzer.setFrequencyRange(120.0, 1000.0);
    try pushTone(&analyzer, 48000.0, 440.0, 4.0, 0.65);
    try snapshotRows(&analyzer, true, rows);

    var peak = findPeak(&analyzer, rows, 380.0, 520.0);
    try checkPeak(440.0, 0.04, peak);

    try analyzer.setFrequencyRange(250.0, 750.0);
    try pushTone(&analyzer, 48000.0, 440.0, 1.0, 0.65);
    try snapshotRows(&analyzer, true, rows);

    peak = findPeak(&analyzer, rows, 380.0, 520.0);
    try checkPeak(440.0, 0.04, peak);
}
