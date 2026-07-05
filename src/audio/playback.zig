//! Core Audio HAL playback of mono float clips.
//! Rewrite target; C reference: src_c/src/audio/playback.c

const std = @import("std");
const ca = @import("../apple/coreaudio.zig");

pub const maximum_output_channels: usize = 32;
pub const maximum_output_sample_rate: f64 = 512_000.0;

pub const Error = std.mem.Allocator.Error || error{
    MissingPlaybackStream,
    NoDefaultOutputDevice,
    NoOutputStreams,
    InvalidOutputSampleRate,
    InvalidOutputChannelCount,
    UnsupportedOutputFormat,
    InvalidPlaybackBuffer,
    CoreAudioGetDefaultOutputDeviceFailed,
    CoreAudioGetOutputStreamsSizeFailed,
    CoreAudioGetOutputStreamsFailed,
    CoreAudioGetStreamVirtualFormatFailed,
    CoreAudioCreateIOProcFailed,
    CoreAudioStartFailed,
    CoreAudioStopFailed,
};

pub const Format = struct {
    sample_rate: f64,
    format_id: u32,
    format_flags: u32,
    bits_per_channel: u32,
    channels_per_frame: u32,
    bytes_per_frame: u32,

    fn fromASBD(source: ca.AudioStreamBasicDescription) Format {
        return .{
            .sample_rate = source.mSampleRate,
            .format_id = source.mFormatID,
            .format_flags = source.mFormatFlags,
            .bits_per_channel = source.mBitsPerChannel,
            .channels_per_frame = source.mChannelsPerFrame,
            .bytes_per_frame = source.mBytesPerFrame,
        };
    }
};

pub const Playback = struct {
    allocator: std.mem.Allocator,
    device: ca.AudioDeviceID,
    callback_id: ?ca.AudioDeviceIOProcID,
    samples: []f32,
    source_frame: f64,
    source_step: f64,
    output_sample_rate: f64,
    output_format: Format,
    playing: std.atomic.Value(bool),
    playback_position: std.atomic.Value(u64),
    started: bool,

    pub fn open(allocator: std.mem.Allocator) Error!*Playback {
        const playback = try allocator.create(Playback);
        playback.* = .{
            .allocator = allocator,
            .device = ca.kAudioObjectUnknown,
            .callback_id = null,
            .samples = &.{},
            .source_frame = 0.0,
            .source_step = 1.0,
            .output_sample_rate = 0.0,
            .output_format = std.mem.zeroes(Format),
            .playing = std.atomic.Value(bool).init(false),
            .playback_position = std.atomic.Value(u64).init(0),
            .started = false,
        };
        errdefer playback.close();

        playback.device = try defaultOutputDevice();
        const asbd = try firstOutputStreamFormat(allocator, playback.device);
        try validateOutputFormat(asbd);

        playback.output_sample_rate = asbd.mSampleRate;
        playback.output_format = Format.fromASBD(asbd);

        var callback_id: ?ca.AudioDeviceIOProcID = null;
        if (ca.AudioDeviceCreateIOProcID(
            playback.device,
            streamOutputCallback,
            playback,
            &callback_id,
        ) != ca.noErr) {
            return Error.CoreAudioCreateIOProcFailed;
        }
        playback.callback_id = callback_id orelse return Error.CoreAudioCreateIOProcFailed;

        return playback;
    }

    pub fn start(self: *Playback, samples: []const f32, sample_rate: f64) Error!void {
        if (samples.len == 0 or
            !std.math.isFinite(sample_rate) or
            sample_rate <= 0.0 or
            sample_rate > maximum_output_sample_rate)
        {
            return Error.InvalidPlaybackBuffer;
        }

        try self.stopDevice();
        self.releaseSamples();

        const copy = try self.allocator.dupe(f32, samples);
        self.samples = copy;
        self.source_frame = 0.0;
        self.source_step = sample_rate / self.output_sample_rate;
        self.playback_position.store(0, .release);
        self.playing.store(true, .release);

        const callback_id = self.callback_id orelse {
            self.playing.store(false, .release);
            self.releaseSamples();
            return Error.CoreAudioStartFailed;
        };

        if (ca.AudioDeviceStart(self.device, callback_id) != ca.noErr) {
            self.playing.store(false, .release);
            self.releaseSamples();
            return Error.CoreAudioStartFailed;
        }

        self.started = true;
    }

    pub fn stop(self: *Playback) Error!void {
        try self.stopDevice();
        self.playing.store(false, .release);
        self.releaseSamples();
    }

    pub fn isPlaying(self: *const Playback) bool {
        return self.playing.load(.acquire);
    }

    pub fn position(self: *const Playback) u64 {
        return self.playback_position.load(.acquire);
    }

    pub fn format(self: *const Playback) Format {
        return self.output_format;
    }

    pub fn close(self: *Playback) void {
        if (self.started) {
            if (self.callback_id) |callback_id| {
                _ = ca.AudioDeviceStop(self.device, callback_id);
            }
        }

        if (self.callback_id) |callback_id| {
            _ = ca.AudioDeviceDestroyIOProcID(self.device, callback_id);
        }

        self.releaseSamples();
        self.allocator.destroy(self);
    }

    fn stopDevice(self: *Playback) Error!void {
        if (!self.started) return;

        const callback_id = self.callback_id orelse return Error.CoreAudioStopFailed;
        if (ca.AudioDeviceStop(self.device, callback_id) != ca.noErr) {
            return Error.CoreAudioStopFailed;
        }
        self.started = false;
    }

    fn releaseSamples(self: *Playback) void {
        self.allocator.free(self.samples);
        self.samples = &.{};
        self.source_frame = 0.0;
        self.source_step = 1.0;
    }
};

pub fn open(allocator: std.mem.Allocator) Error!*Playback {
    return Playback.open(allocator);
}

pub fn defaultOutputFormat(allocator: std.mem.Allocator) Error!Format {
    const device = try defaultOutputDevice();
    return Format.fromASBD(try firstOutputStreamFormat(allocator, device));
}

pub fn start(playback: ?*Playback, samples: []const f32, sample_rate: f64) Error!void {
    const live_playback = playback orelse return Error.MissingPlaybackStream;
    return live_playback.start(samples, sample_rate);
}

pub fn stop(playback: ?*Playback) Error!void {
    const live_playback = playback orelse return Error.MissingPlaybackStream;
    return live_playback.stop();
}

pub fn isPlaying(playback: ?*const Playback) bool {
    const live_playback = playback orelse return false;
    return live_playback.isPlaying();
}

pub fn position(playback: ?*const Playback) u64 {
    const live_playback = playback orelse return 0;
    return live_playback.position();
}

fn defaultOutputDevice() Error!ca.AudioDeviceID {
    var device: ca.AudioDeviceID = ca.kAudioObjectUnknown;
    var size: u32 = @sizeOf(ca.AudioDeviceID);
    const address = ca.AudioObjectPropertyAddress{
        .mSelector = ca.kAudioHardwarePropertyDefaultOutputDevice,
        .mScope = ca.kAudioObjectPropertyScopeGlobal,
        .mElement = ca.kAudioObjectPropertyElementMain,
    };

    if (ca.AudioObjectGetPropertyData(
        ca.kAudioObjectSystemObject,
        &address,
        0,
        null,
        &size,
        &device,
    ) != ca.noErr) {
        return Error.CoreAudioGetDefaultOutputDeviceFailed;
    }

    if (device == ca.kAudioObjectUnknown) return Error.NoDefaultOutputDevice;
    return device;
}

fn firstOutputStreamFormat(
    allocator: std.mem.Allocator,
    device: ca.AudioDeviceID,
) Error!ca.AudioStreamBasicDescription {
    const streams_address = ca.AudioObjectPropertyAddress{
        .mSelector = ca.kAudioDevicePropertyStreams,
        .mScope = ca.kAudioDevicePropertyScopeOutput,
        .mElement = ca.kAudioObjectPropertyElementMain,
    };

    var size: u32 = 0;
    if (ca.AudioObjectGetPropertyDataSize(device, &streams_address, 0, null, &size) != ca.noErr) {
        return Error.CoreAudioGetOutputStreamsSizeFailed;
    }

    if (size < @sizeOf(ca.AudioStreamID) or (size % @sizeOf(ca.AudioStreamID)) != 0) {
        return Error.NoOutputStreams;
    }

    const stream_count: usize = @intCast(size / @sizeOf(ca.AudioStreamID));
    const streams = try allocator.alloc(ca.AudioStreamID, stream_count);
    defer allocator.free(streams);

    if (ca.AudioObjectGetPropertyData(
        device,
        &streams_address,
        0,
        null,
        &size,
        streams.ptr,
    ) != ca.noErr) {
        return Error.CoreAudioGetOutputStreamsFailed;
    }

    var format = std.mem.zeroes(ca.AudioStreamBasicDescription);
    var format_size: u32 = @sizeOf(ca.AudioStreamBasicDescription);
    const format_address = ca.AudioObjectPropertyAddress{
        .mSelector = ca.kAudioStreamPropertyVirtualFormat,
        .mScope = ca.kAudioObjectPropertyScopeGlobal,
        .mElement = ca.kAudioObjectPropertyElementMain,
    };

    if (ca.AudioObjectGetPropertyData(
        streams[0],
        &format_address,
        0,
        null,
        &format_size,
        &format,
    ) != ca.noErr) {
        return Error.CoreAudioGetStreamVirtualFormatFailed;
    }

    return format;
}

fn validateOutputFormat(format: ca.AudioStreamBasicDescription) Error!void {
    if (!std.math.isFinite(format.mSampleRate) or
        format.mSampleRate <= 0.0 or
        format.mSampleRate > maximum_output_sample_rate)
    {
        return Error.InvalidOutputSampleRate;
    }

    if (format.mChannelsPerFrame == 0 or format.mChannelsPerFrame > maximum_output_channels) {
        return Error.InvalidOutputChannelCount;
    }

    if (format.mFormatID != ca.kAudioFormatLinearPCM or
        (format.mFormatFlags & ca.kAudioFormatFlagIsFloat) == 0 or
        format.mBitsPerChannel != 32)
    {
        return Error.UnsupportedOutputFormat;
    }
}

fn bufferFrameCount(buffer: *const ca.AudioBuffer) usize {
    if (buffer.mData == null or buffer.mDataByteSize == 0) return 0;

    const channels: usize = if (buffer.mNumberChannels == 0) 1 else buffer.mNumberChannels;
    if (channels > maximum_output_channels) return 0;

    return @as(usize, buffer.mDataByteSize) / (@sizeOf(f32) * channels);
}

fn outputFrameCount(buffers: *const ca.AudioBufferList) usize {
    var frames: usize = 0;
    for (buffers.buffers()) |*buffer| {
        frames = @max(frames, bufferFrameCount(buffer));
    }
    return frames;
}

fn clearOutput(buffers: *ca.AudioBufferList) void {
    for (buffers.buffers()) |*buffer| {
        const data = buffer.mData orelse continue;
        if (buffer.mDataByteSize == 0) continue;

        const bytes: [*]u8 = @ptrCast(data);
        @memset(bytes[0..buffer.mDataByteSize], 0);
    }
}

fn writeOutputSample(buffers: *ca.AudioBufferList, frame: usize, sample: f32) void {
    for (buffers.buffers()) |*buffer| {
        const data = buffer.mData orelse continue;
        const channels: usize = if (buffer.mNumberChannels == 0) 1 else buffer.mNumberChannels;
        if (channels > maximum_output_channels) continue;

        const frames = bufferFrameCount(buffer);
        if (frame >= frames) continue;

        const output: [*]align(1) f32 = @ptrCast(data);
        writeMirroredFrame(output, frame, channels, sample);
    }
}

fn writeMirroredFrame(output: [*]align(1) f32, frame: usize, channels: usize, sample: f32) void {
    const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
    const Vec = @Vector(lanes, f32);
    const samples: Vec = @splat(sample);
    const offset = frame * channels;
    var channel: usize = 0;
    while (channel + lanes <= channels) : (channel += lanes) {
        output[offset + channel ..][0..lanes].* = samples;
    }
    while (channel < channels) : (channel += 1) {
        output[offset + channel] = sample;
    }
}

fn sourceSampleAt(playback: *const Playback, frame: f64) f32 {
    const index: usize = @intFromFloat(frame);
    if (index >= playback.samples.len) return 0.0;

    const next = index + 1;
    if (next >= playback.samples.len) return playback.samples[index];

    const a = playback.samples[index];
    const b = playback.samples[next];
    const amount: f32 = @floatCast(frame - @as(f64, @floatFromInt(index)));
    return a + ((b - a) * amount);
}

fn clampedPosition(playback: *const Playback) u64 {
    const frame_count: f64 = @floatFromInt(playback.samples.len);
    if (playback.source_frame >= frame_count) return @intCast(playback.samples.len);
    if (playback.source_frame <= 0.0) return 0;
    return @intFromFloat(playback.source_frame);
}

fn renderNextSample(playback: *Playback, sample: *f32) bool {
    const frame_count: f64 = @floatFromInt(playback.samples.len);
    if (playback.source_frame >= frame_count) {
        sample.* = 0.0;
        return false;
    }

    sample.* = sourceSampleAt(playback, playback.source_frame);
    playback.source_frame += playback.source_step;

    if (playback.source_frame > frame_count) {
        playback.source_frame = frame_count;
    }

    return true;
}

fn streamOutputCallback(
    device: ca.AudioObjectID,
    now: *const ca.AudioTimeStamp,
    input_data: *const ca.AudioBufferList,
    input_time: *const ca.AudioTimeStamp,
    output_data: *ca.AudioBufferList,
    output_time: *const ca.AudioTimeStamp,
    client_data: ?*anyopaque,
) callconv(.c) ca.OSStatus {
    _ = device;
    _ = now;
    _ = input_data;
    _ = input_time;
    _ = output_time;

    const playback: *Playback = @ptrCast(@alignCast(client_data orelse {
        clearOutput(output_data);
        return ca.noErr;
    }));

    if (!playback.playing.load(.acquire)) {
        clearOutput(output_data);
        return ca.noErr;
    }

    const frames = outputFrameCount(output_data);
    var still_playing = true;
    var frame: usize = 0;
    while (frame < frames) : (frame += 1) {
        var sample: f32 = 0.0;
        if (still_playing) {
            still_playing = renderNextSample(playback, &sample);
        }

        writeOutputSample(output_data, frame, sample);
    }

    const playback_pos = clampedPosition(playback);
    playback.playback_position.store(playback_pos, .release);
    if (!still_playing or playback_pos >= playback.samples.len) {
        playback.playing.store(false, .release);
    }

    return ca.noErr;
}

fn validOutputASBD() ca.AudioStreamBasicDescription {
    return .{
        .mSampleRate = 48_000.0,
        .mFormatID = ca.kAudioFormatLinearPCM,
        .mFormatFlags = ca.kAudioFormatFlagIsFloat | ca.kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 4,
        .mChannelsPerFrame = 1,
        .mBitsPerChannel = 32,
        .mReserved = 0,
    };
}

fn hardwareUnavailable(err: anyerror) bool {
    return switch (err) {
        Error.NoDefaultOutputDevice,
        Error.NoOutputStreams,
        Error.CoreAudioGetDefaultOutputDeviceFailed,
        Error.CoreAudioGetOutputStreamsSizeFailed,
        Error.CoreAudioGetOutputStreamsFailed,
        Error.CoreAudioGetStreamVirtualFormatFailed,
        Error.UnsupportedOutputFormat,
        => true,
        else => false,
    };
}

fn testPlayback(samples: []f32, source_step: f64, playing_now: bool) Playback {
    return .{
        .allocator = std.testing.allocator,
        .device = ca.kAudioObjectUnknown,
        .callback_id = null,
        .samples = samples,
        .source_frame = 0.0,
        .source_step = source_step,
        .output_sample_rate = 48_000.0,
        .output_format = Format.fromASBD(validOutputASBD()),
        .playing = std.atomic.Value(bool).init(playing_now),
        .playback_position = std.atomic.Value(u64).init(0),
        .started = false,
    };
}

fn timestamp() ca.AudioTimeStamp {
    return std.mem.zeroes(ca.AudioTimeStamp);
}

test "output format copies and validates Core Audio ASBD fields" {
    const asbd = validOutputASBD();
    try validateOutputFormat(asbd);

    const format = Format.fromASBD(asbd);
    try std.testing.expectEqual(@as(f64, 48_000.0), format.sample_rate);
    try std.testing.expectEqual(ca.kAudioFormatLinearPCM, format.format_id);
    try std.testing.expectEqual(ca.kAudioFormatFlagIsFloat | ca.kAudioFormatFlagIsPacked, format.format_flags);
    try std.testing.expectEqual(@as(u32, 32), format.bits_per_channel);
    try std.testing.expectEqual(@as(u32, 1), format.channels_per_frame);
    try std.testing.expectEqual(@as(u32, 4), format.bytes_per_frame);

    var bad_rate = asbd;
    bad_rate.mSampleRate = maximum_output_sample_rate + 1.0;
    try std.testing.expectError(Error.InvalidOutputSampleRate, validateOutputFormat(bad_rate));

    var bad_channels = asbd;
    bad_channels.mChannelsPerFrame = @intCast(maximum_output_channels + 1);
    try std.testing.expectError(Error.InvalidOutputChannelCount, validateOutputFormat(bad_channels));

    var bad_format = asbd;
    bad_format.mFormatFlags = 0;
    try std.testing.expectError(Error.UnsupportedOutputFormat, validateOutputFormat(bad_format));
}

test "ported playback null-state assertions" {
    var sample = [_]f32{0.0};

    try std.testing.expect(!isPlaying(null));
    try std.testing.expectEqual(@as(u64, 0), position(null));
    try std.testing.expectError(Error.MissingPlaybackStream, start(null, &sample, 48_000.0));
    try std.testing.expectError(Error.MissingPlaybackStream, stop(null));
}

test "playback start validates buffers before touching device state" {
    var source = [_]f32{0.0};
    var playback = testPlayback(source[0..], 1.0, false);

    try std.testing.expectError(Error.InvalidPlaybackBuffer, playback.start(&.{}, 48_000.0));
    try std.testing.expectError(Error.InvalidPlaybackBuffer, playback.start(&source, -1.0));
    try std.testing.expect(!playback.isPlaying());
    try std.testing.expectEqual(@as(usize, 1), playback.samples.len);
}

test "playback callback clears output when idle or missing playback state" {
    var data = [_]f32{ 1.0, -1.0, 0.5, -0.5 };
    var output = ca.AudioBufferList{
        .mNumberBuffers = 1,
        .mBuffers = .{.{
            .mNumberChannels = 2,
            .mDataByteSize = @sizeOf(f32) * data.len,
            .mData = @ptrCast(&data[0]),
        }},
    };
    var input = ca.AudioBufferList{
        .mNumberBuffers = 0,
        .mBuffers = .{std.mem.zeroes(ca.AudioBuffer)},
    };
    var time = timestamp();

    try std.testing.expectEqual(ca.noErr, streamOutputCallback(
        ca.kAudioObjectUnknown,
        &time,
        &input,
        &time,
        &output,
        &time,
        null,
    ));
    try std.testing.expectEqualSlices(f32, &[_]f32{ 0.0, 0.0, 0.0, 0.0 }, &data);

    data = .{ 1.0, -1.0, 0.5, -0.5 };
    var source = [_]f32{0.25};
    var playback = testPlayback(source[0..], 1.0, false);
    try std.testing.expectEqual(ca.noErr, streamOutputCallback(
        ca.kAudioObjectUnknown,
        &time,
        &input,
        &time,
        &output,
        &time,
        &playback,
    ));
    try std.testing.expectEqualSlices(f32, &[_]f32{ 0.0, 0.0, 0.0, 0.0 }, &data);
}

test "playback callback duplicates mono samples to every output channel and clears underrun" {
    var source = [_]f32{ 0.25, -0.5 };
    var playback = testPlayback(source[0..], 1.0, true);
    var data = [_]f32{ 9.0, 9.0, 9.0, 9.0, 9.0, 9.0, 9.0, 9.0 };
    var output = ca.AudioBufferList{
        .mNumberBuffers = 1,
        .mBuffers = .{.{
            .mNumberChannels = 2,
            .mDataByteSize = @sizeOf(f32) * data.len,
            .mData = @ptrCast(&data[0]),
        }},
    };
    var input = ca.AudioBufferList{
        .mNumberBuffers = 0,
        .mBuffers = .{std.mem.zeroes(ca.AudioBuffer)},
    };
    var time = timestamp();

    try std.testing.expectEqual(ca.noErr, streamOutputCallback(
        ca.kAudioObjectUnknown,
        &time,
        &input,
        &time,
        &output,
        &time,
        &playback,
    ));

    try std.testing.expectEqualSlices(
        f32,
        &[_]f32{ 0.25, 0.25, -0.5, -0.5, 0.0, 0.0, 0.0, 0.0 },
        &data,
    );
    try std.testing.expect(!playback.isPlaying());
    try std.testing.expectEqual(@as(u64, 2), playback.position());
}

test "playback callback renders all buffers and preserves C interpolation semantics" {
    var source = [_]f32{ 0.0, 10.0, 20.0 };
    var playback = testPlayback(source[0..], 0.5, true);
    var left = [_]f32{ 99.0, 99.0, 99.0, 99.0 };
    var right = [_]f32{ 88.0, 88.0, 88.0, 88.0 };

    const TwoBufferList = extern struct {
        mNumberBuffers: u32,
        mBuffers: [2]ca.AudioBuffer,
    };
    var output_storage = TwoBufferList{
        .mNumberBuffers = 2,
        .mBuffers = .{
            .{
                .mNumberChannels = 1,
                .mDataByteSize = @sizeOf(f32) * left.len,
                .mData = @ptrCast(&left[0]),
            },
            .{
                .mNumberChannels = 1,
                .mDataByteSize = @sizeOf(f32) * right.len,
                .mData = @ptrCast(&right[0]),
            },
        },
    };
    const output: *ca.AudioBufferList = @ptrCast(&output_storage);
    var input = ca.AudioBufferList{
        .mNumberBuffers = 0,
        .mBuffers = .{std.mem.zeroes(ca.AudioBuffer)},
    };
    var time = timestamp();

    try std.testing.expectEqual(ca.noErr, streamOutputCallback(
        ca.kAudioObjectUnknown,
        &time,
        &input,
        &time,
        output,
        &time,
        &playback,
    ));

    const expected = [_]f32{ 0.0, 5.0, 10.0, 15.0 };
    try std.testing.expectEqualSlices(f32, &expected, &left);
    try std.testing.expectEqualSlices(f32, &expected, &right);
    try std.testing.expect(playback.isPlaying());
    try std.testing.expectEqual(@as(u64, 2), playback.position());
}

test "default output format query is guarded for machines without output hardware" {
    const format = defaultOutputFormat(std.testing.allocator) catch |err| {
        try std.testing.expect(hardwareUnavailable(err));
        return;
    };

    try std.testing.expect(format.sample_rate > 0.0);
    try std.testing.expect(format.channels_per_frame > 0);
}
