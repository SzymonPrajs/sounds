//! Local settings persistence for `sounds.settings`.

const std = @import("std");
const analysis = @import("../analysis/engine.zig");
const colormap = @import("../support/colormap.zig");
const frequency_band = @import("../support/frequency_band.zig");

pub const path = "sounds.settings";

pub const Error = error{
    InvalidSettings,
};

const magic = "SNDSET1\x00";
const version: u32 = 1;
const file_size = 56;

pub const Settings = struct {
    mode: analysis.Mode = .tonal,
    colormap: colormap.Colormap = .viridis,
    frequency_band: frequency_band.FrequencyBand = .whole,
    custom_range: frequency_band.Range = frequency_band.default_custom,

    pub fn sanitize(self: Settings) Settings {
        var safe = self;
        if (!customRangeFiniteAndOrdered(safe.custom_range)) {
            safe.custom_range = frequency_band.default_custom;
        }
        return safe;
    }
};

pub fn defaults() Settings {
    return .{};
}

pub fn load() !Settings {
    return loadFromDir(defaultIo(), .cwd(), path);
}

pub fn save(settings: Settings) !void {
    try saveToDir(defaultIo(), .cwd(), path, settings);
}

pub fn loadFromDir(io: std.Io, dir: std.Io.Dir, sub_path: []const u8) !Settings {
    var storage: [file_size + 1]u8 = undefined;
    const bytes = dir.readFile(io, sub_path, &storage) catch |err| switch (err) {
        error.FileNotFound => {
            const initial = defaults();
            try saveToDir(io, dir, sub_path, initial);
            return initial;
        },
        else => return err,
    };

    if (bytes.len != file_size) {
        const initial = defaults();
        try saveToDir(io, dir, sub_path, initial);
        return initial;
    }

    return decode(bytes) catch {
        const initial = defaults();
        try saveToDir(io, dir, sub_path, initial);
        return initial;
    };
}

pub fn saveToDir(io: std.Io, dir: std.Io.Dir, sub_path: []const u8, settings: Settings) !void {
    var bytes: [file_size]u8 = undefined;
    encode(settings, &bytes);
    try dir.writeFile(io, .{ .sub_path = sub_path, .data = &bytes });
}

fn decode(bytes: []const u8) !Settings {
    if (bytes.len != file_size or !std.mem.eql(u8, bytes[0..8], magic)) return Error.InvalidSettings;
    if (readU32(bytes[8..12]) != version) return Error.InvalidSettings;

    const mode = modeFromFile(readI32(bytes[12..16])) orelse return Error.InvalidSettings;
    const map = colormap.fromInt(readI32(bytes[16..20])) orelse return Error.InvalidSettings;
    const band = frequency_band.fromInt(readI32(bytes[24..28])) orelse return Error.InvalidSettings;
    var settings = Settings{
        .mode = mode,
        .colormap = map,
        .frequency_band = band,
        .custom_range = .{
            .min_hz = readF32(bytes[28..32]),
            .max_hz = readF32(bytes[32..36]),
        },
    };
    return settings.sanitize();
}

fn encode(settings_arg: Settings, bytes: *[file_size]u8) void {
    const settings = settings_arg.sanitize();
    @memset(bytes, 0);
    @memcpy(bytes[0..8], magic);
    writeU32(bytes[8..12], version);
    writeI32(bytes[12..16], modeToFile(settings.mode));
    writeI32(bytes[16..20], @intCast(settings.colormap.index()));
    writeI32(bytes[20..24], 0);
    writeI32(bytes[24..28], @intCast(settings.frequency_band.index()));
    writeF32(bytes[28..32], @floatCast(settings.custom_range.min_hz));
    writeF32(bytes[32..36], @floatCast(settings.custom_range.max_hz));
}

fn customRangeFiniteAndOrdered(range: frequency_band.Range) bool {
    return std.math.isFinite(range.min_hz) and
        std.math.isFinite(range.max_hz) and
        range.min_hz > 0.0 and
        range.max_hz > range.min_hz;
}

fn modeFromFile(value: i32) ?analysis.Mode {
    return switch (value) {
        0 => .transient,
        1 => .tonal,
        7 => .sparse,
        else => null,
    };
}

fn modeToFile(mode: analysis.Mode) i32 {
    return switch (mode) {
        .transient => 0,
        .tonal => 1,
        .sparse => 7,
    };
}

fn readU32(bytes: []const u8) u32 {
    return std.mem.readInt(u32, bytes[0..4], .little);
}

fn readI32(bytes: []const u8) i32 {
    return @bitCast(readU32(bytes));
}

fn readF32(bytes: []const u8) f32 {
    return @bitCast(readU32(bytes));
}

fn writeU32(bytes: []u8, value: u32) void {
    std.mem.writeInt(u32, bytes[0..4], value, .little);
}

fn writeI32(bytes: []u8, value: i32) void {
    writeU32(bytes, @bitCast(value));
}

fn writeF32(bytes: []u8, value: f32) void {
    writeU32(bytes, @bitCast(value));
}

fn defaultIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "settings round-trip preserves survivor mode file values" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    const cases = [_]struct {
        mode: analysis.Mode,
        file_value: i32,
    }{
        .{ .mode = .transient, .file_value = 0 },
        .{ .mode = .tonal, .file_value = 1 },
        .{ .mode = .sparse, .file_value = 7 },
    };

    for (cases, 0..) |case, index| {
        var name_buf: [32]u8 = undefined;
        const name = try std.fmt.bufPrint(&name_buf, "sounds-{d}.settings", .{index});
        const written = Settings{
            .mode = case.mode,
            .colormap = .turbo,
            .frequency_band = .custom,
            .custom_range = .{ .min_hz = 123.0, .max_hz = 4567.0 },
        };
        try saveToDir(std.testing.io, tmp.dir, name, written);

        var bytes: [file_size + 1]u8 = undefined;
        const raw = try tmp.dir.readFile(std.testing.io, name, &bytes);
        try std.testing.expectEqual(@as(i32, case.file_value), readI32(raw[12..16]));

        const loaded = try loadFromDir(std.testing.io, tmp.dir, name);
        try std.testing.expectEqual(written.mode, loaded.mode);
        try std.testing.expectEqual(written.colormap, loaded.colormap);
        try std.testing.expectEqual(written.frequency_band, loaded.frequency_band);
        try std.testing.expectApproxEqAbs(written.custom_range.min_hz, loaded.custom_range.min_hz, 0.001);
        try std.testing.expectApproxEqAbs(written.custom_range.max_hz, loaded.custom_range.max_hz, 0.001);
    }
}

test "settings removed mode values load defaults and rewrite the file" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    var bytes: [file_size]u8 = undefined;
    encode(.{
        .mode = .sparse,
        .colormap = .turbo,
        .frequency_band = .custom,
        .custom_range = .{ .min_hz = 123.0, .max_hz = 4567.0 },
    }, &bytes);
    writeI32(bytes[12..16], 4);
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "sounds.settings", .data = &bytes });

    const loaded = try loadFromDir(std.testing.io, tmp.dir, "sounds.settings");
    try std.testing.expectEqual(defaults(), loaded);

    var rewritten_storage: [file_size + 1]u8 = undefined;
    const rewritten = try tmp.dir.readFile(std.testing.io, "sounds.settings", &rewritten_storage);
    try std.testing.expectEqual(@as(i32, 1), readI32(rewritten[12..16]));
}

test "settings reject non-finite custom ranges" {
    var bytes: [file_size]u8 = undefined;
    encode(.{
        .frequency_band = .custom,
        .custom_range = .{ .min_hz = std.math.nan(f64), .max_hz = 1000.0 },
    }, &bytes);

    const loaded = try decode(&bytes);
    try std.testing.expectEqual(frequency_band.default_custom, loaded.custom_range);
}
