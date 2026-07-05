const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    linkPlatform(exe_mod);

    const exe = b.addExecutable(.{
        .name = "sounds",
        .root_module = exe_mod,
    });
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_cmd.addArgs(args);
    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    // `zig build test` compiles and runs every `test` block reachable from
    // src/all_tests.zig, which imports the whole source tree.
    const test_mod = b.createModule(.{
        .root_source_file = b.path("src/all_tests.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    linkPlatform(test_mod);

    const unit_tests = b.addTest(.{ .root_module = test_mod });
    const run_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_tests.step);
}

fn linkPlatform(mod: *std.Build.Module) void {
    // SDL3 resolves through pkg-config (homebrew); frameworks through the SDK.
    mod.linkSystemLibrary("SDL3", .{});
    mod.linkFramework("CoreAudio", .{});
    mod.linkFramework("Accelerate", .{});
}
