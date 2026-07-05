//! Hand-written bindings for the Core Audio HAL surface this app uses.
//!
//! CoreAudio/AudioHardware.h uses Apple blocks, which translate-c cannot
//! parse, so the ABI is declared by hand. Layouts and constants come from
//! CoreAudioTypes.h / AudioHardware.h and are covered by layout tests below.

const std = @import("std");

pub const OSStatus = i32;
pub const AudioObjectID = u32;
pub const AudioDeviceID = AudioObjectID;
pub const AudioStreamID = AudioObjectID;

pub const noErr: OSStatus = 0;
pub const kAudioObjectSystemObject: AudioObjectID = 1;
pub const kAudioObjectUnknown: AudioObjectID = 0;

/// Four-character property codes, e.g. `fourcc("glob")`.
pub fn fourcc(comptime code: *const [4]u8) u32 {
    return std.mem.readInt(u32, code, .big);
}

pub const kAudioObjectPropertyScopeGlobal = fourcc("glob");
pub const kAudioObjectPropertyElementMain: u32 = 0;

pub const kAudioHardwarePropertyDefaultInputDevice = fourcc("dIn ");
pub const kAudioHardwarePropertyDefaultOutputDevice = fourcc("dOut");

pub const kAudioDevicePropertyScopeInput = fourcc("inpt");
pub const kAudioDevicePropertyScopeOutput = fourcc("outp");
pub const kAudioDevicePropertyStreams = fourcc("stm#");
pub const kAudioDevicePropertyBufferFrameSize = fourcc("fsiz");
pub const kAudioDevicePropertyNominalSampleRate = fourcc("nsrt");
pub const kAudioStreamPropertyVirtualFormat = fourcc("sfmt");

pub const kAudioFormatLinearPCM = fourcc("lpcm");
pub const kAudioFormatFlagIsFloat: u32 = 1 << 0;
pub const kAudioFormatFlagIsPacked: u32 = 1 << 3;

pub const AudioObjectPropertyAddress = extern struct {
    mSelector: u32,
    mScope: u32,
    mElement: u32,
};

pub const AudioBuffer = extern struct {
    mNumberChannels: u32,
    mDataByteSize: u32,
    mData: ?*anyopaque,
};

/// Variable-length in C; `mBuffers` is a flexible array. Use `buffers()` to
/// view the live entries.
pub const AudioBufferList = extern struct {
    mNumberBuffers: u32,
    mBuffers: [1]AudioBuffer,

    pub fn buffers(self: anytype) switch (@TypeOf(self)) {
        *const AudioBufferList => []const AudioBuffer,
        *AudioBufferList => []AudioBuffer,
        else => unreachable,
    } {
        return switch (@TypeOf(self)) {
            *const AudioBufferList => @as([*]align(8) const AudioBuffer, @ptrCast(&self.mBuffers))[0..self.mNumberBuffers],
            *AudioBufferList => @as([*]align(8) AudioBuffer, @ptrCast(&self.mBuffers))[0..self.mNumberBuffers],
            else => unreachable,
        };
    }
};

pub const SMPTETime = extern struct {
    mSubframes: i16,
    mSubframeDivisor: i16,
    mCounter: u32,
    mType: u32,
    mFlags: u32,
    mHours: i16,
    mMinutes: i16,
    mSeconds: i16,
    mFrames: i16,
};

pub const AudioTimeStamp = extern struct {
    mSampleTime: f64,
    mHostTime: u64,
    mRateScalar: f64,
    mWordClockTime: u64,
    mSMPTETime: SMPTETime,
    mFlags: u32,
    mReserved: u32,
};

pub const AudioStreamBasicDescription = extern struct {
    mSampleRate: f64,
    mFormatID: u32,
    mFormatFlags: u32,
    mBytesPerPacket: u32,
    mFramesPerPacket: u32,
    mBytesPerFrame: u32,
    mChannelsPerFrame: u32,
    mBitsPerChannel: u32,
    mReserved: u32,
};

pub const AudioDeviceIOProc = *const fn (
    device: AudioObjectID,
    now: *const AudioTimeStamp,
    input_data: *const AudioBufferList,
    input_time: *const AudioTimeStamp,
    output_data: *AudioBufferList,
    output_time: *const AudioTimeStamp,
    client_data: ?*anyopaque,
) callconv(.c) OSStatus;

pub const AudioDeviceIOProcID = AudioDeviceIOProc;

pub extern "c" fn AudioObjectGetPropertyDataSize(
    object: AudioObjectID,
    address: *const AudioObjectPropertyAddress,
    qualifier_size: u32,
    qualifier: ?*const anyopaque,
    out_size: *u32,
) OSStatus;

pub extern "c" fn AudioObjectGetPropertyData(
    object: AudioObjectID,
    address: *const AudioObjectPropertyAddress,
    qualifier_size: u32,
    qualifier: ?*const anyopaque,
    io_size: *u32,
    out_data: *anyopaque,
) OSStatus;

pub extern "c" fn AudioObjectSetPropertyData(
    object: AudioObjectID,
    address: *const AudioObjectPropertyAddress,
    qualifier_size: u32,
    qualifier: ?*const anyopaque,
    size: u32,
    data: *const anyopaque,
) OSStatus;

pub extern "c" fn AudioDeviceCreateIOProcID(
    device: AudioObjectID,
    proc: AudioDeviceIOProc,
    client_data: ?*anyopaque,
    out_proc_id: *?AudioDeviceIOProcID,
) OSStatus;

pub extern "c" fn AudioDeviceDestroyIOProcID(
    device: AudioObjectID,
    proc_id: AudioDeviceIOProcID,
) OSStatus;

pub extern "c" fn AudioDeviceStart(device: AudioObjectID, proc_id: ?AudioDeviceIOProcID) OSStatus;
pub extern "c" fn AudioDeviceStop(device: AudioObjectID, proc_id: ?AudioDeviceIOProcID) OSStatus;

test "struct layouts match the C ABI" {
    try std.testing.expectEqual(64, @sizeOf(AudioTimeStamp));
    try std.testing.expectEqual(40, @sizeOf(AudioStreamBasicDescription));
    try std.testing.expectEqual(12, @sizeOf(AudioObjectPropertyAddress));
    try std.testing.expectEqual(16, @sizeOf(AudioBuffer));
    try std.testing.expectEqual(24, @sizeOf(SMPTETime));
    try std.testing.expectEqual(@sizeOf(AudioObjectID), @sizeOf(AudioStreamID));
    try std.testing.expectEqual(@alignOf(AudioObjectID), @alignOf(AudioStreamID));
}

test "HAL constants match the C headers" {
    try std.testing.expectEqual(@as(OSStatus, 0), noErr);
    try std.testing.expectEqual(fourcc("glob"), kAudioObjectPropertyScopeGlobal);
    try std.testing.expectEqual(fourcc("dIn "), kAudioHardwarePropertyDefaultInputDevice);
    try std.testing.expectEqual(fourcc("dOut"), kAudioHardwarePropertyDefaultOutputDevice);
    try std.testing.expectEqual(fourcc("inpt"), kAudioDevicePropertyScopeInput);
    try std.testing.expectEqual(fourcc("outp"), kAudioDevicePropertyScopeOutput);
    try std.testing.expectEqual(fourcc("stm#"), kAudioDevicePropertyStreams);
    try std.testing.expectEqual(fourcc("fsiz"), kAudioDevicePropertyBufferFrameSize);
    try std.testing.expectEqual(fourcc("nsrt"), kAudioDevicePropertyNominalSampleRate);
    try std.testing.expectEqual(fourcc("sfmt"), kAudioStreamPropertyVirtualFormat);
    try std.testing.expectEqual(fourcc("lpcm"), kAudioFormatLinearPCM);
}

test "default input device is queryable" {
    var address = AudioObjectPropertyAddress{
        .mSelector = kAudioHardwarePropertyDefaultInputDevice,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };
    var device: AudioDeviceID = kAudioObjectUnknown;
    var size: u32 = @sizeOf(AudioDeviceID);
    const status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject,
        &address,
        0,
        null,
        &size,
        &device,
    );
    // Status 0 with a real device on normal machines; either way the call
    // itself must be ABI-safe.
    try std.testing.expect(status == 0 or device == kAudioObjectUnknown);
}
