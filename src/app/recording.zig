//! Recording capture window and WAV writing.
//! Rewrite target; C reference: src_c/src/app/recording.c

const std = @import("std");
const ring_buffer = @import("../audio/ring_buffer.zig");

pub const directory = "recordings";
pub const path_capacity = 512;

const file_prefix = "sounds-";
const file_suffix = "32f";
const file_extension = ".wav";
const maximum_wav_channels = 32;
const maximum_wav_sample_rate = 512_000;

pub const Error = error{
    InvalidRecordingOutput,
    InvalidWavSampleRate,
    RecordingTooLarge,
    InvalidWavMetadata,
    UnsupportedWavFormat,
    InvalidWavSampleCount,
    TimeUnavailable,
    TimestampFormatFailed,
};

pub const Info = struct {
    sample_rate: f64,
    duration_seconds: f64,
    sample_count: u64,
};

pub const Saved = struct {
    path: []u8,
    sample_count: u64,

    pub fn deinit(self: Saved, allocator: std.mem.Allocator) void {
        allocator.free(self.path);
    }
};

pub const Loaded = struct {
    samples: []f32,
    sample_rate: f64,

    pub fn deinit(self: Loaded, allocator: std.mem.Allocator) void {
        allocator.free(self.samples);
    }
};

const WavInfo = struct {
    format: u16 = 0,
    channels: u16 = 0,
    bits_per_sample: u16 = 0,
    block_align: u16 = 0,
    sample_rate: u32 = 0,
    data_offset: u64 = 0,
    data_bytes: u64 = 0,
};

const C = struct {
    const Tm = extern struct {
        tm_sec: c_int,
        tm_min: c_int,
        tm_hour: c_int,
        tm_mday: c_int,
        tm_mon: c_int,
        tm_year: c_int,
        tm_wday: c_int,
        tm_yday: c_int,
        tm_isdst: c_int,
        tm_gmtoff: c_long,
        tm_zone: ?[*:0]const u8,
    };

    extern "c" fn time(timer: ?*std.c.time_t) std.c.time_t;
    extern "c" fn localtime_r(timer: *const std.c.time_t, result: *Tm) ?*Tm;
    extern "c" fn strftime(buffer: [*]u8, max_size: usize, format: [*:0]const u8, time_ptr: *const Tm) usize;
};

pub fn saveLatest(allocator: std.mem.Allocator, ring: *const ring_buffer.RingBuffer, sample_rate: f64) !Saved {
    return saveRecent(allocator, ring, @intCast(ring.capacity()), sample_rate);
}

pub fn saveRecent(
    allocator: std.mem.Allocator,
    ring: *const ring_buffer.RingBuffer,
    requested_samples: u64,
    sample_rate: f64,
) !Saved {
    if (ring.capacity() == 0 or requested_samples == 0) {
        return .{ .path = try allocator.dupe(u8, ""), .sample_count = 0 };
    }

    const wanted_u64 = @min(requested_samples, @as(u64, @intCast(ring.capacity())));
    if (wanted_u64 > std.math.maxInt(usize)) return Error.RecordingTooLarge;
    const wanted: usize = @intCast(wanted_u64);
    const samples = try allocator.alloc(f32, wanted);
    defer allocator.free(samples);

    const count = ring.readLatest(samples);
    return saveSamples(allocator, samples[0..count], sample_rate);
}

pub fn saveSamples(allocator: std.mem.Allocator, samples: []const f32, sample_rate: f64) !Saved {
    return saveSamplesInDir(defaultIo(), .cwd(), allocator, samples, sample_rate);
}

pub fn saveSamplesInDir(
    io: std.Io,
    base_dir: std.Io.Dir,
    allocator: std.mem.Allocator,
    samples: []const f32,
    sample_rate: f64,
) !Saved {
    if (samples.len == 0) {
        return .{ .path = try allocator.dupe(u8, ""), .sample_count = 0 };
    }

    try base_dir.createDirPath(io, directory);
    const path = try buildRecordingPath(allocator);
    errdefer allocator.free(path);

    const bytes = try wavBytes(allocator, samples, sample_rate);
    defer allocator.free(bytes);

    try base_dir.writeFile(io, .{ .sub_path = path, .data = bytes });
    return .{ .path = path, .sample_count = samples.len };
}

pub fn readInfo(path: []const u8) !Info {
    return readInfoInDir(defaultIo(), .cwd(), path);
}

pub fn readInfoInDir(io: std.Io, base_dir: std.Io.Dir, path: []const u8) !Info {
    const bytes = try base_dir.readFileAlloc(io, path, std.heap.c_allocator, .limited(std.math.maxInt(u32) + 64));
    defer std.heap.c_allocator.free(bytes);
    const wav = try parseWavInfo(bytes);
    const frames = wav.data_bytes / wav.block_align;
    return .{
        .sample_rate = @floatFromInt(wav.sample_rate),
        .duration_seconds = @as(f64, @floatFromInt(frames)) / @as(f64, @floatFromInt(wav.sample_rate)),
        .sample_count = frames,
    };
}

pub fn loadSamples(allocator: std.mem.Allocator, path: []const u8) !Loaded {
    return loadSamplesInDir(defaultIo(), .cwd(), allocator, path);
}

pub fn loadSamplesInDir(
    io: std.Io,
    base_dir: std.Io.Dir,
    allocator: std.mem.Allocator,
    path: []const u8,
) !Loaded {
    const bytes = try base_dir.readFileAlloc(io, path, allocator, .limited(std.math.maxInt(u32) + 64));
    defer allocator.free(bytes);

    const wav = try parseWavInfo(bytes);
    const frames = wav.data_bytes / wav.block_align;
    if (frames == 0 or frames > std.math.maxInt(usize)) return Error.InvalidWavSampleCount;

    const samples = try allocator.alloc(f32, @intCast(frames));
    errdefer allocator.free(samples);
    try readWavSamples(bytes, wav, samples);
    return .{ .samples = samples, .sample_rate = @floatFromInt(wav.sample_rate) };
}

fn buildRecordingPath(allocator: std.mem.Allocator) ![]u8 {
    var stamp: [15]u8 = undefined;
    try timestamp(&stamp);
    return std.fmt.allocPrint(
        allocator,
        "{s}/{s}{s}-{s}{s}",
        .{ directory, file_prefix, &stamp, file_suffix, file_extension },
    );
}

fn timestamp(out: *[15]u8) !void {
    const now = C.time(null);
    if (now == @as(std.c.time_t, -1)) return Error.TimeUnavailable;

    var local: C.Tm = undefined;
    if (C.localtime_r(&now, &local) == null) return Error.TimeUnavailable;

    var scratch: [16]u8 = undefined;
    const length = C.strftime(&scratch, scratch.len, "%Y%m%d-%H%M%S", &local);
    if (length != out.len) return Error.TimestampFormatFailed;
    @memcpy(out, scratch[0..length]);
}

fn wavBytes(allocator: std.mem.Allocator, samples: []const f32, sample_rate: f64) ![]u8 {
    if (!std.math.isFinite(sample_rate) or
        sample_rate <= 0.0 or
        sample_rate > maximum_wav_sample_rate)
    {
        return Error.InvalidWavSampleRate;
    }
    if (samples.len > std.math.maxInt(u32) / 4) return Error.RecordingTooLarge;
    if (samples.len > (std.math.maxInt(usize) - 56) / 4) return Error.RecordingTooLarge;

    const data_bytes: u32 = @intCast(samples.len * 4);
    const sample_count_u32: u32 = @intCast(samples.len);
    const rate: u32 = @intFromFloat(@round(sample_rate));
    const block_align: u16 = 4;
    const byte_rate_64 = @as(u64, rate) * block_align;
    if (byte_rate_64 > std.math.maxInt(u32)) return Error.RecordingTooLarge;
    const riff_size: u32 = 48 + data_bytes;

    const bytes = try allocator.alloc(u8, 56 + samples.len * 4);
    errdefer allocator.free(bytes);
    @memset(bytes, 0);

    @memcpy(bytes[0..4], "RIFF");
    writeU32(bytes[4..8], riff_size);
    @memcpy(bytes[8..12], "WAVE");
    @memcpy(bytes[12..16], "fmt ");
    writeU32(bytes[16..20], 16);
    writeU16(bytes[20..22], 3);
    writeU16(bytes[22..24], 1);
    writeU32(bytes[24..28], rate);
    writeU32(bytes[28..32], @intCast(byte_rate_64));
    writeU16(bytes[32..34], block_align);
    writeU16(bytes[34..36], 32);
    @memcpy(bytes[36..40], "fact");
    writeU32(bytes[40..44], 4);
    writeU32(bytes[44..48], sample_count_u32);
    @memcpy(bytes[48..52], "data");
    writeU32(bytes[52..56], data_bytes);

    var offset: usize = 56;
    for (samples) |sample| {
        writeU32(bytes[offset..][0..4], @bitCast(sample));
        offset += 4;
    }
    return bytes;
}

fn parseWavInfo(bytes: []const u8) !WavInfo {
    if (bytes.len < 12 or
        !std.mem.eql(u8, bytes[0..4], "RIFF") or
        !std.mem.eql(u8, bytes[8..12], "WAVE"))
    {
        return Error.InvalidWavMetadata;
    }

    var info = WavInfo{};
    var offset: usize = 12;
    while (offset < bytes.len) {
        if (bytes.len - offset < 8) return Error.InvalidWavMetadata;
        const id = bytes[offset..][0..4];
        const size = readU32(bytes[offset + 4 ..][0..4]);
        const body_start = offset + 8;
        const body_end = body_start + @as(usize, @intCast(size));
        const padded_end = body_end + (size & 1);
        if (body_end < body_start or padded_end < body_end or padded_end > bytes.len) {
            return Error.InvalidWavMetadata;
        }

        if (std.mem.eql(u8, id, "fmt ")) {
            if (size < 16) return Error.InvalidWavMetadata;
            info.format = readU16(bytes[body_start..][0..2]);
            info.channels = readU16(bytes[body_start + 2 ..][0..2]);
            info.sample_rate = readU32(bytes[body_start + 4 ..][0..4]);
            info.block_align = readU16(bytes[body_start + 12 ..][0..2]);
            info.bits_per_sample = readU16(bytes[body_start + 14 ..][0..2]);
        } else if (std.mem.eql(u8, id, "data")) {
            info.data_offset = body_start;
            info.data_bytes = size;
        }
        offset = padded_end;
    }

    if (!wavInfoValid(info)) return Error.InvalidWavMetadata;
    return info;
}

fn wavInfoValid(info: WavInfo) bool {
    if (info.sample_rate == 0 or
        info.sample_rate > maximum_wav_sample_rate or
        info.channels == 0 or
        info.channels > maximum_wav_channels or
        info.block_align == 0 or
        info.data_bytes < info.block_align or
        (info.data_bytes % info.block_align) != 0 or
        !wavFormatSupported(info))
    {
        return false;
    }

    const bytes_per_channel = (@as(u32, info.bits_per_sample) + 7) / 8;
    if (bytes_per_channel == 0 or info.channels > std.math.maxInt(u16) / bytes_per_channel) {
        return false;
    }

    const expected_align = bytes_per_channel * info.channels;
    return expected_align > 0 and info.block_align >= expected_align;
}

fn wavFormatSupported(info: WavInfo) bool {
    if (info.format == 3 and info.bits_per_sample == 32) return true;
    return info.format == 1 and
        (info.bits_per_sample == 16 or info.bits_per_sample == 24 or info.bits_per_sample == 32);
}

fn readWavSamples(bytes: []const u8, info: WavInfo, samples: []f32) !void {
    const frames = info.data_bytes / info.block_align;
    if (samples.len < frames) return Error.InvalidWavSampleCount;

    const bytes_per_channel = (@as(u32, info.bits_per_sample) + 7) / 8;
    const expected_align = bytes_per_channel * info.channels;
    if (info.block_align < expected_align) return Error.InvalidWavMetadata;

    var offset: usize = @intCast(info.data_offset);
    for (samples[0..@intCast(frames)]) |*sample| {
        var mixed: f64 = 0.0;
        var channel: u16 = 0;
        while (channel < info.channels) : (channel += 1) {
            mixed += try readSampleValue(bytes, &offset, info);
        }
        offset += info.block_align - expected_align;
        sample.* = @floatCast(mixed / @as(f64, @floatFromInt(info.channels)));
    }
}

fn readSampleValue(bytes: []const u8, offset: *usize, info: WavInfo) !f32 {
    if (info.format == 3 and info.bits_per_sample == 32) {
        if (bytes.len - offset.* < 4) return Error.InvalidWavMetadata;
        const value: f32 = @bitCast(readU32(bytes[offset.*..][0..4]));
        offset.* += 4;
        return value;
    }

    if (info.format == 1 and info.bits_per_sample == 16) {
        if (bytes.len - offset.* < 2) return Error.InvalidWavMetadata;
        const value: i16 = @bitCast(readU16(bytes[offset.*..][0..2]));
        offset.* += 2;
        return @as(f32, @floatFromInt(value)) / 32768.0;
    }

    if (info.format == 1 and info.bits_per_sample == 24) {
        if (bytes.len - offset.* < 3) return Error.InvalidWavMetadata;
        var value: i32 =
            @as(i32, bytes[offset.*]) |
            (@as(i32, bytes[offset.* + 1]) << 8) |
            (@as(i32, bytes[offset.* + 2]) << 16);
        if ((value & 0x800000) != 0) value |= ~@as(i32, 0xFFFFFF);
        offset.* += 3;
        return @as(f32, @floatFromInt(value)) / 8_388_608.0;
    }

    if (info.format == 1 and info.bits_per_sample == 32) {
        if (bytes.len - offset.* < 4) return Error.InvalidWavMetadata;
        const value: i32 = @bitCast(readU32(bytes[offset.*..][0..4]));
        offset.* += 4;
        return @as(f32, @floatFromInt(value)) / 2_147_483_648.0;
    }

    return Error.UnsupportedWavFormat;
}

fn readU16(bytes: []const u8) u16 {
    return std.mem.readInt(u16, bytes[0..2], .little);
}

fn readU32(bytes: []const u8) u32 {
    return std.mem.readInt(u32, bytes[0..4], .little);
}

fn writeU16(bytes: []u8, value: u16) void {
    std.mem.writeInt(u16, bytes[0..2], value, .little);
}

fn writeU32(bytes: []u8, value: u32) void {
    std.mem.writeInt(u32, bytes[0..4], value, .little);
}

fn defaultIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

fn writeHostileWav(
    io: std.Io,
    dir: std.Io.Dir,
    path: []const u8,
    sample_rate: u32,
    channels: u16,
    block_align: u16,
    data_bytes: u32,
    actual_data_bytes: u32,
) !void {
    var header = [_]u8{0} ** 44;
    @memcpy(header[0..4], "RIFF");
    writeU32(header[4..8], 36 + data_bytes);
    @memcpy(header[8..12], "WAVE");
    @memcpy(header[12..16], "fmt ");
    writeU32(header[16..20], 16);
    writeU16(header[20..22], 3);
    writeU16(header[22..24], channels);
    writeU32(header[24..28], sample_rate);
    writeU32(header[28..32], sample_rate * block_align);
    writeU16(header[32..34], block_align);
    writeU16(header[34..36], 32);
    @memcpy(header[36..40], "data");
    writeU32(header[40..44], data_bytes);

    const bytes = try std.testing.allocator.alloc(u8, header.len + actual_data_bytes);
    defer std.testing.allocator.free(bytes);
    @memcpy(bytes[0..header.len], &header);
    @memset(bytes[header.len..], 0);
    try dir.writeFile(io, .{ .sub_path = path, .data = bytes });
}

test "ported recording WAV writing, naming, and readback assertions" {
    var tmp = std.testing.tmpDir(.{ .access_sub_paths = true });
    defer tmp.cleanup();

    var ring = try ring_buffer.RingBuffer.init(std.testing.allocator, 16);
    defer ring.deinit();
    const samples = [_]f32{ -1.0, -0.25, 0.25, 1.0 };
    ring.write(&samples);

    const saved_from_ring = try saveRecentInTestDir(std.testing.io, tmp.dir, &ring, 4, 48_000.0);
    defer saved_from_ring.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(usize, 4), saved_from_ring.sample_count);
    try std.testing.expect(std.mem.indexOf(u8, saved_from_ring.path, "-32f.wav") != null);
    try checkWavHeader(std.testing.io, tmp.dir, saved_from_ring.path, 16);

    const saved_samples = try saveSamplesInDir(std.testing.io, tmp.dir, std.testing.allocator, &samples, 48_000.0);
    defer saved_samples.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(usize, 4), saved_samples.sample_count);
    try std.testing.expect(std.mem.indexOf(u8, saved_samples.path, "-32f.wav") != null);
    try checkWavHeader(std.testing.io, tmp.dir, saved_samples.path, 16);

    const info = try readInfoInDir(std.testing.io, tmp.dir, saved_samples.path);
    try std.testing.expectEqual(@as(u64, 4), info.sample_count);
    try std.testing.expectEqual(@as(f64, 48_000.0), info.sample_rate);

    const loaded = try loadSamplesInDir(std.testing.io, tmp.dir, std.testing.allocator, saved_samples.path);
    defer loaded.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(f64, 48_000.0), loaded.sample_rate);
    try std.testing.expectEqualSlices(f32, &samples, loaded.samples);
}

test "ported hostile WAV metadata is rejected" {
    var tmp = std.testing.tmpDir(.{ .access_sub_paths = true });
    defer tmp.cleanup();

    try writeHostileWav(std.testing.io, tmp.dir, "bad-rate.wav", 600_000, 1, 4, 4, 4);
    try std.testing.expectError(Error.InvalidWavMetadata, readInfoInDir(std.testing.io, tmp.dir, "bad-rate.wav"));

    try writeHostileWav(std.testing.io, tmp.dir, "bad-channels.wav", 48_000, 33, 132, 132, 132);
    try std.testing.expectError(Error.InvalidWavMetadata, readInfoInDir(std.testing.io, tmp.dir, "bad-channels.wav"));

    try writeHostileWav(std.testing.io, tmp.dir, "truncated.wav", 48_000, 1, 4, 4, 0);
    try std.testing.expectError(Error.InvalidWavMetadata, readInfoInDir(std.testing.io, tmp.dir, "truncated.wav"));
}

test "stale recording stop reads return retained audio" {
    var ring = try ring_buffer.RingBuffer.init(std.testing.allocator, 4);
    defer ring.deinit();

    ring.write(&[_]f32{ -1.0, -0.25, 0.25, 1.0 });
    const stopped_at = ring.written();
    ring.write(&[_]f32{5.0});

    var stopped: [4]f32 = undefined;
    const count = ring.readAvailableEndingAt(stopped_at, &stopped);
    try std.testing.expectEqual(@as(usize, 3), count);
    try std.testing.expectEqualSlices(f32, &[_]f32{ -0.25, 0.25, 1.0 }, stopped[0..count]);
}

fn saveRecentInTestDir(
    io: std.Io,
    dir: std.Io.Dir,
    ring: *const ring_buffer.RingBuffer,
    requested_samples: u64,
    sample_rate: f64,
) !Saved {
    const wanted_u64 = @min(requested_samples, @as(u64, @intCast(ring.capacity())));
    const samples = try std.testing.allocator.alloc(f32, @intCast(wanted_u64));
    defer std.testing.allocator.free(samples);
    const count = ring.readLatest(samples);
    return saveSamplesInDir(io, dir, std.testing.allocator, samples[0..count], sample_rate);
}

fn checkWavHeader(io: std.Io, dir: std.Io.Dir, path_arg: []const u8, expected_data_bytes: u32) !void {
    var prefix: [56]u8 = undefined;
    const bytes = try dir.readFile(io, path_arg, &prefix);
    try std.testing.expectEqual(@as(usize, prefix.len), bytes.len);
    try std.testing.expectEqualStrings("RIFF", bytes[0..4]);
    try std.testing.expectEqualStrings("WAVE", bytes[8..12]);
    try std.testing.expectEqualStrings("fmt ", bytes[12..16]);
    try std.testing.expectEqual(@as(u16, 3), readU16(bytes[20..22]));
    try std.testing.expectEqual(@as(u16, 1), readU16(bytes[22..24]));
    try std.testing.expectEqual(@as(u32, 48_000), readU32(bytes[24..28]));
    try std.testing.expectEqual(@as(u16, 32), readU16(bytes[34..36]));
    try std.testing.expectEqualStrings("fact", bytes[36..40]);
    try std.testing.expectEqual(@as(u32, 4), readU32(bytes[40..44]));
    try std.testing.expectEqual(@as(u32, 4), readU32(bytes[44..48]));
    try std.testing.expectEqualStrings("data", bytes[48..52]);
    try std.testing.expectEqual(expected_data_bytes, readU32(bytes[52..56]));
}
