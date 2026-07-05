//! Test root: imports the whole tree so `zig build test` compiles and runs
//! every `test` block. Add new modules here as they are created.

const std = @import("std");

comptime {
    _ = @import("apple/vdsp.zig");
    _ = @import("apple/coreaudio.zig");

    _ = @import("support/colormap.zig");
    _ = @import("support/frequency_band.zig");

    _ = @import("audio/ring_buffer.zig");
    _ = @import("audio/capture.zig");
    _ = @import("audio/playback.zig");

    _ = @import("analysis/engine.zig");
    _ = @import("analysis/spectrum.zig");
    _ = @import("analysis/wavelet.zig");
    _ = @import("analysis/transient.zig");
    _ = @import("analysis/tonal.zig");
    _ = @import("analysis/spectral_mode.zig");
    _ = @import("analysis/offline_spectrum.zig");
    _ = @import("analysis/band_render.zig");

    _ = @import("app/settings.zig");
    _ = @import("app/clip.zig");
    _ = @import("app/recording.zig");
    _ = @import("app/workspace.zig");
    _ = @import("app/workbench.zig");

    _ = @import("ui/ui.zig");

    _ = @import("main.zig");
}
