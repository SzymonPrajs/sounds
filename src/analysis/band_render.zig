//! Selected/rejected band audition rendering (FFT/FIR/IIR/STFT/... methods).
//! Rewrite target; C reference: src_c/src/analysis/band_render/

const std = @import("std");
const vdsp = @import("../apple/vdsp.zig");
const offline_spectrum = @import("offline_spectrum.zig");

pub const Error = error{
    InvalidRequest,
    InvalidRange,
    SetupFailed,
    TooLarge,
};

const pi_value = std.math.pi;
const maximum_sample_rate = 512000.0;
const render_edge_fade_seconds = 0.010;
const render_edge_fade_max_samples = 2048;
const render_peak_headroom = 1.10;
const render_peak_floor = 0.02;
const render_peak_ceiling = 0.98;

pub const Method = enum {
    fft_mask,
    fir_linear,
    iir_biquad,
    zero_phase_iir,
    stft_mask,
    griffin_lim,
    cqt_approx,
    auditory_approx,
    modal_approx,
    sparse_approx,
};

pub const Request = struct {
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    iterations: u32 = 0,
};

const methods = [_]Method{
    .fft_mask,
    .fir_linear,
    .iir_biquad,
    .zero_phase_iir,
    .stft_mask,
    .griffin_lim,
    .cqt_approx,
    .auditory_approx,
    .modal_approx,
    .sparse_approx,
};

const MaskShape = enum {
    full_band,
    modal_peaks,
    sparse_threshold,
};

const RealDft = struct {
    allocator: std.mem.Allocator,
    length: usize,
    half_length: usize,
    even: []f32,
    odd: []f32,
    real: []f32,
    imag: []f32,
    forward: vdsp.DFTSetup,
    inverse: vdsp.DFTSetup,

    fn init(allocator: std.mem.Allocator, length: usize) !RealDft {
        if (length < 2 or (length & 1) != 0) return Error.InvalidRequest;

        const half = length / 2;
        var dft = RealDft{
            .allocator = allocator,
            .length = length,
            .half_length = half,
            .even = &.{},
            .odd = &.{},
            .real = &.{},
            .imag = &.{},
            .forward = null,
            .inverse = null,
        };
        errdefer dft.deinit();

        dft.forward = vdsp.createDft(null, length, .forward) catch return Error.SetupFailed;
        dft.inverse = vdsp.createDft(null, length, .inverse) catch return Error.SetupFailed;
        dft.even = try allocator.alloc(f32, half);
        dft.odd = try allocator.alloc(f32, half);
        dft.real = try allocator.alloc(f32, half);
        dft.imag = try allocator.alloc(f32, half);

        return dft;
    }

    fn deinit(self: *RealDft) void {
        vdsp.destroyDft(self.forward);
        vdsp.destroyDft(self.inverse);
        if (self.even.len > 0) self.allocator.free(self.even);
        if (self.odd.len > 0) self.allocator.free(self.odd);
        if (self.real.len > 0) self.allocator.free(self.real);
        if (self.imag.len > 0) self.allocator.free(self.imag);
        self.* = undefined;
    }

    fn load(self: *RealDft, input: []const f32) void {
        @memset(self.even, 0.0);
        @memset(self.odd, 0.0);

        const count = @min(input.len, self.length);
        const pair_count = count / 2;
        const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
        const Vec = @Vector(lanes, f32);
        var pair: usize = 0;
        while (pair + lanes <= pair_count) : (pair += lanes) {
            var even: Vec = undefined;
            var odd: Vec = undefined;
            inline for (0..lanes) |lane| {
                even[lane] = input[(pair + lane) * 2];
                odd[lane] = input[(pair + lane) * 2 + 1];
            }
            self.even[pair..][0..lanes].* = even;
            self.odd[pair..][0..lanes].* = odd;
        }
        while (pair < pair_count) : (pair += 1) {
            self.even[pair] = input[pair * 2];
            self.odd[pair] = input[pair * 2 + 1];
        }
        if ((count & 1) != 0) {
            self.even[pair_count] = input[pair_count * 2];
        }
    }

    fn store(self: *const RealDft, output: []f32) void {
        const scale = 1.0 / @as(f64, @floatFromInt(self.length));
        const count = @min(output.len, self.length);

        for (output[0..count], 0..) |*sample, i| {
            const value = if ((i & 1) == 0) self.even[i / 2] else self.odd[i / 2];
            sample.* = @floatCast(@as(f64, value) * scale);
        }
    }

    fn executeForward(self: *RealDft) void {
        vdsp.executeDft(self.forward, self.even, self.odd, self.real, self.imag);
    }

    fn executeInverse(self: *RealDft) void {
        vdsp.executeDft(self.inverse, self.real, self.imag, self.even, self.odd);
    }

    fn binPower(self: *const RealDft, bin: usize) f64 {
        if (bin == 0) return @as(f64, self.real[0]) * @as(f64, self.real[0]);
        if (bin >= self.half_length) return @as(f64, self.imag[0]) * @as(f64, self.imag[0]);

        return @as(f64, self.real[bin]) * @as(f64, self.real[bin]) +
            @as(f64, self.imag[bin]) * @as(f64, self.imag[bin]);
    }
};

pub fn methodCount() usize {
    return methods.len;
}

pub fn methodAt(index: isize) Method {
    if (index < 0) return .fft_mask;

    const i: usize = @intCast(index);
    if (i >= methods.len) return .fft_mask;
    return methods[i];
}

pub fn methodIndex(method: Method) usize {
    for (methods, 0..) |candidate, i| {
        if (candidate == method) return i;
    }

    return 0;
}

pub fn methodOffset(method: Method, offset: isize) Method {
    const count: isize = @intCast(methods.len);
    var index: isize = @as(isize, @intCast(methodIndex(method))) + offset;

    while (index < 0) index += count;
    return methodAt(@intCast(@mod(index, count)));
}

pub fn methodName(method: Method) []const u8 {
    return switch (method) {
        .fft_mask => "FFT MASK IFFT",
        .fir_linear => "FIR LINEAR",
        .iir_biquad => "IIR BIQUAD",
        .zero_phase_iir => "IIR ZERO PHASE",
        .stft_mask => "STFT MASK ISTFT",
        .griffin_lim => "GRIFFIN LIM",
        .cqt_approx => "CQT APPROX",
        .auditory_approx => "ERB AUDITORY",
        .modal_approx => "MODAL",
        .sparse_approx => "SPARSE",
    };
}

pub fn methodShortName(method: Method) []const u8 {
    return methodName(method);
}

pub fn render(
    allocator: std.mem.Allocator,
    input: []const f32,
    request: Request,
    method: Method,
    output: []f32,
) !void {
    if (output.len < input.len) return Error.InvalidRequest;

    switch (method) {
        .fft_mask => try fftMask(allocator, input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
        .fir_linear => try firLinear(allocator, input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
        .iir_biquad => try iirBiquad(input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
        .zero_phase_iir => try zeroPhaseIir(allocator, input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
        .stft_mask => try stftMask(allocator, input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
        .griffin_lim => try griffinLim(allocator, input, request.sample_rate, request.low_hz, request.high_hz, request.iterations, output[0..input.len]),
        .cqt_approx => try cqtApprox(allocator, input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
        .auditory_approx => try auditoryApprox(input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
        .modal_approx => try modalApprox(allocator, input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
        .sparse_approx => try sparseApprox(allocator, input, request.sample_rate, request.low_hz, request.high_hz, output[0..input.len]),
    }
}

pub fn sanitizeOutput(output: []f32, sample_rate: f64, reference: ?[]const f32) void {
    if (output.len == 0) return;

    for (output) |*sample| {
        if (!std.math.isFinite(sample.*)) sample.* = 0.0;
    }

    const fade_count = renderFadeSampleCount(output.len, sample_rate);
    applyEdgeFade(output, fade_count);

    const peak = absolutePeak(output);
    const reference_peak = if (reference) |ref| absolutePeak(ref[0..@min(ref.len, output.len)]) else peak;
    const limit = renderPeakLimit(reference_peak);

    if (peak > limit and peak > 0.0) {
        const scale: f32 = @floatCast(limit / peak);
        scaleSamples(output, scale);
    }
}

pub fn fftMask(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    try fftMaskShape(allocator, input, sample_rate, low_hz, high_hz, .full_band, output);
}

pub fn firLinear(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    try validRequest(input, sample_rate, low_hz, high_hz, output);

    const taps = firTapCount(sample_rate, low_hz, high_hz);
    const kernel = try allocator.alloc(f32, taps);
    defer allocator.free(kernel);

    const center = taps / 2;
    const low = low_hz / sample_rate;
    const high = high_hz / sample_rate;

    for (kernel, 0..) |*tap, i| {
        const n = @as(f64, @floatFromInt(i)) - @as(f64, @floatFromInt(center));
        const ideal = 2.0 * high * sinc(2.0 * high * n) -
            2.0 * low * sinc(2.0 * low * n);
        const unit = @as(f64, @floatFromInt(i)) / @as(f64, @floatFromInt(taps - 1));
        const window = 0.5 - 0.5 * @cos(2.0 * pi_value * unit);

        tap.* = @floatCast(ideal * window);
    }

    for (output[0..input.len], 0..) |*sample, n| {
        sample.* = firSample(input, kernel, center, n);
    }
}

pub fn iirBiquad(
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    try validRequest(input, sample_rate, low_hz, high_hz, output);

    var biquad = Biquad.init(sample_rate, low_hz, high_hz);
    for (output[0..input.len], input) |*sample, source| {
        sample.* = biquad.process(source);
    }
}

pub fn zeroPhaseIir(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    try validRequest(input, sample_rate, low_hz, high_hz, output);

    const work = try allocator.alloc(f32, input.len);
    defer allocator.free(work);

    try iirBiquad(input, sample_rate, low_hz, high_hz, work);

    var biquad = Biquad.init(sample_rate, low_hz, high_hz);
    for (0..input.len) |i| {
        const reverse = input.len - 1 - i;
        output[reverse] = biquad.process(work[reverse]);
    }
}

pub fn stftMask(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    try validRequest(input, sample_rate, low_hz, high_hz, output);

    const frame_length = 1024;
    const hop = frame_length / 4;
    if (input.len < frame_length) return fftMask(allocator, input, sample_rate, low_hz, high_hz, output);

    var dft = try RealDft.init(allocator, frame_length);
    defer dft.deinit();

    const weight = try allocator.alloc(f32, input.len);
    defer allocator.free(weight);
    @memset(weight, 0.0);
    @memset(output[0..input.len], 0.0);

    var start: usize = 0;
    while (start < input.len) : (start += hop) {
        @memset(dft.even, 0.0);
        @memset(dft.odd, 0.0);

        for (0..frame_length) |i| {
            const source = start + i;
            const unit = @as(f64, @floatFromInt(i)) /
                @as(f64, @floatFromInt(frame_length - 1));
            const window: f32 = @floatCast(0.5 - 0.5 * @cos(2.0 * pi_value * unit));
            const value = if (source < input.len) input[source] * window else 0.0;

            if ((i & 1) == 0) {
                dft.even[i / 2] = value;
            } else {
                dft.odd[i / 2] = value;
            }
        }

        dft.executeForward();
        applyFrequencyMask(&dft, sample_rate, low_hz, high_hz, .full_band);
        dft.executeInverse();

        for (0..frame_length) |i| {
            const target = start + i;
            if (target >= input.len) break;

            const unit = @as(f64, @floatFromInt(i)) /
                @as(f64, @floatFromInt(frame_length - 1));
            const window: f32 = @floatCast(0.5 - 0.5 * @cos(2.0 * pi_value * unit));
            const value = (if ((i & 1) == 0) dft.even[i / 2] else dft.odd[i / 2]) /
                @as(f32, @floatFromInt(frame_length));

            output[target] += value * window;
            weight[target] += window * window;
        }

        if (start + frame_length >= input.len) break;
    }

    normalizeByWeight(output[0..input.len], weight);
}

pub fn griffinLim(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    iterations: u32,
    output: []f32,
) !void {
    try stftMask(allocator, input, sample_rate, low_hz, high_hz, output);

    const passes = if (iterations > 0) iterations else 2;
    const work = try allocator.alloc(f32, input.len);
    defer allocator.free(work);

    for (0..passes) |_| {
        @memcpy(work, output[0..input.len]);
        try stftMask(allocator, work, sample_rate, low_hz, high_hz, output);
    }
}

pub fn cqtApprox(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    const grow = std.math.pow(f64, 2.0, 1.0 / 24.0);
    try firLinear(allocator, input, sample_rate, low_hz / grow, high_hz * grow, output);
}

pub fn auditoryApprox(
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    const center = @sqrt(low_hz * high_hz);
    const width = erbWidth(center);
    const low = @max(1.0, low_hz - width);
    const high = @min(sample_rate * 0.5, high_hz + width);

    try iirBiquad(input, sample_rate, low, high, output);
}

pub fn modalApprox(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    try fftMaskShape(allocator, input, sample_rate, low_hz, high_hz, .modal_peaks, output);
}

pub fn sparseApprox(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    output: []f32,
) !void {
    try fftMaskShape(allocator, input, sample_rate, low_hz, high_hz, .sparse_threshold, output);
}

fn fftMaskShape(
    allocator: std.mem.Allocator,
    input: []const f32,
    sample_rate: f64,
    low_hz: f64,
    high_hz: f64,
    shape: MaskShape,
    output: []f32,
) !void {
    try validRequest(input, sample_rate, low_hz, high_hz, output);

    const fft_length = try nextPowerOfTwo(input.len);
    var dft = try RealDft.init(allocator, fft_length);
    defer dft.deinit();

    dft.load(input);
    dft.executeForward();
    applyFrequencyMask(&dft, sample_rate, low_hz, high_hz, shape);
    dft.executeInverse();
    dft.store(output[0..input.len]);
}

fn applyFrequencyMask(dft: *RealDft, sample_rate: f64, low_hz: f64, high_hz: f64, shape: MaskShape) void {
    const softness = @max(10.0, (high_hz - low_hz) * 0.04);
    var strongest: f64 = 0.0;

    if (shape == .sparse_threshold) {
        var bin: usize = 1;
        while (bin <= dft.half_length) : (bin += 1) {
            const hz = @as(f64, @floatFromInt(bin)) * sample_rate / @as(f64, @floatFromInt(dft.length));
            const mask = raisedBandMask(hz, low_hz, high_hz, softness);
            if (mask > 0.0) strongest = @max(strongest, dft.binPower(bin));
            if (bin == dft.half_length) break;
        }
    }

    var bin: usize = 0;
    while (bin <= dft.half_length) : (bin += 1) {
        const hz = @as(f64, @floatFromInt(bin)) * sample_rate / @as(f64, @floatFromInt(dft.length));
        var mask = raisedBandMask(hz, low_hz, high_hz, softness);

        if (shape == .modal_peaks and mask > 0.0 and bin > 1 and bin + 1 < dft.half_length) {
            const power = dft.binPower(bin);
            const peak = power >= dft.binPower(bin - 1) and power >= dft.binPower(bin + 1);
            mask = if (peak) mask else 0.0;
        } else if (shape == .sparse_threshold and mask > 0.0) {
            mask = if (dft.binPower(bin) >= strongest * 0.04) mask else 0.0;
        }

        if (bin == 0) {
            dft.real[0] = @floatCast(@as(f64, dft.real[0]) * mask);
        } else if (bin >= dft.half_length) {
            dft.imag[0] = @floatCast(@as(f64, dft.imag[0]) * mask);
        } else {
            dft.real[bin] = @floatCast(@as(f64, dft.real[bin]) * mask);
            dft.imag[bin] = @floatCast(@as(f64, dft.imag[bin]) * mask);
        }

        if (bin == dft.half_length) break;
    }
}

fn raisedBandMask(hz: f64, low_hz: f64, high_hz: f64, softness_hz: f64) f64 {
    if (hz <= 0.0 or hz < low_hz - softness_hz or hz > high_hz + softness_hz) return 0.0;
    if (hz >= low_hz and hz <= high_hz) return 1.0;
    if (softness_hz <= 0.0) return 0.0;

    if (hz < low_hz) {
        const unit = (hz - (low_hz - softness_hz)) / softness_hz;
        return 0.5 - 0.5 * @cos(pi_value * clamp(unit, 0.0, 1.0));
    }

    const unit = ((high_hz + softness_hz) - hz) / softness_hz;
    return 0.5 - 0.5 * @cos(pi_value * clamp(unit, 0.0, 1.0));
}

fn validRequest(input: []const f32, sample_rate: f64, low_hz_arg: f64, high_hz: f64, output: []f32) !void {
    if (input.len == 0 or
        output.len < input.len or
        !std.math.isFinite(sample_rate) or
        sample_rate <= 0.0 or
        sample_rate > maximum_sample_rate)
    {
        return Error.InvalidRequest;
    }

    const low_hz = @max(low_hz_arg, 0.0);
    const nyquist = sample_rate * 0.5;
    if (high_hz <= low_hz or high_hz > nyquist) return Error.InvalidRange;
}

fn sinc(value: f64) f64 {
    if (@abs(value) < 1.0e-12) return 1.0;
    return @sin(pi_value * value) / (pi_value * value);
}

fn firTapCount(sample_rate: f64, low_hz: f64, high_hz: f64) usize {
    const transition = @max(30.0, @min(low_hz, sample_rate * 0.5 - high_hz));
    var taps: usize = @intFromFloat(@ceil(sample_rate * 3.0 / transition));

    if (taps < 65) taps = 65;
    if (taps > 513) taps = 513;
    if ((taps & 1) == 0) taps += 1;
    return taps;
}

fn firSample(input: []const f32, kernel: []const f32, center: usize, sample: usize) f32 {
    const source_center = sample + center;
    const k_min = if (source_center >= input.len) source_center - input.len + 1 else 0;
    const k_max = if (source_center < kernel.len) source_center else kernel.len - 1;

    if (k_min > k_max) return 0.0;

    var value: f64 = 0.0;
    var k = k_min;
    var source_index = source_center - k_min;
    while (k <= k_max) : ({
        k += 1;
        source_index -= 1;
    }) {
        value += @as(f64, kernel[k]) * @as(f64, input[source_index]);
    }

    return @floatCast(value);
}

const Biquad = struct {
    b0: f64,
    b1: f64,
    b2: f64,
    a1: f64,
    a2: f64,
    x1: f64 = 0.0,
    x2: f64 = 0.0,
    y1: f64 = 0.0,
    y2: f64 = 0.0,

    fn init(sample_rate: f64, low_hz: f64, high_hz: f64) Biquad {
        const center = @sqrt(low_hz * high_hz);
        const width = @max(high_hz - low_hz, 1.0);
        const q = clamp(center / width, 0.05, 80.0);
        const w0 = 2.0 * pi_value * center / sample_rate;
        const alpha = @sin(w0) / (2.0 * q);
        const a0 = 1.0 + alpha;

        return .{
            .b0 = alpha / a0,
            .b1 = 0.0,
            .b2 = -alpha / a0,
            .a1 = -2.0 * @cos(w0) / a0,
            .a2 = (1.0 - alpha) / a0,
        };
    }

    fn process(self: *Biquad, input: f32) f32 {
        const output = self.b0 * input +
            self.b1 * self.x1 +
            self.b2 * self.x2 -
            self.a1 * self.y1 -
            self.a2 * self.y2;

        self.x2 = self.x1;
        self.x1 = input;
        self.y2 = self.y1;
        self.y1 = output;
        return @floatCast(output);
    }
};

fn erbWidth(hz: f64) f64 {
    return 24.7 * (4.37 * hz / 1000.0 + 1.0);
}

fn absolutePeak(samples: []const f32) f64 {
    var peak: f64 = 0.0;

    for (samples) |sample| {
        const value = @abs(@as(f64, sample));
        if (std.math.isFinite(value) and value > peak) peak = value;
    }

    return peak;
}

fn scaleSamples(samples: []f32, scale: f32) void {
    const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
    const Vec = @Vector(lanes, f32);
    const scale_vec: Vec = @splat(scale);
    var index: usize = 0;
    while (index + lanes <= samples.len) : (index += lanes) {
        const values: Vec = samples[index..][0..lanes].*;
        samples[index..][0..lanes].* = values * scale_vec;
    }
    while (index < samples.len) : (index += 1) samples[index] *= scale;
}

fn normalizeByWeight(samples: []f32, weights: []const f32) void {
    const count = @min(samples.len, weights.len);
    const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
    const Vec = @Vector(lanes, f32);
    const epsilon: Vec = @splat(1.0e-8);
    var index: usize = 0;
    while (index + lanes <= count) : (index += lanes) {
        const sample_vec: Vec = samples[index..][0..lanes].*;
        const weight_vec: Vec = weights[index..][0..lanes].*;
        samples[index..][0..lanes].* = @select(
            f32,
            weight_vec > epsilon,
            sample_vec / weight_vec,
            sample_vec,
        );
    }
    while (index < count) : (index += 1) {
        if (weights[index] > 1.0e-8) samples[index] /= weights[index];
    }
}

fn renderPeakLimit(reference_peak_arg: f64) f64 {
    var reference_peak = reference_peak_arg;
    if (!std.math.isFinite(reference_peak) or reference_peak < 0.0) reference_peak = 0.0;

    var limit = reference_peak * render_peak_headroom;
    if (limit < render_peak_floor) limit = render_peak_floor;
    if (limit > render_peak_ceiling) limit = render_peak_ceiling;
    return limit;
}

fn renderFadeSampleCount(sample_count: usize, sample_rate: f64) usize {
    if (sample_count < 2 or !std.math.isFinite(sample_rate) or sample_rate <= 0.0) return 0;

    var fade: usize = @intFromFloat(@ceil(sample_rate * render_edge_fade_seconds));
    if (fade > render_edge_fade_max_samples) fade = render_edge_fade_max_samples;
    if (fade > sample_count / 2) fade = sample_count / 2;
    return fade;
}

fn applyEdgeFade(samples: []f32, fade_count: usize) void {
    if (fade_count == 0) return;

    for (0..fade_count) |i| {
        const position = if (fade_count == 1)
            0.0
        else
            @as(f64, @floatFromInt(i)) / @as(f64, @floatFromInt(fade_count - 1));
        const gain: f32 = @floatCast(0.5 - 0.5 * @cos(pi_value * position));

        samples[i] *= gain;
        samples[samples.len - 1 - i] *= gain;
    }
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

fn toneProjection(samples: []const f32, sample_rate: f64, hz: f64) f64 {
    var sine: f64 = 0.0;
    var cosine: f64 = 0.0;

    for (samples, 0..) |sample, i| {
        const phase = 2.0 * pi_value * hz * @as(f64, @floatFromInt(i)) / sample_rate;
        sine += @as(f64, sample) * @sin(phase);
        cosine += @as(f64, sample) * @cos(phase);
    }

    return @sqrt(sine * sine + cosine * cosine) / @as(f64, @floatFromInt(samples.len));
}

test "band render methods, sanitization, and offline spectrum fixtures" {
    const sample_rate = 48000.0;
    const sample_count = 4096;
    var input: [sample_count]f32 = undefined;
    var selected: [sample_count]f32 = .{0.0} ** sample_count;
    var rows: [96]f32 = .{0.0} ** 96;
    var columns: [96 * 24]f32 = .{0.0} ** (96 * 24);

    for (&input, 0..) |*sample, i| {
        const t = @as(f64, @floatFromInt(i)) / sample_rate;
        sample.* = @floatCast(0.5 * @sin(2.0 * pi_value * 500.0 * t) +
            0.5 * @sin(2.0 * pi_value * 4000.0 * t));
    }

    const request = Request{
        .sample_rate = sample_rate,
        .low_hz = 400.0,
        .high_hz = 700.0,
        .iterations = 1,
    };

    try render(std.testing.allocator, &input, request, .fft_mask, &selected);

    const low = toneProjection(&selected, sample_rate, 500.0);
    const high = toneProjection(&selected, sample_rate, 4000.0);
    try std.testing.expect(low > high * 8.0);

    try std.testing.expectEqual(@as(usize, 10), methodCount());
    try std.testing.expectEqual(Method.fft_mask, methodAt(-1));
    try std.testing.expectEqual(Method.sparse_approx, methodOffset(.fft_mask, -1));
    try std.testing.expectEqualStrings("FFT MASK IFFT", methodName(.fft_mask));

    @memset(&selected, 0.0);
    selected[0] = 12.0;
    selected[1] = -6.0;
    selected[32] = std.math.nan(f32);
    selected[64] = 4.0;
    selected[sample_count - 1] = -12.0;

    sanitizeOutput(&selected, sample_rate, &input);
    try std.testing.expectEqual(@as(f32, 0.0), selected[0]);
    try std.testing.expectEqual(@as(f32, 0.0), selected[sample_count - 1]);
    try std.testing.expect(std.math.isFinite(selected[32]));
    try std.testing.expect(absolutePeak(&selected) <= 0.981);

    try offline_spectrum.spectrumDb(
        std.testing.allocator,
        &input,
        sample_rate,
        20.0,
        24000.0,
        &rows,
    );

    try offline_spectrum.spectrogramDb(
        std.testing.allocator,
        &input,
        sample_rate,
        20.0,
        24000.0,
        &columns,
        96,
        24,
    );

    var maximum: f32 = -1.0e30;
    for (columns) |cell| {
        try std.testing.expect(std.math.isFinite(cell));
        maximum = @max(maximum, cell);
    }
    try std.testing.expect(maximum > -119.0);
}
