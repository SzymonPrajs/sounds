//! Single import point for SDL3. Everything SDL goes through `c.sdl`.
//! Apple frameworks are NOT imported here — their headers do not survive
//! translate-c; use the hand-written bindings in src/apple/ instead.

pub const sdl = @cImport({
    @cInclude("SDL3/SDL.h");
});
