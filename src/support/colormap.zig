//! Perceptual color maps for spectrogram rendering.

const std = @import("std");

pub const Color = struct {
    red: f32,
    green: f32,
    blue: f32,
};

pub const Colormap = enum {
    viridis,
    magma,
    inferno,
    plasma,
    cividis,
    turbo,

    pub fn name(self: Colormap) []const u8 {
        return names[@intFromEnum(self)];
    }

    pub fn index(self: Colormap) usize {
        return @intFromEnum(self);
    }

    pub fn sample(self: Colormap, unit: f32) Color {
        return sampleStops(&stop_tables[@intFromEnum(self)], unit);
    }
};

pub const order = [_]Colormap{
    .viridis,
    .magma,
    .inferno,
    .plasma,
    .cividis,
    .turbo,
};

pub const count = order.len;

pub fn at(index: isize) Colormap {
    if (index < 0) return .viridis;

    const i: usize = @intCast(index);
    if (i >= order.len) return .viridis;
    return order[i];
}

pub fn fromInt(value: i32) ?Colormap {
    return switch (value) {
        0 => .viridis,
        1 => .magma,
        2 => .inferno,
        3 => .plasma,
        4 => .cividis,
        5 => .turbo,
        else => null,
    };
}

const names = [_][]const u8{
    "viridis",
    "magma",
    "inferno",
    "plasma",
    "cividis",
    "turbo",
};

const stop_count = 11;

inline fn rgb(red: f32, green: f32, blue: f32) Color {
    return .{ .red = red, .green = green, .blue = blue };
}

const viridis_stops = [stop_count]Color{
    rgb(0.267004, 0.004874, 0.329415),
    rgb(0.282623, 0.140926, 0.457517),
    rgb(0.253935, 0.265254, 0.529983),
    rgb(0.206756, 0.371758, 0.553117),
    rgb(0.163625, 0.471133, 0.558148),
    rgb(0.127568, 0.566949, 0.550556),
    rgb(0.134692, 0.658636, 0.517649),
    rgb(0.266941, 0.748751, 0.440573),
    rgb(0.477504, 0.821444, 0.318195),
    rgb(0.741388, 0.873449, 0.149561),
    rgb(0.993248, 0.906157, 0.143936),
};

const magma_stops = [stop_count]Color{
    rgb(0.001462, 0.000466, 0.013866),
    rgb(0.078815, 0.054184, 0.211667),
    rgb(0.232077, 0.059889, 0.437695),
    rgb(0.390384, 0.100379, 0.501864),
    rgb(0.550287, 0.161158, 0.505719),
    rgb(0.716387, 0.214982, 0.475290),
    rgb(0.868793, 0.287728, 0.409303),
    rgb(0.967671, 0.439703, 0.359810),
    rgb(0.994738, 0.624350, 0.427397),
    rgb(0.995680, 0.812706, 0.572645),
    rgb(0.987053, 0.991438, 0.749504),
};

const inferno_stops = [stop_count]Color{
    rgb(0.001462, 0.000466, 0.013866),
    rgb(0.087411, 0.044556, 0.224813),
    rgb(0.258234, 0.038571, 0.406485),
    rgb(0.416331, 0.090203, 0.432943),
    rgb(0.578304, 0.148039, 0.404411),
    rgb(0.735683, 0.215906, 0.330245),
    rgb(0.865006, 0.316822, 0.226055),
    rgb(0.954506, 0.468744, 0.099874),
    rgb(0.987622, 0.645320, 0.039886),
    rgb(0.964394, 0.843848, 0.273391),
    rgb(0.988362, 0.998364, 0.644924),
};

const plasma_stops = [stop_count]Color{
    rgb(0.050383, 0.029803, 0.527975),
    rgb(0.254627, 0.013882, 0.615419),
    rgb(0.417642, 0.000564, 0.658390),
    rgb(0.562738, 0.051545, 0.641509),
    rgb(0.692840, 0.165141, 0.564522),
    rgb(0.798216, 0.280197, 0.469538),
    rgb(0.881443, 0.392529, 0.383229),
    rgb(0.949217, 0.517763, 0.295662),
    rgb(0.988260, 0.652325, 0.211364),
    rgb(0.988648, 0.809579, 0.145357),
    rgb(0.940015, 0.975158, 0.131326),
};

const cividis_stops = [stop_count]Color{
    rgb(0.000000, 0.135112, 0.304751),
    rgb(0.032110, 0.201199, 0.440785),
    rgb(0.208926, 0.272546, 0.424809),
    rgb(0.309601, 0.340399, 0.424790),
    rgb(0.401418, 0.411790, 0.440708),
    rgb(0.488697, 0.485318, 0.471008),
    rgb(0.582087, 0.558670, 0.468118),
    rgb(0.683950, 0.638793, 0.444251),
    rgb(0.785965, 0.720438, 0.399613),
    rgb(0.896818, 0.811030, 0.320982),
    rgb(0.995737, 0.909344, 0.217772),
};

const turbo_stops = [stop_count]Color{
    rgb(0.189950, 0.071760, 0.232170),
    rgb(0.269670, 0.348780, 0.796310),
    rgb(0.244270, 0.609370, 0.996970),
    rgb(0.097500, 0.837140, 0.803420),
    rgb(0.275970, 0.970920, 0.516530),
    rgb(0.643620, 0.989990, 0.233560),
    rgb(0.883310, 0.865530, 0.217190),
    rgb(0.996070, 0.642770, 0.191650),
    rgb(0.940840, 0.355660, 0.070310),
    rgb(0.764760, 0.143740, 0.010410),
    rgb(0.479600, 0.015830, 0.010550),
};

const stop_tables = [_][stop_count]Color{
    viridis_stops,
    magma_stops,
    inferno_stops,
    plasma_stops,
    cividis_stops,
    turbo_stops,
};

fn clampUnit(value: f32) f32 {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

fn mixChannel(a: f32, b: f32, amount: f32) f32 {
    return a + (b - a) * amount;
}

fn sampleStops(stops: []const Color, unit: f32) Color {
    const t = clampUnit(unit);
    const scaled = t * @as(f32, @floatFromInt(stops.len - 1));
    const index: usize = @intFromFloat(scaled);

    if (index >= stops.len - 1) return stops[stops.len - 1];

    const low = stops[index];
    const high = stops[index + 1];
    const amount = scaled - @as(f32, @floatFromInt(index));

    return .{
        .red = mixChannel(low.red, high.red, amount),
        .green = mixChannel(low.green, high.green, amount),
        .blue = mixChannel(low.blue, high.blue, amount),
    };
}

fn expectColorApprox(expected: Color, actual: Color) !void {
    try std.testing.expectApproxEqAbs(expected.red, actual.red, 1.0e-6);
    try std.testing.expectApproxEqAbs(expected.green, actual.green, 1.0e-6);
    try std.testing.expectApproxEqAbs(expected.blue, actual.blue, 1.0e-6);
}

test "colormap order, names, and index fallbacks are stable" {
    try std.testing.expectEqual(@as(usize, 6), count);
    try std.testing.expectEqual(Colormap.viridis, at(-1));
    try std.testing.expectEqual(Colormap.viridis, at(6));

    for (order, 0..) |map, i| {
        try std.testing.expectEqual(i, map.index());
        try std.testing.expectEqual(map, at(@intCast(i)));
    }

    try std.testing.expectEqualStrings("viridis", Colormap.viridis.name());
    try std.testing.expectEqualStrings("magma", Colormap.magma.name());
    try std.testing.expectEqualStrings("inferno", Colormap.inferno.name());
    try std.testing.expectEqualStrings("plasma", Colormap.plasma.name());
    try std.testing.expectEqualStrings("cividis", Colormap.cividis.name());
    try std.testing.expectEqualStrings("turbo", Colormap.turbo.name());
    try std.testing.expectEqual(@as(?Colormap, null), fromInt(-1));
    try std.testing.expectEqual(@as(?Colormap, Colormap.turbo), fromInt(5));
}

test "colormap samples clamp and interpolate between color stops" {
    try expectColorApprox(rgb(0.267004, 0.004874, 0.329415), Colormap.viridis.sample(-10.0));
    try expectColorApprox(rgb(0.993248, 0.906157, 0.143936), Colormap.viridis.sample(10.0));
    try expectColorApprox(rgb(0.987053, 0.991438, 0.749504), Colormap.magma.sample(1.0));

    try expectColorApprox(
        rgb(0.490190, 0.0260545, 0.6499495),
        Colormap.plasma.sample(0.25),
    );
    try expectColorApprox(
        rgb(0.763465, 0.927760, 0.225375),
        Colormap.turbo.sample(0.55),
    );
}
