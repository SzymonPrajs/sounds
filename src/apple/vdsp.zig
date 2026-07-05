//! Hand-written bindings for the slice of Accelerate/vDSP this app uses.
//!
//! The Accelerate umbrella header does not survive translate-c on current
//! SDKs, and importing thousands of translated declarations for six routines
//! would be noise anyway. Extend this file as new vDSP calls are needed;
//! signatures come from vecLib/vDSP.h.
//!
//! Naming: raw externs keep their C names; Zig-friendly wrappers below take
//! slices and hide strides that are always 1 in this codebase.

pub const Length = usize; // vDSP_Length
pub const Stride = isize; // vDSP_Stride

pub const DFTSetup = ?*opaque {}; // struct vDSP_DFT_SetupStruct *

pub const DFTDirection = enum(c_int) {
    forward = 1, // vDSP_DFT_FORWARD
    inverse = -1, // vDSP_DFT_INVERSE
};

/// Real-to-complex (forward) / complex-to-real (inverse) DFT setup.
/// `length` must be f * 2^n with f in {1, 3, 5, 15} and n >= 4.
pub extern "c" fn vDSP_DFT_zrop_CreateSetup(
    previous: DFTSetup,
    length: Length,
    direction: DFTDirection,
) DFTSetup;

pub extern "c" fn vDSP_DFT_DestroySetup(setup: DFTSetup) void;

/// Split-complex execute; for zrop setups the real input is packed
/// even/odd into `ir`/`ii` (length/2 each).
pub extern "c" fn vDSP_DFT_Execute(
    setup: DFTSetup,
    ir: [*]const f32,
    ii: [*]const f32,
    or_: [*]f32,
    oi: [*]f32,
) void;

pub extern "c" fn vDSP_dotpr(
    a: [*]const f32,
    ia: Stride,
    b: [*]const f32,
    ib: Stride,
    result: *f32,
    n: Length,
) void;

pub extern "c" fn vDSP_vmul(
    a: [*]const f32,
    ia: Stride,
    b: [*]const f32,
    ib: Stride,
    c: [*]f32,
    ic: Stride,
    n: Length,
) void;

/// NOTE the C convention: vDSP_vsub(B, 1, A, 1, C, 1, n) computes C = A - B.
pub extern "c" fn vDSP_vsub(
    b: [*]const f32,
    ib: Stride,
    a: [*]const f32,
    ia: Stride,
    c: [*]f32,
    ic: Stride,
    n: Length,
) void;

pub extern "c" fn vDSP_rmsqv(a: [*]const f32, ia: Stride, result: *f32, n: Length) void;

pub extern "c" fn vDSP_maxmgv(a: [*]const f32, ia: Stride, result: *f32, n: Length) void;

// --- Zig-friendly wrappers (unit stride, slice-based) ---

pub fn dot(a: []const f32, b: []const f32) f32 {
    std.debug.assert(a.len == b.len);
    var result: f32 = 0;
    vDSP_dotpr(a.ptr, 1, b.ptr, 1, &result, a.len);
    return result;
}

pub fn dotStrided(a: [*]const f32, ia: Stride, b: [*]const f32, ib: Stride, n: usize) f32 {
    if (n == 0) return 0;

    var result: f32 = 0;
    vDSP_dotpr(a, ia, b, ib, &result, n);
    return result;
}

pub fn mul(a: []const f32, b: []const f32, out: []f32) void {
    std.debug.assert(a.len == b.len and a.len == out.len);
    vDSP_vmul(a.ptr, 1, b.ptr, 1, out.ptr, 1, out.len);
}

pub fn mulStrided(a: [*]const f32, ia: Stride, b: [*]const f32, ib: Stride, out: []f32) void {
    if (out.len == 0) return;
    vDSP_vmul(a, ia, b, ib, out.ptr, 1, out.len);
}

/// out = a - b, elementwise.
pub fn sub(a: []const f32, b: []const f32, out: []f32) void {
    std.debug.assert(a.len == b.len and a.len == out.len);
    vDSP_vsub(b.ptr, 1, a.ptr, 1, out.ptr, 1, out.len);
}

pub fn rms(a: []const f32) f32 {
    var result: f32 = 0;
    vDSP_rmsqv(a.ptr, 1, &result, a.len);
    return result;
}

pub fn maxMagnitude(a: []const f32) f32 {
    var result: f32 = 0;
    vDSP_maxmgv(a.ptr, 1, &result, a.len);
    return result;
}

pub fn createDft(previous: DFTSetup, length: usize, direction: DFTDirection) !DFTSetup {
    return vDSP_DFT_zrop_CreateSetup(previous, length, direction) orelse error.SetupFailed;
}

pub fn destroyDft(setup: DFTSetup) void {
    if (setup) |_| vDSP_DFT_DestroySetup(setup);
}

pub fn executeDft(
    setup: DFTSetup,
    input_real: []const f32,
    input_imag: []const f32,
    output_real: []f32,
    output_imag: []f32,
) void {
    std.debug.assert(input_real.len == input_imag.len);
    std.debug.assert(input_real.len == output_real.len);
    std.debug.assert(input_real.len == output_imag.len);
    vDSP_DFT_Execute(setup, input_real.ptr, input_imag.ptr, output_real.ptr, output_imag.ptr);
}

const std = @import("std");

test "vDSP wrappers agree with scalar math" {
    const a = [_]f32{ 1, 2, 3, 4 };
    const b = [_]f32{ 4, 3, 2, 1 };

    try std.testing.expectEqual(@as(f32, 20), dot(&a, &b));

    var out: [4]f32 = undefined;
    sub(&a, &b, &out);
    try std.testing.expectEqualSlices(f32, &.{ -3, -1, 1, 3 }, &out);

    try std.testing.expectEqual(@as(f32, 4), maxMagnitude(&a));
}

test "strided vDSP wrappers match scalar math" {
    const samples = [_]f32{ 1, 10, 2, 20, 3, 30, 4, 40 };
    const window = [_]f32{ 2, 0, 3, 0, 4, 0, 5, 0 };

    var out: [4]f32 = undefined;
    mulStrided(&samples, 2, &window, 2, &out);
    try std.testing.expectEqualSlices(f32, &.{ 2, 6, 12, 20 }, &out);

    const dot_reverse = dotStrided(&samples, 2, samples[6..].ptr, -2, 4);
    try std.testing.expectEqual(@as(f32, 20), dot_reverse);
}

test "forward/inverse DFT round-trips a real signal" {
    const n = 16;
    const fwd = vDSP_DFT_zrop_CreateSetup(null, n, .forward) orelse return error.SetupFailed;
    defer vDSP_DFT_DestroySetup(fwd);
    const inv = vDSP_DFT_zrop_CreateSetup(fwd, n, .inverse) orelse return error.SetupFailed;
    defer vDSP_DFT_DestroySetup(inv);

    var signal: [n]f32 = undefined;
    for (&signal, 0..) |*s, i| {
        s.* = @sin(2.0 * std.math.pi * 3.0 * @as(f32, @floatFromInt(i)) / n);
    }

    // Pack even/odd, run forward + inverse, expect 2*n scaling.
    var ir: [n / 2]f32 = undefined;
    var ii: [n / 2]f32 = undefined;
    for (0..n / 2) |i| {
        ir[i] = signal[2 * i];
        ii[i] = signal[2 * i + 1];
    }
    var re: [n / 2]f32 = undefined;
    var im: [n / 2]f32 = undefined;
    vDSP_DFT_Execute(fwd, &ir, &ii, &re, &im);
    var back_re: [n / 2]f32 = undefined;
    var back_im: [n / 2]f32 = undefined;
    vDSP_DFT_Execute(inv, &re, &im, &back_re, &back_im);

    for (0..n / 2) |i| {
        try std.testing.expectApproxEqAbs(signal[2 * i] * 2 * n, back_re[i], 1e-3);
        try std.testing.expectApproxEqAbs(signal[2 * i + 1] * 2 * n, back_im[i], 1e-3);
    }
}
