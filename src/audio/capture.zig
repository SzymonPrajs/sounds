//! Core Audio HAL microphone capture.
//! Rewrite target; C reference: src_c/src/audio/capture.c

const std = @import("std");
const ca = @import("../apple/coreaudio.zig");
const ring_buffer = @import("ring_buffer.zig");

pub const maximum_input_channels: usize = 32;
pub const maximum_input_sample_rate: f64 = 512_000.0;
pub const minimum_scratch_frames: u32 = 4096;

pub const Error = std.mem.Allocator.Error || error{
    MissingInputCallback,
    MissingInputStream,
    NoDefaultInputDevice,
    NoInputStreams,
    InvalidInputSampleRate,
    InvalidInputChannelCount,
    UnsupportedInputFormat,
    CoreAudioGetDefaultInputDeviceFailed,
    CoreAudioGetInputStreamsSizeFailed,
    CoreAudioGetInputStreamsFailed,
    CoreAudioGetStreamVirtualFormatFailed,
    CoreAudioCreateIOProcFailed,
    CoreAudioStartFailed,
    CoreAudioStopFailed,
};

pub const InputCallback = *const fn (samples: []const f32, user_data: ?*anyopaque) void;

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

pub const Options = struct {
    callback: ?InputCallback = null,
    user_data: ?*anyopaque = null,
};

pub const InputStream = struct {
    allocator: std.mem.Allocator,
    device: ca.AudioDeviceID,
    callback_id: ?ca.AudioDeviceIOProcID,
    callback: InputCallback,
    user_data: ?*anyopaque,
    scratch: []f32,
    format_info: Format,
    started: bool,

    pub fn open(allocator: std.mem.Allocator, options: Options) Error!*InputStream {
        const callback = options.callback orelse return Error.MissingInputCallback;

        const stream = try allocator.create(InputStream);
        stream.* = .{
            .allocator = allocator,
            .device = ca.kAudioObjectUnknown,
            .callback_id = null,
            .callback = callback,
            .user_data = options.user_data,
            .scratch = &.{},
            .format_info = std.mem.zeroes(Format),
            .started = false,
        };
        errdefer stream.close();

        stream.device = try defaultInputDevice();
        const asbd = try firstInputStreamFormat(allocator, stream.device);
        try validateInputFormat(asbd);

        stream.format_info = Format.fromASBD(asbd);
        const scratch_capacity = inputBufferFrameCapacity(stream.device);
        stream.scratch = try allocator.alloc(f32, scratch_capacity);
        errdefer {
            allocator.free(stream.scratch);
            stream.scratch = &.{};
        }

        var callback_id: ?ca.AudioDeviceIOProcID = null;
        if (ca.AudioDeviceCreateIOProcID(
            stream.device,
            streamInputCallback,
            stream,
            &callback_id,
        ) != ca.noErr) {
            return Error.CoreAudioCreateIOProcFailed;
        }
        stream.callback_id = callback_id orelse return Error.CoreAudioCreateIOProcFailed;

        return stream;
    }

    pub fn openRingBuffer(
        allocator: std.mem.Allocator,
        ring: *ring_buffer.RingBuffer,
    ) Error!*InputStream {
        return InputStream.open(allocator, .{
            .callback = writeRingCallback,
            .user_data = ring,
        });
    }

    pub fn format(self: *const InputStream) Format {
        return self.format_info;
    }

    pub fn start(self: *InputStream) Error!void {
        if (self.started) return;

        const callback_id = self.callback_id orelse return Error.CoreAudioStartFailed;
        if (ca.AudioDeviceStart(self.device, callback_id) != ca.noErr) {
            return Error.CoreAudioStartFailed;
        }
        self.started = true;
    }

    pub fn stop(self: *InputStream) Error!void {
        if (!self.started) return;

        const callback_id = self.callback_id orelse return Error.CoreAudioStopFailed;
        if (ca.AudioDeviceStop(self.device, callback_id) != ca.noErr) {
            return Error.CoreAudioStopFailed;
        }
        self.started = false;
    }

    pub fn close(self: *InputStream) void {
        if (self.started) {
            if (self.callback_id) |callback_id| {
                _ = ca.AudioDeviceStop(self.device, callback_id);
            }
        }

        if (self.callback_id) |callback_id| {
            _ = ca.AudioDeviceDestroyIOProcID(self.device, callback_id);
        }

        self.allocator.free(self.scratch);
        self.allocator.destroy(self);
    }
};

pub fn defaultInputFormat(allocator: std.mem.Allocator) Error!Format {
    const device = try defaultInputDevice();
    return Format.fromASBD(try firstInputStreamFormat(allocator, device));
}

pub fn open(allocator: std.mem.Allocator, options: Options) Error!*InputStream {
    return InputStream.open(allocator, options);
}

pub fn openRingBuffer(
    allocator: std.mem.Allocator,
    ring: *ring_buffer.RingBuffer,
) Error!*InputStream {
    return InputStream.openRingBuffer(allocator, ring);
}

pub fn start(stream: ?*InputStream) Error!void {
    const live_stream = stream orelse return Error.MissingInputStream;
    return live_stream.start();
}

pub fn stop(stream: ?*InputStream) Error!void {
    const live_stream = stream orelse return Error.MissingInputStream;
    return live_stream.stop();
}

fn defaultInputDevice() Error!ca.AudioDeviceID {
    var device: ca.AudioDeviceID = ca.kAudioObjectUnknown;
    var size: u32 = @sizeOf(ca.AudioDeviceID);
    const address = ca.AudioObjectPropertyAddress{
        .mSelector = ca.kAudioHardwarePropertyDefaultInputDevice,
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
        return Error.CoreAudioGetDefaultInputDeviceFailed;
    }

    if (device == ca.kAudioObjectUnknown) return Error.NoDefaultInputDevice;
    return device;
}

fn firstInputStreamFormat(
    allocator: std.mem.Allocator,
    device: ca.AudioDeviceID,
) Error!ca.AudioStreamBasicDescription {
    const streams_address = ca.AudioObjectPropertyAddress{
        .mSelector = ca.kAudioDevicePropertyStreams,
        .mScope = ca.kAudioDevicePropertyScopeInput,
        .mElement = ca.kAudioObjectPropertyElementMain,
    };

    var size: u32 = 0;
    if (ca.AudioObjectGetPropertyDataSize(device, &streams_address, 0, null, &size) != ca.noErr) {
        return Error.CoreAudioGetInputStreamsSizeFailed;
    }

    if (size < @sizeOf(ca.AudioStreamID) or (size % @sizeOf(ca.AudioStreamID)) != 0) {
        return Error.NoInputStreams;
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
        return Error.CoreAudioGetInputStreamsFailed;
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

fn validateInputFormat(format: ca.AudioStreamBasicDescription) Error!void {
    if (!std.math.isFinite(format.mSampleRate) or
        format.mSampleRate <= 0.0 or
        format.mSampleRate > maximum_input_sample_rate)
    {
        return Error.InvalidInputSampleRate;
    }

    if (format.mChannelsPerFrame == 0 or format.mChannelsPerFrame > maximum_input_channels) {
        return Error.InvalidInputChannelCount;
    }

    if (format.mFormatID != ca.kAudioFormatLinearPCM or
        (format.mFormatFlags & ca.kAudioFormatFlagIsFloat) == 0 or
        format.mBitsPerChannel != 32)
    {
        return Error.UnsupportedInputFormat;
    }
}

fn inputBufferFrameCapacity(device: ca.AudioDeviceID) u32 {
    var frames: u32 = 0;
    var size: u32 = @sizeOf(u32);
    const address = ca.AudioObjectPropertyAddress{
        .mSelector = ca.kAudioDevicePropertyBufferFrameSize,
        .mScope = ca.kAudioObjectPropertyScopeGlobal,
        .mElement = ca.kAudioObjectPropertyElementMain,
    };

    if (ca.AudioObjectGetPropertyData(device, &address, 0, null, &size, &frames) != ca.noErr or
        frames == 0)
    {
        frames = minimum_scratch_frames;
    }

    return @max(frames, minimum_scratch_frames);
}

const InputChunk = struct {
    interleaved: [*]align(1) const f32,
    frames: usize,
    channels: usize,
};

fn inputChunkFromBuffers(input_data: ?*const ca.AudioBufferList) ?InputChunk {
    const buffers = input_data orelse return null;
    if (buffers.mNumberBuffers == 0) return null;

    const buffer = &buffers.mBuffers[0];
    const data = buffer.mData orelse return null;
    if (buffer.mDataByteSize == 0) return null;

    const channels: usize = if (buffer.mNumberChannels == 0) 1 else buffer.mNumberChannels;
    if (channels > maximum_input_channels) return null;

    const frame_bytes = @sizeOf(f32) * channels;
    const frames = @as(usize, buffer.mDataByteSize) / frame_bytes;
    if (frames == 0) return null;

    return .{
        .interleaved = @ptrCast(data),
        .frames = frames,
        .channels = channels,
    };
}

fn copyFirstChannel(destination: []f32, chunk: InputChunk) void {
    if (chunk.channels == 1) {
        @memcpy(destination, chunk.interleaved[0..destination.len]);
        return;
    }

    var frame: usize = 0;
    const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
    const Vec = @Vector(lanes, f32);
    while (frame + lanes <= destination.len) : (frame += lanes) {
        var values: Vec = undefined;
        inline for (0..lanes) |lane| {
            values[lane] = chunk.interleaved[(frame + lane) * chunk.channels];
        }
        destination[frame..][0..lanes].* = values;
    }
    while (frame < destination.len) : (frame += 1) {
        destination[frame] = chunk.interleaved[frame * chunk.channels];
    }
}

fn alignedSamples(samples: [*]align(1) const f32) ?[*]const f32 {
    if ((@intFromPtr(samples) % @alignOf(f32)) != 0) return null;
    return @alignCast(samples);
}

fn streamInputCallback(
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
    _ = input_time;
    _ = output_data;
    _ = output_time;

    const stream: *InputStream = @ptrCast(@alignCast(client_data orelse return ca.noErr));
    const chunk = inputChunkFromBuffers(input_data) orelse return ca.noErr;

    if (chunk.channels == 1) {
        if (alignedSamples(chunk.interleaved)) |samples| {
            stream.callback(samples[0..chunk.frames], stream.user_data);
            return ca.noErr;
        }

        const frames = @min(chunk.frames, stream.scratch.len);
        copyFirstChannel(stream.scratch[0..frames], chunk);
        stream.callback(stream.scratch[0..frames], stream.user_data);
        return ca.noErr;
    }

    const frames = @min(chunk.frames, stream.scratch.len);
    copyFirstChannel(stream.scratch[0..frames], chunk);
    stream.callback(stream.scratch[0..frames], stream.user_data);
    return ca.noErr;
}

fn writeRingCallback(samples: []const f32, user_data: ?*anyopaque) void {
    const ring: *ring_buffer.RingBuffer = @ptrCast(@alignCast(user_data orelse return));
    ring.write(samples);
}

fn validInputASBD() ca.AudioStreamBasicDescription {
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
        Error.NoDefaultInputDevice,
        Error.NoInputStreams,
        Error.CoreAudioGetDefaultInputDeviceFailed,
        Error.CoreAudioGetInputStreamsSizeFailed,
        Error.CoreAudioGetInputStreamsFailed,
        Error.CoreAudioGetStreamVirtualFormatFailed,
        Error.UnsupportedInputFormat,
        => true,
        else => false,
    };
}

const TestSink = struct {
    samples: [8]f32 = .{0.0} ** 8,
    count: usize = 0,

    fn callback(samples: []const f32, user_data: ?*anyopaque) void {
        const self: *TestSink = @ptrCast(@alignCast(user_data.?));
        self.count = @min(samples.len, self.samples.len);
        @memcpy(self.samples[0..self.count], samples[0..self.count]);
    }
};

fn timestamp() ca.AudioTimeStamp {
    return std.mem.zeroes(ca.AudioTimeStamp);
}

test "input format copies and validates Core Audio ASBD fields" {
    const asbd = validInputASBD();
    try validateInputFormat(asbd);

    const format = Format.fromASBD(asbd);
    try std.testing.expectEqual(@as(f64, 48_000.0), format.sample_rate);
    try std.testing.expectEqual(ca.kAudioFormatLinearPCM, format.format_id);
    try std.testing.expectEqual(ca.kAudioFormatFlagIsFloat | ca.kAudioFormatFlagIsPacked, format.format_flags);
    try std.testing.expectEqual(@as(u32, 32), format.bits_per_channel);
    try std.testing.expectEqual(@as(u32, 1), format.channels_per_frame);
    try std.testing.expectEqual(@as(u32, 4), format.bytes_per_frame);

    var bad_rate = asbd;
    bad_rate.mSampleRate = -1.0;
    try std.testing.expectError(Error.InvalidInputSampleRate, validateInputFormat(bad_rate));

    var bad_channels = asbd;
    bad_channels.mChannelsPerFrame = @intCast(maximum_input_channels + 1);
    try std.testing.expectError(Error.InvalidInputChannelCount, validateInputFormat(bad_channels));

    var bad_format = asbd;
    bad_format.mFormatID = ca.fourcc("aac ");
    try std.testing.expectError(Error.UnsupportedInputFormat, validateInputFormat(bad_format));
}

test "input callback forwards mono float buffers unchanged" {
    var sink = TestSink{};
    var scratch: [4]f32 = undefined;
    var stream = InputStream{
        .allocator = std.testing.allocator,
        .device = ca.kAudioObjectUnknown,
        .callback_id = null,
        .callback = TestSink.callback,
        .user_data = &sink,
        .scratch = scratch[0..],
        .format_info = Format.fromASBD(validInputASBD()),
        .started = false,
    };

    var samples = [_]f32{ 0.25, -0.5, 0.75 };
    var input = ca.AudioBufferList{
        .mNumberBuffers = 1,
        .mBuffers = .{.{
            .mNumberChannels = 1,
            .mDataByteSize = @sizeOf(f32) * samples.len,
            .mData = @ptrCast(&samples[0]),
        }},
    };
    var output = ca.AudioBufferList{
        .mNumberBuffers = 0,
        .mBuffers = .{std.mem.zeroes(ca.AudioBuffer)},
    };
    var time = timestamp();

    try std.testing.expectEqual(ca.noErr, streamInputCallback(
        ca.kAudioObjectUnknown,
        &time,
        &input,
        &time,
        &output,
        &time,
        &stream,
    ));
    try std.testing.expectEqual(@as(usize, samples.len), sink.count);
    try std.testing.expectEqualSlices(f32, &samples, sink.samples[0..sink.count]);
}

test "input callback deinterleaves the first channel and truncates to scratch" {
    var sink = TestSink{};
    var scratch: [2]f32 = undefined;
    var stream = InputStream{
        .allocator = std.testing.allocator,
        .device = ca.kAudioObjectUnknown,
        .callback_id = null,
        .callback = TestSink.callback,
        .user_data = &sink,
        .scratch = scratch[0..],
        .format_info = Format.fromASBD(validInputASBD()),
        .started = false,
    };

    var interleaved = [_]f32{ 1.0, 10.0, 2.0, 20.0, 3.0, 30.0 };
    var input = ca.AudioBufferList{
        .mNumberBuffers = 1,
        .mBuffers = .{.{
            .mNumberChannels = 2,
            .mDataByteSize = @sizeOf(f32) * interleaved.len,
            .mData = @ptrCast(&interleaved[0]),
        }},
    };
    var output = ca.AudioBufferList{
        .mNumberBuffers = 0,
        .mBuffers = .{std.mem.zeroes(ca.AudioBuffer)},
    };
    var time = timestamp();

    try std.testing.expectEqual(ca.noErr, streamInputCallback(
        ca.kAudioObjectUnknown,
        &time,
        &input,
        &time,
        &output,
        &time,
        &stream,
    ));
    try std.testing.expectEqual(@as(usize, 2), sink.count);
    try std.testing.expectEqualSlices(f32, &[_]f32{ 1.0, 2.0 }, sink.samples[0..sink.count]);
}

test "first-channel deinterleave handles vector chunks and scalar tail" {
    const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
    const frames = lanes + 1;
    var interleaved: [frames * 2]f32 = undefined;
    for (0..frames) |frame| {
        interleaved[frame * 2] = @floatFromInt(frame + 1);
        interleaved[frame * 2 + 1] = -99.0;
    }

    var out: [frames]f32 = undefined;
    copyFirstChannel(&out, .{
        .interleaved = @ptrCast(&interleaved[0]),
        .frames = frames,
        .channels = 2,
    });

    for (&out, 0..) |sample, frame| {
        try std.testing.expectEqual(@as(f32, @floatFromInt(frame + 1)), sample);
    }
}

test "input callback ignores empty or unsupported buffers" {
    var sink = TestSink{};
    var scratch: [2]f32 = undefined;
    var stream = InputStream{
        .allocator = std.testing.allocator,
        .device = ca.kAudioObjectUnknown,
        .callback_id = null,
        .callback = TestSink.callback,
        .user_data = &sink,
        .scratch = scratch[0..],
        .format_info = Format.fromASBD(validInputASBD()),
        .started = false,
    };
    var input = ca.AudioBufferList{
        .mNumberBuffers = 0,
        .mBuffers = .{std.mem.zeroes(ca.AudioBuffer)},
    };
    var output = ca.AudioBufferList{
        .mNumberBuffers = 0,
        .mBuffers = .{std.mem.zeroes(ca.AudioBuffer)},
    };
    var time = timestamp();

    try std.testing.expectEqual(ca.noErr, streamInputCallback(
        ca.kAudioObjectUnknown,
        &time,
        &input,
        &time,
        &output,
        &time,
        &stream,
    ));
    try std.testing.expectEqual(@as(usize, 0), sink.count);
}

test "ring-buffer input callback writes mono float samples" {
    var ring = try ring_buffer.RingBuffer.init(std.testing.allocator, 4);
    defer ring.deinit();

    const samples = [_]f32{ -1.0, -0.25, 0.25, 1.0, 2.0 };
    writeRingCallback(&samples, &ring);

    var out: [4]f32 = undefined;
    try std.testing.expectEqual(@as(usize, 4), ring.readLatest(out[0..]));
    try std.testing.expectEqualSlices(f32, &[_]f32{ -0.25, 0.25, 1.0, 2.0 }, &out);
}

test "input stream nullable wrappers reject missing stream" {
    try std.testing.expectError(Error.MissingInputStream, start(null));
    try std.testing.expectError(Error.MissingInputStream, stop(null));
}

test "default input format query is guarded for machines without input hardware" {
    const format = defaultInputFormat(std.testing.allocator) catch |err| {
        try std.testing.expect(hardwareUnavailable(err));
        return;
    };

    try std.testing.expect(format.sample_rate > 0.0);
    try std.testing.expect(format.channels_per_frame > 0);
}
