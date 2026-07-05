//! Offline workbench (recordings list, trim, spectrum, band lab).
//! Rewrite target; C reference: src_c/src/app/workbench/

const std = @import("std");
const band_render = @import("../analysis/band_render.zig");
const offline_spectrum = @import("../analysis/offline_spectrum.zig");
const playback = @import("../audio/playback.zig");
const frequency_band = @import("../support/frequency_band.zig");
const ui = @import("../ui/ui.zig");
const app_clip = @import("clip.zig");
const recording_io = @import("recording.zig");

const RecordingList = std.array_list.Managed(Recording);
const SummaryList = std.array_list.Managed(ui.RecordingSummary);

const maximum_workbench_sample_rate = 512_000.0;
const trim_step_seconds = 0.02;

const C = struct {
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

    extern "c" fn localtime_r(timer: *const std.c.time_t, result: *Tm) ?*Tm;
    extern "c" fn strftime(buffer: [*]u8, max_size: usize, format: [*:0]const u8, time_ptr: *const Tm) usize;
};

pub const Error = error{
    InvalidWorkbenchState,
    RecordingNameExists,
    RecordingNameTooLong,
    TooLarge,
};

pub const Recording = struct {
    allocator: std.mem.Allocator,
    clip: app_clip.Clip,
    path: []u8 = &.{},
    label_storage: [128]u8 = [_]u8{0} ** 128,
    label_len: usize = 0,
    created_storage: [32]u8 = [_]u8{0} ** 32,
    created_len: usize = 0,
    created_at: i128 = 0,
    seconds: f64 = 0.0,
    loaded: bool = false,
    disk_backed: bool = false,

    pub fn init(allocator: std.mem.Allocator) Recording {
        return .{
            .allocator = allocator,
            .clip = app_clip.Clip.init(allocator),
        };
    }

    pub fn deinit(self: *Recording) void {
        self.clip.deinit();
        self.allocator.free(self.path);
        self.* = undefined;
    }

    fn label(self: *const Recording) []const u8 {
        return self.label_storage[0..self.label_len];
    }

    fn createdLabel(self: *const Recording) []const u8 {
        return self.created_storage[0..self.created_len];
    }

    fn setPath(self: *Recording, path: []const u8) !void {
        const copy = try self.allocator.dupe(u8, path);
        self.allocator.free(self.path);
        self.path = copy;
        self.disk_backed = path.len > 0;
    }

    fn setLabel(self: *Recording, text: []const u8) void {
        const source = if (text.len == 0) "recording" else text;
        self.label_len = @min(source.len, self.label_storage.len);
        @memcpy(self.label_storage[0..self.label_len], source[0..self.label_len]);
    }

    fn setCreatedLabel(self: *Recording, seconds: i128) void {
        self.created_at = seconds;
        const text = formatCreatedLabel(seconds, &self.created_storage);
        self.created_len = text.len;
    }

    fn summary(self: *const Recording) ui.RecordingSummary {
        return .{
            .label = self.label(),
            .created = self.createdLabel(),
            .seconds = self.seconds,
            .loaded = self.loaded,
        };
    }
};

const SpectrogramCache = struct {
    allocator: std.mem.Allocator,
    cells: []f32 = &.{},
    columns: usize = 0,
    rows: usize = 0,
    source_id: usize = 0,
    source_count: usize = 0,
    sample_rate: f64 = 0.0,
    min_hz: f64 = 0.0,
    max_hz: f64 = 0.0,
    dirty: bool = true,

    fn init(allocator: std.mem.Allocator) SpectrogramCache {
        return .{ .allocator = allocator };
    }

    fn deinit(self: *SpectrogramCache) void {
        self.allocator.free(self.cells);
        self.* = undefined;
    }

    fn clear(self: *SpectrogramCache) void {
        self.columns = 0;
        self.rows = 0;
        self.source_id = 0;
        self.source_count = 0;
        self.dirty = true;
    }

    fn ensure(
        self: *SpectrogramCache,
        name: []const u8,
        samples: []const f32,
        sample_rate: f64,
        columns_arg: usize,
        rows_arg: usize,
        min_hz: f64,
        max_hz: f64,
    ) !void {
        _ = name;
        const columns: usize = @min(columns_arg, @as(usize, 512));
        const rows: usize = @min(rows_arg, @as(usize, 320));
        if (samples.len == 0 or columns == 0 or rows == 0) {
            self.clear();
            self.source_id = if (samples.len > 0) @intFromPtr(samples.ptr) else 0;
            self.source_count = samples.len;
            self.sample_rate = sample_rate;
            self.min_hz = min_hz;
            self.max_hz = max_hz;
            return;
        }
        if (!validSampleRate(sample_rate)) return Error.InvalidWorkbenchState;
        if (columns > std.math.maxInt(usize) / rows) return Error.TooLarge;

        const cell_count = columns * rows;
        if (self.cells.len != cell_count) {
            self.allocator.free(self.cells);
            self.cells = try self.allocator.alloc(f32, cell_count);
            self.columns = columns;
            self.rows = rows;
            self.dirty = true;
        }

        const source_id = @intFromPtr(samples.ptr);
        if (self.source_id != source_id or
            self.source_count != samples.len or
            self.sample_rate != sample_rate or
            self.min_hz != min_hz or
            self.max_hz != max_hz)
        {
            self.source_id = source_id;
            self.source_count = samples.len;
            self.sample_rate = sample_rate;
            self.min_hz = min_hz;
            self.max_hz = max_hz;
            self.dirty = true;
        }

        if (!self.dirty) return;
        try offline_spectrum.spectrogramDb(
            self.allocator,
            samples,
            sample_rate,
            min_hz,
            max_hz,
            self.cells,
            rows,
            columns,
        );
        self.dirty = false;
    }

    fn frame(self: *const SpectrogramCache) ui.SpectrogramFrame {
        if (self.dirty or self.columns == 0 or self.rows == 0) return .{};
        return .{
            .columns = self.cells,
            .column_count = self.columns,
            .row_count = self.rows,
            .min_hz = self.min_hz,
            .max_hz = self.max_hz,
        };
    }
};

pub const Workbench = struct {
    allocator: std.mem.Allocator,
    clip: app_clip.Clip,
    recordings: RecordingList,
    summaries: SummaryList,
    selected_recording: usize = 0,
    active_recording: usize = 0,
    has_active_recording: bool = false,
    scan_started: bool = false,
    scan_complete: bool = false,
    scan_failed: bool = false,
    recording_rename_active: bool = false,
    recording_delete_pending: bool = false,
    recording_delete_index: usize = 0,

    spectrum_cells: []f32 = &.{},
    spectrum_rows: usize = 0,
    spectrum_min_hz: f64 = 0.0,
    spectrum_max_hz: f64 = 0.0,
    spectrum_dirty: bool = true,
    active_spectrogram: SpectrogramCache,
    band_spectrogram: SpectrogramCache,

    selected_samples: []f32 = &.{},
    rejected_samples: []f32 = &.{},
    render_count: usize = 0,
    render_dirty: bool = true,

    low_hz: f64 = frequency_band.mid_min_hz,
    high_hz: f64 = frequency_band.mid_max_hz,
    playback_offset: u64 = 0,
    band_method: band_render.Method = .fft_mask,
    audition: ui.Audition = .original,
    trim_edge: TrimEdge = .start,
    upper_handle_selected: bool = false,
    trim_editing: bool = false,
    draft_trim_start: u64 = 0,
    draft_trim_end: u64 = 0,

    pub fn init(allocator: std.mem.Allocator) Workbench {
        return .{
            .allocator = allocator,
            .clip = app_clip.Clip.init(allocator),
            .recordings = RecordingList.init(allocator),
            .summaries = SummaryList.init(allocator),
            .active_spectrogram = SpectrogramCache.init(allocator),
            .band_spectrogram = SpectrogramCache.init(allocator),
        };
    }

    pub fn deinit(self: *Workbench) void {
        self.clip.deinit();
        for (self.recordings.items) |*recording| recording.deinit();
        self.recordings.deinit();
        self.summaries.deinit();
        self.allocator.free(self.spectrum_cells);
        self.active_spectrogram.deinit();
        self.band_spectrogram.deinit();
        self.allocator.free(self.selected_samples);
        self.allocator.free(self.rejected_samples);
        self.* = undefined;
    }

    pub fn startRecordingScan(self: *Workbench) !void {
        if (self.scan_started) return;
        self.scan_started = true;
        self.scan_complete = false;
        self.scan_failed = false;
        self.scanRecordings(defaultIo(), .cwd()) catch |err| {
            self.scan_failed = true;
            self.scan_complete = true;
            return err;
        };
        self.scan_complete = true;
        try self.refreshSummaries();
    }

    pub fn pollRecordingScan(self: *Workbench) !void {
        if (!self.scan_started) try self.startRecordingScan();
    }

    pub fn scanState(self: *const Workbench) ui.RecordingScanState {
        if (self.scan_failed) return .failed;
        if (!self.scan_complete) return .scanning;
        return .complete;
    }

    pub fn refreshSummaries(self: *Workbench) !void {
        self.summaries.clearRetainingCapacity();
        try self.summaries.ensureTotalCapacity(self.recordings.items.len);
        for (self.recordings.items) |*recording| {
            self.summaries.appendAssumeCapacity(recording.summary());
        }
    }

    pub fn cycleRecording(self: *Workbench, delta: isize) void {
        if (delta == 0 or self.recordings.items.len == 0) return;
        self.syncActiveRecordingTrim();
        const count: isize = @intCast(self.recordings.items.len);
        var index: isize = @as(isize, @intCast(self.selected_recording)) + delta;
        while (index < 0) index += count;
        self.selected_recording = @intCast(@mod(index, count));
        self.recording_delete_pending = false;
    }

    pub fn selectRecording(self: *Workbench) !void {
        if (self.recordings.items.len == 0) return;
        try self.selectRecordingIndex(self.selected_recording);
    }

    pub fn ensureSelectedRecordingLoaded(self: *Workbench) !void {
        if (self.recordings.items.len == 0) return;
        if (self.selected_recording >= self.recordings.items.len) self.selected_recording = 0;
        if (self.has_active_recording and
            self.active_recording == self.selected_recording and
            self.recordings.items[self.selected_recording].loaded and
            self.clip.hasAudio())
        {
            return;
        }
        try self.selectRecordingIndex(self.selected_recording);
    }

    pub fn deleteSelectedRecording(self: *Workbench) !void {
        if (self.recordings.items.len == 0 or self.selected_recording >= self.recordings.items.len) return;
        if (!self.recording_delete_pending or self.recording_delete_index != self.selected_recording) {
            self.recording_delete_pending = true;
            self.recording_delete_index = self.selected_recording;
            return;
        }

        self.syncActiveRecordingTrim();
        const deleted = self.selected_recording;
        var removed = self.recordings.orderedRemove(deleted);
        defer removed.deinit();

        if (removed.disk_backed and removed.path.len > 0) {
            std.Io.Dir.cwd().deleteFile(defaultIo(), removed.path) catch |err| switch (err) {
                error.FileNotFound => {},
                else => return err,
            };
        }

        if (self.recordings.items.len == 0) {
            self.selected_recording = 0;
            self.clearActiveClip();
        } else {
            if (self.selected_recording >= self.recordings.items.len) {
                self.selected_recording = self.recordings.items.len - 1;
            }
            if (self.has_active_recording) {
                if (self.active_recording == deleted) {
                    self.clearActiveClip();
                } else if (self.active_recording > deleted) {
                    self.active_recording -= 1;
                }
            }
        }
        self.recording_delete_pending = false;
        try self.refreshSummaries();
    }

    pub fn cancelRecordingDelete(self: *Workbench) void {
        self.recording_delete_pending = false;
    }

    pub fn beginRecordingRename(self: *Workbench) void {
        if (self.recordings.items.len == 0 or self.selected_recording >= self.recordings.items.len) return;
        self.syncActiveRecordingTrim();
        self.recording_rename_active = true;
        self.recording_delete_pending = false;
    }

    pub fn cancelRecordingRename(self: *Workbench) void {
        self.recording_rename_active = false;
    }

    pub fn commitRecordingRename(self: *Workbench, text: []const u8) !void {
        if (!self.recording_rename_active) return;
        if (self.recordings.items.len == 0 or self.selected_recording >= self.recordings.items.len) {
            self.cancelRecordingRename();
            return;
        }

        var stem_storage: [128]u8 = undefined;
        const stem = sanitizedRecordingStem(text, &stem_storage);
        const new_path = try std.fmt.allocPrint(self.allocator, "{s}/{s}.wav", .{ recording_io.directory, stem });
        defer self.allocator.free(new_path);
        if (new_path.len >= recording_io.path_capacity) return Error.RecordingNameTooLong;

        var rec = &self.recordings.items[self.selected_recording];
        if (rec.disk_backed and rec.path.len > 0 and !std.mem.eql(u8, rec.path, new_path)) {
            if (std.Io.Dir.cwd().access(defaultIo(), new_path, .{})) |_| {
                return Error.RecordingNameExists;
            } else |err| switch (err) {
                error.FileNotFound => {},
                else => return err,
            }
            try std.Io.Dir.cwd().rename(rec.path, std.Io.Dir.cwd(), new_path, defaultIo());
            try rec.setPath(new_path);
        }

        rec.setLabel(stem);
        if (rec.loaded) rec.clip.setLabel(stem);
        if (self.has_active_recording and self.active_recording == self.selected_recording) {
            self.clip.setLabel(stem);
        }
        self.cancelRecordingRename();
        try self.refreshSummaries();
    }

    pub fn addRecordingFromSamples(
        self: *Workbench,
        samples: []const f32,
        sample_rate: f64,
        path_arg: []const u8,
    ) !void {
        if (samples.len == 0) return;
        self.syncActiveRecordingTrim();

        var rec = Recording.init(self.allocator);
        errdefer rec.deinit();
        if (path_arg.len > 0) try rec.setPath(path_arg);
        rec.setLabel(labelFromPath(path_arg, "new recording"));
        rec.seconds = if (sample_rate > 0.0) @as(f64, @floatFromInt(samples.len)) / sample_rate else 0.0;
        rec.loaded = true;
        rec.setCreatedLabel(fileCreatedAtSeconds(defaultIo(), .cwd(), path_arg) orelse unixNowSeconds());
        try rec.clip.replace(samples, sample_rate, rec.label());

        const inserted = try self.insertRecordingSorted(&rec);
        try self.selectRecordingIndex(inserted);
    }

    pub fn applyTrimEvents(self: *Workbench, events: ui.PendingEvents) void {
        if (self.clip.samples.len == 0) return;
        if (events.select_trim_start) self.beginTrimEdit(.start);
        if (events.select_trim_end) self.beginTrimEdit(.end);
        if (events.trim_delta != 0) self.moveDraftTrimEdge(events.trim_delta);
        if (events.apply_trim and self.trim_editing) {
            self.clip.setTrim(self.draft_trim_start, self.draft_trim_end);
            self.trim_editing = false;
            self.syncActiveRecordingTrim();
            self.markClipChanged();
        }
        if (events.clear_trim) {
            self.clip.clearTrim();
            self.trim_editing = false;
            self.syncActiveRecordingTrim();
            self.markClipChanged();
        }
    }

    pub fn cycleAudition(self: *Workbench) void {
        self.audition = switch (self.audition) {
            .original => .selected,
            .selected => .rejected,
            .rejected => .original,
        };
    }

    pub fn cycleBandMethod(self: *Workbench) void {
        self.band_method = band_render.methodOffset(self.band_method, 1);
        self.markBandRenderDirty();
    }

    pub fn cycleBandHandle(self: *Workbench) void {
        self.upper_handle_selected = !self.upper_handle_selected;
    }

    pub fn moveSelectedBandEdge(self: *Workbench, semitone_steps: isize, min_hz: f64, max_hz: f64) void {
        self.moveBandEdge(self.upper_handle_selected, semitone_steps, min_hz, max_hz);
    }

    pub fn moveLowerBandEdge(self: *Workbench, semitone_steps: isize, min_hz: f64, max_hz: f64) void {
        self.moveBandEdge(false, semitone_steps, min_hz, max_hz);
    }

    pub fn moveUpperBandEdge(self: *Workbench, semitone_steps: isize, min_hz: f64, max_hz: f64) void {
        self.moveBandEdge(true, semitone_steps, min_hz, max_hz);
    }

    pub fn ensureSpectrum(self: *Workbench, rows: usize, sample_rate: f64, min_hz: f64, max_hz: f64) !void {
        const samples = self.clip.activeSamples();
        if (samples.len == 0 or rows == 0) return;
        if (!validSampleRate(sample_rate)) return Error.InvalidWorkbenchState;

        if (self.spectrum_cells.len != rows) {
            self.allocator.free(self.spectrum_cells);
            self.spectrum_cells = try self.allocator.alloc(f32, rows);
            self.spectrum_rows = rows;
            self.spectrum_dirty = true;
        }
        if (self.spectrum_min_hz != min_hz or self.spectrum_max_hz != max_hz) {
            self.spectrum_min_hz = min_hz;
            self.spectrum_max_hz = max_hz;
            self.spectrum_dirty = true;
        }
        if (!self.spectrum_dirty) return;
        try offline_spectrum.spectrumDb(self.allocator, samples, sample_rate, min_hz, max_hz, self.spectrum_cells);
        self.spectrum_dirty = false;
    }

    pub fn ensureActiveSpectrogram(self: *Workbench, columns: usize, rows: usize, min_hz: f64, max_hz: f64) !void {
        const range = self.activeRange();
        const samples = if (range.end > range.start)
            self.clip.samples[@intCast(range.start)..@intCast(range.end)]
        else
            &.{};
        try self.active_spectrogram.ensure("active", samples, self.clip.sample_rate, columns, rows, min_hz, max_hz);
    }

    pub fn ensureBandRender(self: *Workbench) !void {
        if (!self.clip.hasAudio()) {
            self.render_count = 0;
            self.band_spectrogram.clear();
            return;
        }

        const samples = self.clip.activeSamples();
        try self.ensureRenderBuffers(samples.len);
        if (!self.render_dirty) return;

        try band_render.render(
            self.allocator,
            samples,
            .{
                .sample_rate = self.clip.sample_rate,
                .low_hz = self.low_hz,
                .high_hz = self.high_hz,
                .iterations = 2,
            },
            self.band_method,
            self.selected_samples[0..samples.len],
        );
        band_render.sanitizeOutput(self.selected_samples[0..samples.len], self.clip.sample_rate, samples);
        subtractInto(self.rejected_samples[0..samples.len], samples, self.selected_samples[0..samples.len]);
        band_render.sanitizeOutput(self.rejected_samples[0..samples.len], self.clip.sample_rate, samples);
        self.render_dirty = false;
    }

    pub fn ensureBandSpectrogram(self: *Workbench, columns: usize, rows: usize, min_hz: f64, max_hz: f64) !void {
        if (self.render_dirty) {
            self.band_spectrogram.clear();
            return;
        }
        try self.band_spectrogram.ensure(
            "band",
            self.selected_samples[0..self.render_count],
            self.clip.sample_rate,
            columns,
            rows,
            min_hz,
            max_hz,
        );
    }

    pub fn togglePlayback(self: *Workbench, player: ?*playback.Playback, workspace: ui.Workspace) !void {
        const live_player = player orelse return;
        if (live_player.isPlaying()) {
            try live_player.stop();
            return;
        }

        if (!self.clip.hasAudio()) try self.ensureSelectedRecordingLoaded();
        if (!self.clip.hasAudio()) return;

        var samples = self.clip.activeSamples();
        if (workspace == .band_lab and self.audition != .original) {
            try self.ensureBandRender();
            samples = switch (self.audition) {
                .original => samples,
                .selected => self.selected_samples[0..self.render_count],
                .rejected => self.rejected_samples[0..self.render_count],
            };
        }
        try live_player.start(samples, self.clip.sample_rate);
        self.playback_offset = self.clip.trim_start;
    }

    pub fn trimState(self: *const Workbench, playback_position: u64, playback_visible: bool) ui.TrimState {
        const start = if (self.trim_editing) self.draft_trim_start else self.clip.trim_start;
        const end = if (self.trim_editing) self.draft_trim_end else self.clip.trim_end;
        return .{
            .samples = self.clip.fullSamples(),
            .source_sample_count = @intCast(self.clip.samples.len),
            .trim_start_sample = start,
            .trim_end_sample = end,
            .trim_end_selected = self.trim_edge == .end,
            .playback_sample = @min(self.playback_offset + playback_position, @as(u64, @intCast(self.clip.samples.len))),
            .playback_visible = playback_visible and self.clip.samples.len > 0,
        };
    }

    pub fn bandState(self: *const Workbench) ui.BandLabState {
        return .{
            .spectrogram = self.band_spectrogram.frame(),
            .low_hz = self.low_hz,
            .high_hz = self.high_hz,
            .upper_selected = self.upper_handle_selected,
            .method = self.band_method,
            .audition = self.audition,
        };
    }

    pub fn spectrumState(self: *const Workbench) ui.SpectrumState {
        return .{ .db_rows = if (self.spectrum_dirty) &.{} else self.spectrum_cells[0..self.spectrum_rows] };
    }

    fn scanRecordings(self: *Workbench, io_arg: std.Io, base_dir: std.Io.Dir) !void {
        var dir = base_dir.openDir(io_arg, recording_io.directory, .{ .iterate = true }) catch |err| switch (err) {
            error.FileNotFound => return,
            else => return err,
        };
        defer dir.close(io_arg);

        var it = dir.iterate();
        while (try it.next(io_arg)) |entry| {
            if (entry.kind != .file or !hasWavExtension(entry.name)) continue;
            const path = try std.fmt.allocPrint(self.allocator, "{s}/{s}", .{ recording_io.directory, entry.name });
            defer self.allocator.free(path);
            const info = recording_io.readInfoInDir(io_arg, base_dir, path) catch continue;
            var rec = Recording.init(self.allocator);
            errdefer rec.deinit();
            try rec.setPath(path);
            rec.setLabel(labelFromPath(path, "recording"));
            rec.seconds = info.duration_seconds;
            rec.loaded = false;
            rec.disk_backed = true;
            rec.setCreatedLabel(fileCreatedAtSeconds(io_arg, base_dir, path) orelse 0);
            _ = try self.insertRecordingSorted(&rec);
        }
        if (self.recordings.items.len > 0) self.selected_recording = 0;
    }

    fn insertRecordingSorted(self: *Workbench, recording: *Recording) !usize {
        if (recording.path.len > 0) {
            for (self.recordings.items, 0..) |*existing, index| {
                if (std.mem.eql(u8, existing.path, recording.path)) {
                    recording.deinit();
                    recording.* = Recording.init(self.allocator);
                    return index;
                }
            }
        }

        var index: usize = 0;
        while (index < self.recordings.items.len) : (index += 1) {
            const existing = &self.recordings.items[index];
            if (existing.created_at < recording.created_at) break;
            if (existing.created_at == recording.created_at and std.mem.order(u8, existing.label(), recording.label()) == .gt) break;
        }
        if (index <= self.selected_recording and self.recordings.items.len > 0) self.selected_recording += 1;
        if (self.has_active_recording and index <= self.active_recording and self.recordings.items.len > 0) self.active_recording += 1;
        try self.recordings.insert(index, recording.*);
        recording.* = Recording.init(self.allocator);
        try self.refreshSummaries();
        return index;
    }

    fn selectRecordingIndex(self: *Workbench, index: usize) !void {
        if (index >= self.recordings.items.len) return;
        self.syncActiveRecordingTrim();
        var rec = &self.recordings.items[index];
        try self.loadRecordingSamples(rec);
        try self.clip.replace(rec.clip.fullSamples(), rec.clip.sample_rate, rec.clip.label());
        self.clip.setTrim(rec.clip.trim_start, rec.clip.trim_end);
        self.selected_recording = index;
        self.active_recording = index;
        self.has_active_recording = true;
        self.trim_editing = false;
        self.recording_delete_pending = false;
        self.markClipChanged();
        try self.refreshSummaries();
    }

    fn loadRecordingSamples(self: *Workbench, rec: *Recording) !void {
        if (rec.loaded) return;
        if (!rec.disk_backed or rec.path.len == 0) return Error.InvalidWorkbenchState;
        const loaded = try recording_io.loadSamples(self.allocator, rec.path);
        defer loaded.deinit(self.allocator);
        try rec.clip.replace(loaded.samples, loaded.sample_rate, rec.label());
        rec.seconds = if (loaded.sample_rate > 0.0) @as(f64, @floatFromInt(loaded.samples.len)) / loaded.sample_rate else 0.0;
        rec.loaded = true;
    }

    fn syncActiveRecordingTrim(self: *Workbench) void {
        if (!self.has_active_recording or self.active_recording >= self.recordings.items.len) return;
        var rec = &self.recordings.items[self.active_recording];
        if (!rec.loaded or rec.clip.samples.len != self.clip.samples.len) return;
        rec.clip.trim_start = self.clip.trim_start;
        rec.clip.trim_end = self.clip.trim_end;
    }

    fn clearActiveClip(self: *Workbench) void {
        self.clip.clear();
        self.has_active_recording = false;
        self.active_recording = 0;
        self.trim_editing = false;
        self.markClipChanged();
    }

    fn markClipChanged(self: *Workbench) void {
        self.spectrum_dirty = true;
        self.active_spectrogram.clear();
        self.band_spectrogram.clear();
        self.render_dirty = true;
        self.render_count = 0;
        self.playback_offset = self.clip.trim_start;
    }

    fn markBandRenderDirty(self: *Workbench) void {
        self.render_dirty = true;
        self.band_spectrogram.clear();
    }

    fn beginTrimEdit(self: *Workbench, edge: TrimEdge) void {
        if (self.clip.samples.len == 0) return;
        if (!self.trim_editing) {
            self.draft_trim_start = self.clip.trim_start;
            self.draft_trim_end = self.clip.trim_end;
            self.trim_editing = true;
            self.active_spectrogram.dirty = true;
        }
        self.trim_edge = edge;
    }

    fn moveDraftTrimEdge(self: *Workbench, delta_steps: isize) void {
        if (self.clip.samples.len == 0 or delta_steps == 0) return;
        if (!self.trim_editing) self.beginTrimEdit(self.trim_edge);
        const step: u64 = if (validSampleRate(self.clip.sample_rate))
            @max(1, @as(u64, @intFromFloat(self.clip.sample_rate * trim_step_seconds)))
        else
            1;
        const delta = delta_steps * @as(isize, @intCast(step));
        if (self.trim_edge == .start) {
            const limit: isize = @intCast(self.draft_trim_end - 1);
            const next = std.math.clamp(@as(isize, @intCast(self.draft_trim_start)) + delta, 0, limit);
            self.draft_trim_start = @intCast(next);
        } else {
            const minimum: isize = @intCast(self.draft_trim_start + 1);
            const maximum: isize = @intCast(self.clip.samples.len);
            const next = std.math.clamp(@as(isize, @intCast(self.draft_trim_end)) + delta, minimum, maximum);
            self.draft_trim_end = @intCast(next);
        }
        self.active_spectrogram.dirty = true;
    }

    fn activeRange(self: *const Workbench) struct { start: u64, end: u64 } {
        var start = if (self.trim_editing) self.draft_trim_start else self.clip.trim_start;
        var end = if (self.trim_editing) self.draft_trim_end else self.clip.trim_end;
        const sample_count: u64 = @intCast(self.clip.samples.len);
        start = @min(start, sample_count);
        end = @min(end, sample_count);
        if (end < start) end = start;
        return .{ .start = start, .end = end };
    }

    fn moveBandEdge(self: *Workbench, upper: bool, semitone_steps: isize, min_hz: f64, max_hz: f64) void {
        if (semitone_steps == 0) return;
        const ratio = std.math.pow(f64, 2.0, @as(f64, @floatFromInt(semitone_steps)) / 12.0);
        if (upper) {
            self.high_hz = @min(max_hz, @max(self.low_hz * 1.03, self.high_hz * ratio));
        } else {
            self.low_hz = @max(min_hz, @min(self.high_hz / 1.03, self.low_hz * ratio));
        }
        self.markBandRenderDirty();
    }

    fn ensureRenderBuffers(self: *Workbench, sample_count: usize) !void {
        if (sample_count == 0) return Error.InvalidWorkbenchState;
        if (self.render_count == sample_count and self.selected_samples.len >= sample_count and self.rejected_samples.len >= sample_count) return;
        const selected = try self.allocator.alloc(f32, sample_count);
        errdefer self.allocator.free(selected);
        const rejected = try self.allocator.alloc(f32, sample_count);
        errdefer self.allocator.free(rejected);

        self.allocator.free(self.selected_samples);
        self.allocator.free(self.rejected_samples);
        self.selected_samples = selected;
        self.rejected_samples = rejected;
        self.render_count = sample_count;
        self.markBandRenderDirty();
    }
};

const TrimEdge = enum { start, end };

fn validSampleRate(sample_rate: f64) bool {
    return std.math.isFinite(sample_rate) and sample_rate > 0.0 and sample_rate <= maximum_workbench_sample_rate;
}

fn hasWavExtension(name: []const u8) bool {
    return std.ascii.endsWithIgnoreCase(name, ".wav");
}

fn labelFromPath(path: []const u8, fallback: []const u8) []const u8 {
    if (path.len == 0) return fallback;
    var name = path;
    if (std.mem.lastIndexOfScalar(u8, path, '/')) |slash| name = path[slash + 1 ..];
    if (std.ascii.endsWithIgnoreCase(name, ".wav")) name = name[0 .. name.len - 4];
    return if (name.len == 0) fallback else name;
}

fn sanitizedRecordingStem(input: []const u8, out: []u8) []const u8 {
    var length: usize = 0;
    for (input) |byte| {
        var output: u8 = 0;
        if (std.ascii.isAlphanumeric(byte)) {
            output = byte;
        } else if (byte == '-' or byte == '_') {
            output = byte;
        } else if (byte == ' ') {
            output = '-';
        }
        if (output == 0) continue;
        if (length > 0 and output == '-' and out[length - 1] == '-') continue;
        if (length >= out.len) break;
        out[length] = output;
        length += 1;
    }
    while (length > 0 and out[length - 1] == '-') length -= 1;
    if (length == 0) {
        const fallback = "recording";
        @memcpy(out[0..fallback.len], fallback);
        return out[0..fallback.len];
    }
    return out[0..length];
}

fn fileCreatedAtSeconds(io_arg: std.Io, dir: std.Io.Dir, path: []const u8) ?i128 {
    if (path.len == 0) return null;
    const stat = dir.statFile(io_arg, path, .{}) catch return null;
    return @divTrunc(stat.mtime.nanoseconds, std.time.ns_per_s);
}

fn unixNowSeconds() i128 {
    return @divTrunc(std.Io.Clock.real.now(defaultIo()).nanoseconds, std.time.ns_per_s);
}

fn formatCreatedLabel(seconds: i128, buffer: []u8) []const u8 {
    if (seconds <= 0) return writeUnknownTime(buffer);
    var timer: std.c.time_t = @intCast(seconds);
    var local: C.Tm = undefined;
    if (C.localtime_r(&timer, &local) == null) return writeUnknownTime(buffer);
    const length = C.strftime(buffer.ptr, buffer.len, "%Y-%m-%d %H:%M", &local);
    if (length == 0) return writeUnknownTime(buffer);
    return buffer[0..length];
}

fn writeUnknownTime(buffer: []u8) []const u8 {
    const text = "unknown time";
    const len = @min(buffer.len, text.len);
    @memcpy(buffer[0..len], text[0..len]);
    return buffer[0..len];
}

fn subtractInto(output: []f32, left: []const f32, right: []const f32) void {
    const count = @min(output.len, @min(left.len, right.len));
    const lanes = comptime (std.simd.suggestVectorLength(f32) orelse 4);
    const Vec = @Vector(lanes, f32);
    var index: usize = 0;
    while (index + lanes <= count) : (index += lanes) {
        const a: Vec = left[index..][0..lanes].*;
        const b: Vec = right[index..][0..lanes].*;
        output[index..][0..lanes].* = a - b;
    }
    while (index < count) : (index += 1) output[index] = left[index] - right[index];
}

fn defaultIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "recording stem sanitizes names like the C workbench" {
    var buffer: [64]u8 = undefined;
    try std.testing.expectEqualStrings("hello-world_2wav", sanitizedRecordingStem("hello  world_2!!.wav", &buffer));
    try std.testing.expectEqualStrings("recording", sanitizedRecordingStem("!!!", &buffer));
}

test "workbench inserts recordings newest-first and selects added audio" {
    var wb = Workbench.init(std.testing.allocator);
    defer wb.deinit();

    try wb.addRecordingFromSamples(&[_]f32{ 0.1, 0.2, 0.3 }, 3.0, "");
    try std.testing.expectEqual(@as(usize, 1), wb.recordings.items.len);
    try std.testing.expect(wb.clip.hasAudio());
    try std.testing.expectEqual(@as(usize, 0), wb.selected_recording);

    wb.beginTrimEdit(.start);
    wb.moveDraftTrimEdge(1);
    wb.applyTrimEvents(.{ .apply_trim = true });
    try std.testing.expectEqual(@as(u64, 1), wb.clip.trim_start);
}

test "workbench duplicate recording paths reuse existing row" {
    var wb = Workbench.init(std.testing.allocator);
    defer wb.deinit();

    try wb.addRecordingFromSamples(&[_]f32{ 0.1, 0.2 }, 2.0, "recordings/one.wav");
    try wb.addRecordingFromSamples(&[_]f32{ 9.0, 10.0 }, 2.0, "recordings/one.wav");

    try std.testing.expectEqual(@as(usize, 1), wb.recordings.items.len);
    try std.testing.expectEqualStrings("one", wb.recordings.items[0].label());
    try std.testing.expectEqualSlices(f32, &[_]f32{ 0.1, 0.2 }, wb.clip.activeSamples());
}
