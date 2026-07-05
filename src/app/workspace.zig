//! Workspace/tab state.
//! Rewrite target; C reference: src_c/src/app/workspace.c

pub const Workspace = enum {
    live,
    recordings,
    trim,
    spectrum,
    band_lab,

    pub fn index(self: Workspace) usize {
        return @intFromEnum(self);
    }

    pub fn name(self: Workspace) []const u8 {
        return switch (self) {
            .live => "LIVE SPECTROGRAM",
            .recordings => "RECORDINGS",
            .trim => "TRIM",
            .spectrum => "WHOLE SPECTRUM",
            .band_lab => "BAND LAB",
        };
    }

    pub fn shortName(self: Workspace) []const u8 {
        return switch (self) {
            .live => "LIVE",
            .recordings => "RECS",
            .trim => "TRIM",
            .spectrum => "SPECTRUM",
            .band_lab => "BAND",
        };
    }

    pub fn offset(self: Workspace, delta: isize) Workspace {
        return workspaceOffset(self, delta);
    }
};

pub const order = [_]Workspace{
    .live,
    .recordings,
    .trim,
    .spectrum,
    .band_lab,
};

pub const count = order.len;

pub fn at(index: isize) Workspace {
    if (index < 0) return .live;
    const i: usize = @intCast(index);
    if (i >= order.len) return .live;
    return order[i];
}

pub fn workspaceOffset(workspace: Workspace, delta: isize) Workspace {
    const workspace_count: isize = @intCast(order.len);
    var index: isize = @as(isize, @intCast(workspace.index())) + delta;
    while (index < 0) index += workspace_count;
    return order[@intCast(@mod(index, workspace_count))];
}

test "workspace order and wrapping match the C API" {
    const std = @import("std");

    try std.testing.expectEqual(@as(usize, 5), count);
    try std.testing.expectEqual(Workspace.live, at(-1));
    try std.testing.expectEqual(Workspace.live, at(99));
    try std.testing.expectEqual(Workspace.recordings, Workspace.live.offset(1));
    try std.testing.expectEqual(Workspace.live, Workspace.band_lab.offset(1));
    try std.testing.expectEqual(Workspace.band_lab, Workspace.live.offset(-1));
    try std.testing.expectEqualStrings("RECORDINGS", Workspace.recordings.name());
    try std.testing.expectEqualStrings("BAND", Workspace.band_lab.shortName());
}
