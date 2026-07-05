//! Entry point. Wires capture, analysis, UI, and recording together.
//! Rewrite target for src_c/src/main.c — for now a linkage-proving shell:
//! opens the SDL window and runs an empty frame loop.

const std = @import("std");
const sdl = @import("c.zig").sdl;
const ui = @import("ui/ui.zig");

pub fn main() !void {
    if (!sdl.SDL_Init(sdl.SDL_INIT_VIDEO)) {
        std.log.err("SDL_Init failed: {s}", .{sdl.SDL_GetError()});
        return error.SdlInit;
    }
    defer sdl.SDL_Quit();

    const window = sdl.SDL_CreateWindow(
        "sounds",
        1280,
        800,
        sdl.SDL_WINDOW_HIGH_PIXEL_DENSITY | sdl.SDL_WINDOW_RESIZABLE,
    ) orelse {
        std.log.err("SDL_CreateWindow failed: {s}", .{sdl.SDL_GetError()});
        return error.SdlCreateWindow;
    };
    defer sdl.SDL_DestroyWindow(window);

    var app_ui = try ui.Ui.init(std.heap.c_allocator, window);
    defer app_ui.deinit();

    var waveform: [1024]f32 = undefined;
    fillSmokeWaveform(&waveform);

    var snapshot = ui.Snapshot{
        .waveform = &waveform,
    };

    var running = true;
    while (running) {
        app_ui.beginFrame();

        var event: sdl.SDL_Event = undefined;
        while (sdl.SDL_PollEvent(&event)) {
            app_ui.handleEvent(&event, &snapshot);
        }

        const events = try app_ui.render(&snapshot);
        applySmokeEvents(&snapshot, events, &running);
        if (snapshot.recording_enabled) snapshot.recording_seconds += 1.0 / 60.0;

        sdl.SDL_Delay(16);
    }
}

fn fillSmokeWaveform(samples: []f32) void {
    for (samples, 0..) |*sample, index| {
        const phase = @as(f64, @floatFromInt(index)) / @as(f64, @floatFromInt(samples.len));
        const slow = @sin(phase * std.math.pi * 2.0 * 3.0) * 0.70;
        const fast = @sin(phase * std.math.pi * 2.0 * 37.0) * 0.12;
        sample.* = @floatCast(slow + fast);
    }
}

fn applySmokeEvents(snapshot: *ui.Snapshot, events: ui.PendingEvents, running: *bool) void {
    if (events.quit) running.* = false;
    if (events.workspace) |workspace| snapshot.workspace = workspace;
    if (events.mode) |mode| snapshot.mode = mode;
    if (events.frequency_band) |band| snapshot.frequency_band = band;
    if (events.colormap) |map| snapshot.colormap = map;
    if (events.toggle_sst) snapshot.sst_enabled = !snapshot.sst_enabled;
    if (events.toggle_recording) {
        snapshot.recording_enabled = !snapshot.recording_enabled;
        snapshot.recording_seconds = 0.0;
    }
    if (events.toggle_playback) snapshot.playback_enabled = !snapshot.playback_enabled;
    if (events.custom_low_hz) |hz| {
        snapshot.custom_range.min_hz = hz;
        snapshot.frequency_band = .custom;
    }
    if (events.custom_high_hz) |hz| {
        snapshot.custom_range.max_hz = hz;
        snapshot.frequency_band = .custom;
    }
    if (events.delete_recording) snapshot.recording_delete_pending = !snapshot.recording_delete_pending;
    if (events.cancel_recording_delete) snapshot.recording_delete_pending = false;

    const range = snapshot.frequency_band.limits(.{ .min_hz = 10.0, .max_hz = 24_000.0 }, snapshot.custom_range);
    snapshot.min_hz = range.min_hz;
    snapshot.max_hz = range.max_hz;
}
