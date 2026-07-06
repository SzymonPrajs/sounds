//! Entry point that wires capture, analysis, UI, playback, and recording.

const std = @import("std");
const sdl = @import("c.zig").sdl;

const analysis = @import("analysis/engine.zig");
const capture = @import("audio/capture.zig");
const playback = @import("audio/playback.zig");
const ring_buffer = @import("audio/ring_buffer.zig");
const settings_mod = @import("app/settings.zig");
const recording = @import("app/recording.zig");
const workbench_mod = @import("app/workbench.zig");
const ui = @import("ui/ui.zig");

const app_name = "Sounds";
const initial_window_width = 1100;
const initial_window_height = 640;
const minimum_window_width = 480;
const minimum_window_height = 320;
const waveform_seconds = 0.08;
const raw_recording_seconds = 30.0;
const capture_buffer_seconds = 2.0;
const spectrogram_columns_per_second = 240.0;
const minimum_waveform_samples = 1024;
const maximum_audio_sample_rate = 512_000.0;
const maximum_audio_channels = 32;

pub fn main() !void {
    var app = try App.init(std.heap.c_allocator);
    defer app.deinit();
    try app.run();
}

const RecordingSnapshot = struct {
    buffer: []f32 = &.{},
    count: usize = 0,

    fn deinit(self: RecordingSnapshot, allocator: std.mem.Allocator) void {
        allocator.free(self.buffer);
    }

    fn samples(self: RecordingSnapshot) []const f32 {
        return self.buffer[0..self.count];
    }
};

const App = struct {
    allocator: std.mem.Allocator,
    window: *sdl.SDL_Window,
    app_ui: ui.Ui,
    ring: *ring_buffer.RingBuffer,
    stream: *capture.InputStream,
    engine: analysis.Engine,
    player: *playback.Playback,
    waveform: []f32,
    workbench: workbench_mod.Workbench,
    settings: settings_mod.Settings,
    sample_rate: f64,
    recording_enabled: bool = false,
    recording_started_at: u64 = 0,
    running: bool = true,
    app_workspace: ui.Workspace = .live,

    fn init(allocator: std.mem.Allocator) !App {
        if (!sdl.SDL_Init(sdl.SDL_INIT_VIDEO)) {
            std.log.err("SDL_Init failed: {s}", .{sdl.SDL_GetError()});
            return error.SdlInit;
        }
        errdefer sdl.SDL_Quit();

        const window = sdl.SDL_CreateWindow(
            app_name,
            initial_window_width,
            initial_window_height,
            sdl.SDL_WINDOW_HIGH_PIXEL_DENSITY | sdl.SDL_WINDOW_RESIZABLE,
        ) orelse {
            std.log.err("SDL_CreateWindow failed: {s}", .{sdl.SDL_GetError()});
            return error.SdlCreateWindow;
        };
        errdefer sdl.SDL_DestroyWindow(window);
        _ = sdl.SDL_SetWindowMinimumSize(window, minimum_window_width, minimum_window_height);

        var app_ui = try ui.Ui.init(allocator, window);
        errdefer app_ui.deinit();

        var workbench = workbench_mod.Workbench.init(allocator);
        errdefer workbench.deinit();
        try workbench.startRecordingScan();

        var settings = try settings_mod.load();
        settings = settings.sanitize();

        const input_format = try capture.defaultInputFormat(allocator);
        if (!inputFormatSupported(input_format)) return error.InvalidInputFormat;

        const ring_capacity = ringCapacityForRate(input_format.sample_rate);
        const ring = try allocator.create(ring_buffer.RingBuffer);
        errdefer allocator.destroy(ring);
        ring.* = try ring_buffer.RingBuffer.init(allocator, @intCast(ring_capacity));
        errdefer ring.deinit();

        const stream = try capture.InputStream.openRingBuffer(allocator, ring);
        errdefer stream.close();

        var engine = try analysis.Engine.init(allocator, input_format.sample_rate, spectrogram_columns_per_second);
        errdefer engine.deinit();
        engine.setMode(settings.mode);
        try engine.setFrequencyBand(settings.frequency_band, settings.custom_range);

        const player = try playback.Playback.open(allocator);
        errdefer player.close();

        const waveform_capacity = waveformCapacityForRate(input_format.sample_rate);
        const waveform = try allocator.alloc(f32, @intCast(waveform_capacity));
        errdefer allocator.free(waveform);

        try stream.start();
        errdefer stream.stop() catch {};

        return .{
            .allocator = allocator,
            .window = window,
            .app_ui = app_ui,
            .ring = ring,
            .stream = stream,
            .engine = engine,
            .player = player,
            .waveform = waveform,
            .workbench = workbench,
            .settings = settings,
            .sample_rate = input_format.sample_rate,
        };
    }

    fn deinit(self: *App) void {
        self.stream.stop() catch {};
        self.player.stop() catch {};
        if (self.recording_enabled) {
            self.stopRecording(false) catch |err| {
                std.log.err("could not save active recording: {}", .{err});
            };
        }
        self.allocator.free(self.waveform);
        self.player.close();
        self.engine.deinit();
        self.stream.close();
        self.ring.deinit();
        self.allocator.destroy(self.ring);
        self.workbench.deinit();
        self.app_ui.deinit();
        sdl.SDL_DestroyWindow(self.window);
        sdl.SDL_Quit();
        self.* = undefined;
    }

    fn run(self: *App) !void {
        // SOUNDS_PERF=1 logs any frame slower than 25ms with a phase breakdown.
        const perf_log = std.c.getenv("SOUNDS_PERF") != null;
        while (self.running) {
            const t0 = sdl.SDL_GetTicksNS();
            self.app_ui.beginFrame();
            var frame_snapshot = try self.buildSnapshot();
            const t1 = sdl.SDL_GetTicksNS();

            var event: sdl.SDL_Event = undefined;
            while (sdl.SDL_PollEvent(&event)) {
                self.app_ui.handleEvent(&event, &frame_snapshot);
            }
            const t2 = sdl.SDL_GetTicksNS();

            const events = try self.app_ui.render(&frame_snapshot);
            const t3 = sdl.SDL_GetTicksNS();
            try self.applyEvents(events);
            const t4 = sdl.SDL_GetTicksNS();
            if (perf_log and t4 - t0 > 25_000_000) {
                std.debug.print("slow frame ws={s}: total={d}ms snapshot={d}ms events={d}ms render={d}ms apply={d}ms\n", .{
                    frame_snapshot.workspace.label(),
                    (t4 - t0) / 1_000_000,
                    (t1 - t0) / 1_000_000,
                    (t2 - t1) / 1_000_000,
                    (t3 - t2) / 1_000_000,
                    (t4 - t3) / 1_000_000,
                });
            }
            // With vsync, SDL_RenderPresent already blocks until the next
            // display refresh; sleeping on top of it beats against the
            // refresh rate and makes the live flow judder.
            if (!self.app_ui.vsync_enabled) sdl.SDL_Delay(16);
        }
    }

    fn buildSnapshot(self: *App) !ui.Snapshot {
        try self.workbench.pollRecordingScan();
        const written = self.ring.written();
        const waveform_count = self.ring.readLatest(self.waveform);
        const plot = self.app_ui.plotSize();
        const playback_enabled = self.player.isPlaying();
        const playback_position = self.player.position();
        const recording_seconds = recordingElapsedSeconds(
            self.recording_enabled,
            self.recording_started_at,
            written,
            self.sample_rate,
        );

        var frame_snapshot = ui.Snapshot{
            .workspace = self.app_workspace,
            .mode = self.settings.mode,
            .frequency_band = self.settings.frequency_band,
            .colormap = self.settings.colormap,
            .sst_enabled = self.engine.sstEnabled(),
            .recording_enabled = self.recording_enabled,
            .recording_seconds = recording_seconds,
            .playback_enabled = playback_enabled,
            .min_hz = self.engine.minFrequency(),
            .max_hz = self.engine.maxFrequency(),
            .custom_range = self.settings.custom_range,
            .waveform = self.waveform[0..waveform_count],
            .recordings = self.workbench.summaries.items,
            .recording_index = self.workbench.selected_recording,
            .active_recording_index = if (self.workbench.has_active_recording) self.workbench.active_recording else null,
            .recording_scan_state = self.workbench.scanState(),
            .recording_rename_active = self.workbench.recording_rename_active,
            .recording_delete_pending = self.workbench.recording_delete_pending and
                self.workbench.recording_delete_index == self.workbench.selected_recording,
            .trim = self.workbench.trimState(playback_position, playback_enabled),
            .spectrum = self.workbench.spectrumState(),
            .band_lab = self.workbench.bandState(),
        };

        switch (frame_snapshot.workspace) {
            .live => {
                const frame = try self.engine.update(
                    self.ring,
                    written,
                    @intCast(self.ring.capacity()),
                    plot.rows,
                    plot.columns,
                );
                frame_snapshot.live_frame = .{
                    .columns = frame.columns,
                    .column_count = frame.column_count,
                    .row_count = frame.row_count,
                    .min_hz = self.engine.minFrequency(),
                    .max_hz = self.engine.maxFrequency(),
                };
            },
            .recordings => {
                frame_snapshot.waveform = self.workbench.clip.fullSamples();
            },
            .trim, .spectrum, .band_lab => {
                try self.workbench.ensureSelectedRecordingLoaded();
                frame_snapshot.waveform = self.workbench.clip.fullSamples();
                frame_snapshot.trim = self.workbench.trimState(playback_position, playback_enabled);

                if (frame_snapshot.workspace == .trim or frame_snapshot.workspace == .spectrum) {
                    try self.workbench.ensureActiveSpectrogram(
                        plot.columns,
                        plot.rows,
                        self.engine.minFrequency(),
                        self.engine.maxFrequency(),
                    );
                }
                if (frame_snapshot.workspace == .spectrum) {
                    const rows = if (self.workbench.active_spectrogram.rows > 0) self.workbench.active_spectrogram.rows else plot.rows;
                    try self.workbench.ensureSpectrum(
                        rows,
                        self.workbench.clip.sample_rate,
                        self.engine.minFrequency(),
                        self.engine.maxFrequency(),
                    );
                    frame_snapshot.spectrum = self.workbench.spectrumState();
                } else if (frame_snapshot.workspace == .band_lab) {
                    try self.workbench.ensureBandRender();
                    try self.workbench.ensureBandEnvelope();
                    try self.workbench.ensureBandSpectrogram(
                        plot.columns,
                        plot.rows,
                        self.engine.minFrequency(),
                        self.engine.maxFrequency(),
                    );
                    frame_snapshot.band_lab = self.workbench.bandState();
                }
            },
        }

        return frame_snapshot;
    }

    fn applyEvents(self: *App, events: ui.PendingEvents) !void {
        if (events.quit) self.running = false;

        const written = self.ring.written();
        var settings_changed = false;

        if (events.workspace) |workspace| {
            self.app_workspace = workspace;
            if (workspace == .live) self.engine.resetModeTimeline(self.settings.mode, written);
        }

        if (events.mode) |mode| {
            self.settings.mode = mode;
            self.engine.setMode(mode);
            self.engine.resetModeTimeline(mode, written);
            settings_changed = true;
        }

        if (events.colormap) |map| {
            self.settings.colormap = map;
            settings_changed = true;
        }

        if (events.frequency_band) |band| {
            self.settings.frequency_band = band;
            try self.engine.setFrequencyBand(band, self.settings.custom_range);
            settings_changed = true;
        }

        if (events.custom_low_hz) |hz| {
            if (std.math.isFinite(hz) and hz > 0.0 and hz < self.settings.custom_range.max_hz) {
                self.settings.custom_range.min_hz = hz;
                self.settings.frequency_band = .custom;
                try self.engine.setFrequencyBand(.custom, self.settings.custom_range);
                settings_changed = true;
            }
        }

        if (events.custom_high_hz) |hz| {
            if (std.math.isFinite(hz) and hz > self.settings.custom_range.min_hz) {
                self.settings.custom_range.max_hz = hz;
                self.settings.frequency_band = .custom;
                try self.engine.setFrequencyBand(.custom, self.settings.custom_range);
                settings_changed = true;
            }
        }

        if (events.toggle_sst) {
            self.engine.toggleSst();
            if (self.settings.mode == .tonal) self.engine.resetModeTimeline(.tonal, written);
        }

        if (settings_changed) try settings_mod.save(self.settings);

        if (events.toggle_recording) {
            if (self.recording_enabled) {
                try self.stopRecording(true);
            } else {
                self.startRecording(written);
            }
        }

        if (events.recording_delta != 0) self.workbench.cycleRecording(events.recording_delta);
        if (events.select_recording) try self.workbench.selectRecording();
        if (events.delete_recording) try self.workbench.deleteSelectedRecording();
        if (events.cancel_recording_delete) self.workbench.cancelRecordingDelete();
        if (events.begin_recording_rename) self.workbench.beginRecordingRename();
        if (events.cancel_recording_rename) self.workbench.cancelRecordingRename();
        if (events.commit_recording_rename) {
            try self.workbench.commitRecordingRename(events.recording_rename_text[0..events.recording_rename_len]);
        }

        self.workbench.applyTrimEvents(events);
        if (events.cycle_audition) self.workbench.cycleAudition();
        if (events.cycle_band_method) self.workbench.cycleBandMethod();
        if (events.cycle_band_edge) self.workbench.cycleBandHandle();
        if (events.lower_band_delta != 0) {
            self.workbench.moveLowerBandEdge(events.lower_band_delta, self.engine.minFrequency(), self.engine.maxFrequency());
        }
        if (events.upper_band_delta != 0) {
            self.workbench.moveUpperBandEdge(events.upper_band_delta, self.engine.minFrequency(), self.engine.maxFrequency());
        }

        if (events.toggle_playback) {
            try self.workbench.togglePlayback(self.player, self.app_workspace);
        }

        try self.workbench.refreshSummaries();
    }

    fn startRecording(self: *App, written: u64) void {
        self.recording_enabled = true;
        self.recording_started_at = written;
        std.log.info("recording started", .{});
    }

    fn stopRecording(self: *App, add_to_workbench: bool) !void {
        const started_at = self.recording_started_at;
        const ended_at = self.ring.written();
        const recorded = try self.captureRecordingSnapshot(started_at, ended_at);
        defer recorded.deinit(self.allocator);

        self.recording_enabled = false;
        self.recording_started_at = ended_at;
        const samples = recorded.samples();
        if (samples.len == 0) return;

        const saved = try recording.saveSamples(self.allocator, samples, self.sample_rate);
        defer saved.deinit(self.allocator);
        if (saved.sample_count > 0) {
            std.log.info("saved {d} recorded sample(s) to {s} as 32-bit float WAV", .{ saved.sample_count, saved.path });
            if (add_to_workbench) {
                try self.workbench.addRecordingFromSamples(samples, self.sample_rate, saved.path);
            }
        }
    }

    fn captureRecordingSnapshot(self: *App, started_at: u64, ended_at: u64) !RecordingSnapshot {
        if (ended_at <= started_at) return .{};
        const requested = ended_at - started_at;
        const wanted_u64 = @min(requested, @as(u64, @intCast(self.ring.capacity())));
        if (wanted_u64 == 0 or wanted_u64 > std.math.maxInt(usize)) return .{};

        const samples = try self.allocator.alloc(f32, @intCast(wanted_u64));
        errdefer self.allocator.free(samples);
        const count = self.ring.readAvailableEndingAt(ended_at, samples);
        if (count == 0) {
            self.allocator.free(samples);
            return .{};
        }
        return .{ .buffer = samples, .count = count };
    }
};

fn inputFormatSupported(format: capture.Format) bool {
    return std.math.isFinite(format.sample_rate) and
        format.sample_rate > 0.0 and
        format.sample_rate <= maximum_audio_sample_rate and
        format.channels_per_frame > 0 and
        format.channels_per_frame <= maximum_audio_channels;
}

fn sampleCountForSeconds(sample_rate: f64, seconds: f64) u64 {
    if (!std.math.isFinite(sample_rate) or sample_rate <= 0.0 or seconds <= 0.0) return 0;
    return @intFromFloat(sample_rate * seconds);
}

fn waveformCapacityForRate(sample_rate: f64) u64 {
    return @max(sampleCountForSeconds(sample_rate, waveform_seconds), minimum_waveform_samples);
}

fn ringCapacityForRate(sample_rate: f64) u64 {
    return @max(
        sampleCountForSeconds(sample_rate, capture_buffer_seconds),
        sampleCountForSeconds(sample_rate, raw_recording_seconds),
    );
}

fn recordingElapsedSeconds(recording_enabled: bool, started_at: u64, written: u64, sample_rate: f64) f64 {
    if (!recording_enabled or written <= started_at or sample_rate <= 0.0) return 0.0;
    return @as(f64, @floatFromInt(written - started_at)) / sample_rate;
}

test "app sizing helpers preserve capture and recording windows" {
    try std.testing.expectEqual(@as(u64, 1024), waveformCapacityForRate(1_000.0));
    try std.testing.expectEqual(@as(u64, 1_440_000), ringCapacityForRate(48_000.0));
    try std.testing.expectApproxEqAbs(@as(f64, 2.0), recordingElapsedSeconds(true, 10, 20, 5.0), 0.0001);
}
