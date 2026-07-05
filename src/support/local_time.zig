//! Local timezone formatting helpers.
//! libc is used because Zig's standard library does not format local timezones.

const std = @import("std");

pub const Error = error{
    TimeUnavailable,
    FormatFailed,
};

const Libc = struct {
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

pub fn formatNow(buffer: []u8, pattern: [*:0]const u8) Error![]const u8 {
    const now = Libc.time(null);
    if (now == @as(std.c.time_t, -1)) return Error.TimeUnavailable;
    return formatTimestamp(now, buffer, pattern);
}

pub fn formatUnixSeconds(seconds: i128, buffer: []u8, pattern: [*:0]const u8) Error![]const u8 {
    const timer: std.c.time_t = @intCast(seconds);
    return formatTimestamp(timer, buffer, pattern);
}

fn formatTimestamp(timer: std.c.time_t, buffer: []u8, pattern: [*:0]const u8) Error![]const u8 {
    var local: Libc.Tm = undefined;
    if (Libc.localtime_r(&timer, &local) == null) return Error.TimeUnavailable;

    const length = Libc.strftime(buffer.ptr, buffer.len, pattern, &local);
    if (length == 0) return Error.FormatFailed;
    return buffer[0..length];
}
