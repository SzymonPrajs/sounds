//! Entry point. Wires capture, analysis, UI, and recording together.
//! Rewrite target for src_c/src/main.c — for now a linkage-proving shell:
//! opens the SDL window and runs an empty frame loop.

const std = @import("std");
const sdl = @import("c.zig").sdl;

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

    var running = true;
    while (running) {
        var event: sdl.SDL_Event = undefined;
        while (sdl.SDL_PollEvent(&event)) {
            switch (event.type) {
                sdl.SDL_EVENT_QUIT => running = false,
                sdl.SDL_EVENT_KEY_DOWN => if (event.key.key == sdl.SDLK_Q) {
                    running = false;
                },
                else => {},
            }
        }
        sdl.SDL_Delay(16);
    }
}
