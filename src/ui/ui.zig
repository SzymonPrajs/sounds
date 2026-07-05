//! Fresh immediate-mode UI on SDL3. The public surface is deliberately
//! app-neutral: phase 5 can feed a `Snapshot`, process `PendingEvents`, and let
//! this module own SDL texture presentation and all text/pixel drawing.

const std = @import("std");
const sdl = @import("../c.zig").sdl;

pub const core = @import("core.zig");
pub const draw = @import("draw.zig");
pub const font = @import("font.zig");
pub const layout = @import("layout.zig");
pub const widgets = @import("widgets.zig");

const analysis = @import("../analysis/engine.zig");
const band_render = @import("../analysis/band_render.zig");
const colormap = @import("../support/colormap.zig");
const frequency_band = @import("../support/frequency_band.zig");

pub const Workspace = layout.Workspace;
pub const AnalysisMode = analysis.Mode;
pub const FrequencyBand = frequency_band.FrequencyBand;
pub const Colormap = colormap.Colormap;

pub const Error = error{
    SdlRenderer,
    SdlTexture,
    SdlWindowSize,
    SdlUpdateTexture,
    SdlRender,
    TooLarge,
};

pub const SpectrogramFrame = struct {
    columns: []const f32 = &.{},
    column_count: usize = 0,
    row_count: usize = 0,
    min_hz: f64 = frequency_band.full_min_hz,
    max_hz: f64 = frequency_band.full_max_hz,

    pub fn valid(self: SpectrogramFrame) bool {
        return self.column_count > 0 and
            self.row_count > 0 and
            self.columns.len >= self.column_count * self.row_count;
    }
};

pub const RecordingSummary = struct {
    label: []const u8,
    created: []const u8 = "",
    seconds: f64 = 0.0,
    loaded: bool = true,
};

pub const RecordingScanState = enum {
    scanning,
    complete,
    failed,
};

pub const TrimState = struct {
    samples: []const f32 = &.{},
    source_sample_count: u64 = 0,
    trim_start_sample: u64 = 0,
    trim_end_sample: u64 = 0,
    trim_end_selected: bool = false,
    playback_sample: u64 = 0,
    playback_visible: bool = false,
};

pub const SpectrumState = struct {
    db_rows: []const f32 = &.{},
};

pub const BandLabState = struct {
    spectrogram: SpectrogramFrame = .{},
    low_hz: f64 = frequency_band.mid_min_hz,
    high_hz: f64 = frequency_band.mid_max_hz,
    upper_selected: bool = false,
    method: band_render.Method = .fft_mask,
    audition: Audition = .original,
};

pub const Audition = enum {
    original,
    selected,
    rejected,

    fn label(self: Audition) []const u8 {
        return switch (self) {
            .original => "ORIG",
            .selected => "BAND",
            .rejected => "REJECT",
        };
    }
};

pub const Snapshot = struct {
    workspace: Workspace = .live,
    mode: AnalysisMode = .tonal,
    frequency_band: FrequencyBand = .whole,
    colormap: Colormap = .viridis,
    sst_enabled: bool = true,
    recording_enabled: bool = false,
    recording_seconds: f64 = 0.0,
    playback_enabled: bool = false,
    min_hz: f64 = frequency_band.full_min_hz,
    max_hz: f64 = frequency_band.full_max_hz,
    custom_range: frequency_band.Range = frequency_band.default_custom,
    waveform: []const f32 = &.{},
    live_frame: SpectrogramFrame = .{},
    recordings: []const RecordingSummary = &.{},
    recording_index: usize = 0,
    active_recording_index: ?usize = null,
    recording_scan_state: RecordingScanState = .complete,
    recording_rename_active: bool = false,
    recording_delete_pending: bool = false,
    trim: TrimState = .{},
    spectrum: SpectrumState = .{},
    band_lab: BandLabState = .{},
};

pub const PendingEvents = struct {
    quit: bool = false,
    workspace: ?Workspace = null,
    mode: ?AnalysisMode = null,
    frequency_band: ?FrequencyBand = null,
    colormap: ?Colormap = null,
    toggle_sst: bool = false,
    toggle_recording: bool = false,
    toggle_playback: bool = false,
    recording_delta: isize = 0,
    select_recording: bool = false,
    begin_recording_rename: bool = false,
    commit_recording_rename: bool = false,
    cancel_recording_rename: bool = false,
    recording_rename_text: [128]u8 = [_]u8{0} ** 128,
    recording_rename_len: usize = 0,
    delete_recording: bool = false,
    cancel_recording_delete: bool = false,
    custom_low_hz: ?f64 = null,
    custom_high_hz: ?f64 = null,
    select_trim_start: bool = false,
    select_trim_end: bool = false,
    trim_delta: isize = 0,
    apply_trim: bool = false,
    clear_trim: bool = false,
    cycle_audition: bool = false,
    cycle_band_method: bool = false,
    cycle_band_edge: bool = false,
    lower_band_delta: isize = 0,
    upper_band_delta: isize = 0,
};

const MenuTab = enum {
    analysis,
    bands,
    colors,

    fn label(self: MenuTab) []const u8 {
        return switch (self) {
            .analysis => "ANALYSIS",
            .bands => "BANDS",
            .colors => "COLORS",
        };
    }

    fn index(self: MenuTab) usize {
        return @intFromEnum(self);
    }
};

const menu_tabs = [_]MenuTab{ .analysis, .bands, .colors };

const analysis_order = [_]AnalysisMode{
    .tonal,
    .transient,
    .sparse,
    .reassigned,
    .squeezed,
    .superlet,
    .multitaper,
    .s_transform,
};

const MenuState = struct {
    open: bool = false,
    tab: MenuTab = .analysis,
    cursors: [menu_tabs.len]usize = [_]usize{0} ** menu_tabs.len,
    custom_low_storage: [32]u8 = [_]u8{0} ** 32,
    custom_high_storage: [32]u8 = [_]u8{0} ** 32,
    custom_low_len: usize = 0,
    custom_high_len: usize = 0,

    fn openWithSnapshot(self: *MenuState, snapshot: *const Snapshot) void {
        self.open = true;
        self.cursors[MenuTab.analysis.index()] = analysisCursorForMode(snapshot.mode);
        self.cursors[MenuTab.bands.index()] = snapshot.frequency_band.index();
        self.cursors[MenuTab.colors.index()] = snapshot.colormap.index();
        self.formatCustom(snapshot.custom_range);
    }

    fn formatCustom(self: *MenuState, custom: frequency_band.Range) void {
        self.custom_low_len = writeFrequencyNumber(&self.custom_low_storage, custom.min_hz);
        self.custom_high_len = writeFrequencyNumber(&self.custom_high_storage, custom.max_hz);
    }

    fn rowCount(self: *const MenuState) usize {
        return switch (self.tab) {
            .analysis => analysis_order.len + 1,
            .bands => frequency_band.count + 2,
            .colors => colormap.count,
        };
    }

    fn moveCursor(self: *MenuState, delta: isize) void {
        var cursor = layout.ListCursor{ .index = self.cursors[self.tab.index()] };
        cursor.move(delta, self.rowCount());
        self.cursors[self.tab.index()] = cursor.index;
    }

    fn switchTab(self: *MenuState, delta: isize) void {
        const count: isize = @intCast(menu_tabs.len);
        var index: isize = @as(isize, @intCast(self.tab.index())) + delta;
        while (index < 0) index += count;
        self.tab = menu_tabs[@intCast(@mod(index, count))];
        if (self.cursors[self.tab.index()] >= self.rowCount()) {
            self.cursors[self.tab.index()] = self.rowCount() - 1;
        }
    }
};

const LiveHistory = struct {
    allocator: std.mem.Allocator,
    cells: []f32 = &.{},
    column_capacity: usize = 0,
    row_count: usize = 0,
    filled_columns: usize = 0,
    min_hz: f64 = frequency_band.full_min_hz,
    max_hz: f64 = frequency_band.full_max_hz,

    fn init(allocator: std.mem.Allocator) LiveHistory {
        return .{ .allocator = allocator };
    }

    fn deinit(self: *LiveHistory) void {
        if (self.cells.len > 0) self.allocator.free(self.cells);
        self.* = undefined;
    }

    fn ensure(self: *LiveHistory, columns: usize, rows: usize) !void {
        if (columns == 0 or rows == 0) return;
        if (columns > std.math.maxInt(usize) / rows) return Error.TooLarge;
        if (columns == self.column_capacity and rows == self.row_count) return;

        if (self.cells.len > 0) self.allocator.free(self.cells);
        self.cells = try self.allocator.alloc(f32, columns * rows);
        @memset(self.cells, draw.floor_db);
        self.column_capacity = columns;
        self.row_count = rows;
        self.filled_columns = 0;
    }

    fn append(self: *LiveHistory, frame: SpectrogramFrame, visible_columns: usize) !void {
        if (!frame.valid() or visible_columns == 0) return;
        try self.ensure(visible_columns, frame.row_count);
        self.min_hz = frame.min_hz;
        self.max_hz = frame.max_hz;

        const start = if (frame.column_count > self.column_capacity)
            frame.column_count - self.column_capacity
        else
            0;

        for (start..frame.column_count) |column| {
            self.appendColumn(frame.columns[column * frame.row_count ..][0..frame.row_count]);
        }
    }

    fn appendColumn(self: *LiveHistory, column: []const f32) void {
        if (self.column_capacity == 0 or self.row_count == 0) return;

        const destination = if (self.filled_columns < self.column_capacity) blk: {
            const index = self.filled_columns;
            self.filled_columns += 1;
            break :blk index;
        } else blk: {
            std.mem.copyForwards(
                f32,
                self.cells[0 .. (self.column_capacity - 1) * self.row_count],
                self.cells[self.row_count .. self.column_capacity * self.row_count],
            );
            break :blk self.column_capacity - 1;
        };

        @memcpy(
            self.cells[destination * self.row_count ..][0..self.row_count],
            column[0..self.row_count],
        );
    }

    fn visible(self: *const LiveHistory) SpectrogramFrame {
        return .{
            .columns = self.cells[0 .. self.filled_columns * self.row_count],
            .column_count = self.filled_columns,
            .row_count = self.row_count,
            .min_hz = self.min_hz,
            .max_hz = self.max_hz,
        };
    }
};

pub const Ui = struct {
    allocator: std.mem.Allocator,
    window: *sdl.SDL_Window,
    renderer: *sdl.SDL_Renderer,
    texture: ?*sdl.SDL_Texture = null,
    pixels: []draw.Color = &.{},
    width: i32 = 0,
    height: i32 = 0,
    input: core.Input = .{},
    imui: core.Context = .{},
    events: PendingEvents = .{},
    menu: MenuState = .{},
    live: LiveHistory,
    rename_storage: [128]u8 = [_]u8{0} ** 128,
    rename_len: usize = 0,
    rename_index: ?usize = null,

    pub fn init(allocator: std.mem.Allocator, window: *sdl.SDL_Window) !Ui {
        const renderer = sdl.SDL_CreateRenderer(window, null) orelse {
            std.log.err("SDL_CreateRenderer failed: {s}", .{sdl.SDL_GetError()});
            return Error.SdlRenderer;
        };
        errdefer sdl.SDL_DestroyRenderer(renderer);

        _ = sdl.SDL_StartTextInput(window);

        return .{
            .allocator = allocator,
            .window = window,
            .renderer = renderer,
            .live = LiveHistory.init(allocator),
        };
    }

    pub fn deinit(self: *Ui) void {
        self.live.deinit();
        if (self.texture) |texture| sdl.SDL_DestroyTexture(texture);
        if (self.pixels.len > 0) self.allocator.free(self.pixels);
        sdl.SDL_DestroyRenderer(self.renderer);
        self.* = undefined;
    }

    pub fn beginFrame(self: *Ui) void {
        self.input.beginFrame();
        self.events = .{};
    }

    pub fn handleEvent(self: *Ui, event: *const sdl.SDL_Event, snapshot: *const Snapshot) void {
        switch (event.type) {
            sdl.SDL_EVENT_QUIT => self.events.quit = true,
            sdl.SDL_EVENT_MOUSE_MOTION => {
                self.input.mouse_x = @intFromFloat(@round(event.motion.x));
                self.input.mouse_y = @intFromFloat(@round(event.motion.y));
            },
            sdl.SDL_EVENT_MOUSE_BUTTON_DOWN => if (event.button.button == sdl.SDL_BUTTON_LEFT) {
                self.input.mouse_x = @intFromFloat(@round(event.button.x));
                self.input.mouse_y = @intFromFloat(@round(event.button.y));
                self.input.mouse_left_down = true;
                self.input.mouse_left_pressed = true;
            },
            sdl.SDL_EVENT_MOUSE_BUTTON_UP => if (event.button.button == sdl.SDL_BUTTON_LEFT) {
                self.input.mouse_x = @intFromFloat(@round(event.button.x));
                self.input.mouse_y = @intFromFloat(@round(event.button.y));
                self.input.mouse_left_down = false;
                self.input.mouse_left_released = true;
            },
            sdl.SDL_EVENT_MOUSE_WHEEL => {
                const wheel = if (event.wheel.integer_y != 0)
                    event.wheel.integer_y
                else
                    @as(i32, @intFromFloat(@round(event.wheel.y)));
                self.input.wheel_y += wheel;
            },
            sdl.SDL_EVENT_TEXT_INPUT => {
                if (event.text.text) |text_ptr| self.input.appendText(std.mem.span(text_ptr));
            },
            sdl.SDL_EVENT_KEY_DOWN => self.handleKeyDown(event.key.key, event.key.repeat, snapshot),
            else => {},
        }
    }

    pub fn render(self: *Ui, snapshot: *const Snapshot) !PendingEvents {
        try self.ensureRenderTarget();
        var buffer = draw.PixelBuffer{ .pixels = self.pixels, .width = self.width, .height = self.height };
        const frame_layout = layout.frame(self.width, self.height);
        const metrics = layout.Metrics.forSize(self.width, self.height);

        if (snapshot.live_frame.valid()) {
            try self.live.append(snapshot.live_frame, @intCast(@max(0, frame_layout.plot.width)));
        }

        buffer.fill(draw.palette.background);
        self.imui.setTextScale(metrics.scale);
        self.imui.begin(if (self.menu.open) null else &self.input);
        self.drawBanner(&buffer, frame_layout.banner, metrics, snapshot);
        self.drawWaveform(&buffer, frame_layout.waveform, snapshot);
        self.drawWorkspace(&buffer, frame_layout, metrics, snapshot);
        self.imui.end();

        if (self.menu.open) {
            self.imui.begin(&self.input);
            self.drawMenu(&buffer, metrics, snapshot);
            self.imui.end();
        }

        self.drawHints(&buffer, frame_layout.hints, metrics, snapshot);
        try self.present();
        return self.events;
    }

    fn handleKeyDown(self: *Ui, key: sdl.SDL_Keycode, repeat: bool, snapshot: *const Snapshot) void {
        _ = repeat;
        switch (key) {
            sdl.SDLK_LEFT => self.input.key_left = true,
            sdl.SDLK_RIGHT => self.input.key_right = true,
            sdl.SDLK_UP => self.input.key_up = true,
            sdl.SDLK_DOWN => self.input.key_down = true,
            sdl.SDLK_HOME => self.input.key_home = true,
            sdl.SDLK_END => self.input.key_end = true,
            sdl.SDLK_BACKSPACE => self.input.key_backspace = true,
            sdl.SDLK_DELETE => self.input.key_delete = true,
            sdl.SDLK_RETURN => self.input.key_enter = true,
            sdl.SDLK_ESCAPE => self.input.key_escape = true,
            sdl.SDLK_TAB => self.input.key_tab = true,
            else => {},
        }

        if (self.imui.hasTextFocus()) {
            if (key == sdl.SDLK_ESCAPE and snapshot.recording_rename_active) {
                self.events.cancel_recording_rename = true;
            }
            return;
        }

        if (key == sdl.SDLK_ESCAPE and snapshot.recording_rename_active) {
            self.events.cancel_recording_rename = true;
            return;
        }

        if (self.menu.open) {
            switch (key) {
                sdl.SDLK_ESCAPE, sdl.SDLK_M => self.menu.open = false,
                sdl.SDLK_UP => self.menu.moveCursor(-1),
                sdl.SDLK_DOWN => self.menu.moveCursor(1),
                sdl.SDLK_LEFT => self.menu.switchTab(-1),
                sdl.SDLK_RIGHT, sdl.SDLK_TAB => self.menu.switchTab(1),
                sdl.SDLK_RETURN => self.commitMenuCursor(snapshot),
                else => {},
            }
            return;
        }

        switch (key) {
            sdl.SDLK_ESCAPE => {
                if (snapshot.recording_delete_pending) {
                    self.events.cancel_recording_delete = true;
                } else {
                    self.events.quit = true;
                }
            },
            sdl.SDLK_Q => self.events.quit = true,
            sdl.SDLK_M => self.menu.openWithSnapshot(snapshot),
            sdl.SDLK_TAB => self.events.workspace = nextWorkspace(snapshot.workspace, 1),
            sdl.SDLK_L => self.events.workspace = .live,
            sdl.SDLK_V => self.events.workspace = .recordings,
            sdl.SDLK_T => self.events.workspace = .trim,
            sdl.SDLK_O => self.events.workspace = .spectrum,
            sdl.SDLK_B => self.events.workspace = .band_lab,
            sdl.SDLK_R => self.events.toggle_recording = true,
            sdl.SDLK_P, sdl.SDLK_SPACE => self.events.toggle_playback = true,
            sdl.SDLK_N => {
                if (snapshot.workspace == .recordings) self.events.begin_recording_rename = true;
            },
            sdl.SDLK_D => {
                if (snapshot.workspace == .recordings) self.events.delete_recording = true;
            },
            sdl.SDLK_RETURN => {
                if (snapshot.workspace == .recordings) self.events.select_recording = true;
            },
            sdl.SDLK_LEFT => {
                if (snapshot.workspace == .trim) self.events.trim_delta -= 1;
            },
            sdl.SDLK_RIGHT => {
                if (snapshot.workspace == .trim) self.events.trim_delta += 1;
            },
            sdl.SDLK_UP => self.handleWorkspaceUpDown(snapshot, -1),
            sdl.SDLK_DOWN => self.handleWorkspaceUpDown(snapshot, 1),
            sdl.SDLK_BACKSPACE => {
                if (snapshot.workspace == .trim) self.events.clear_trim = true;
            },
            sdl.SDLK_COMMA => {
                if (snapshot.workspace == .trim) self.events.select_trim_start = true;
            },
            sdl.SDLK_PERIOD => {
                if (snapshot.workspace == .trim) self.events.select_trim_end = true;
            },
            sdl.SDLK_SLASH => {
                if (snapshot.workspace == .trim) self.events.apply_trim = true;
            },
            sdl.SDLK_LEFTBRACKET => {
                if (snapshot.workspace == .band_lab) self.events.lower_band_delta -= 1;
            },
            sdl.SDLK_RIGHTBRACKET => {
                if (snapshot.workspace == .band_lab) self.events.lower_band_delta += 1;
            },
            sdl.SDLK_MINUS => {
                if (snapshot.workspace == .band_lab) self.events.upper_band_delta -= 1;
            },
            sdl.SDLK_EQUALS => {
                if (snapshot.workspace == .band_lab) self.events.upper_band_delta += 1;
            },
            sdl.SDLK_A => {
                if (snapshot.workspace == .band_lab) self.events.cycle_audition = true;
            },
            sdl.SDLK_F => {
                if (snapshot.workspace == .band_lab) self.events.cycle_band_method = true;
            },
            sdl.SDLK_H => {
                if (snapshot.workspace == .band_lab) self.events.cycle_band_edge = true;
            },
            sdl.SDLK_1, sdl.SDLK_2, sdl.SDLK_3, sdl.SDLK_4, sdl.SDLK_5, sdl.SDLK_6, sdl.SDLK_7, sdl.SDLK_8 => {
                const index: usize = @intCast(key - sdl.SDLK_1);
                self.events.mode = analysis_order[index];
            },
            else => {},
        }
    }

    fn handleWorkspaceUpDown(self: *Ui, snapshot: *const Snapshot, delta: isize) void {
        switch (snapshot.workspace) {
            .recordings => self.events.recording_delta += delta,
            .trim => self.events.trim_delta += delta,
            .band_lab => {
                if (snapshot.band_lab.upper_selected) {
                    self.events.upper_band_delta += delta;
                } else {
                    self.events.lower_band_delta += delta;
                }
            },
            else => {},
        }
    }

    fn commitMenuCursor(self: *Ui, snapshot: *const Snapshot) void {
        _ = snapshot;
        const cursor = self.menu.cursors[self.menu.tab.index()];
        switch (self.menu.tab) {
            .analysis => {
                if (cursor < analysis_order.len) {
                    self.events.mode = analysis_order[cursor];
                    self.menu.open = false;
                } else {
                    self.events.toggle_sst = true;
                }
            },
            .bands => {
                if (cursor < frequency_band.count) {
                    self.events.frequency_band = frequency_band.order[cursor];
                    self.menu.open = false;
                } else if (cursor == frequency_band.count) {
                    self.imui.focusTextField("custom-low", self.menu.custom_low_storage[0..self.menu.custom_low_len]);
                } else {
                    self.imui.focusTextField("custom-high", self.menu.custom_high_storage[0..self.menu.custom_high_len]);
                }
            },
            .colors => {
                if (cursor < colormap.count) {
                    self.events.colormap = colormap.order[cursor];
                    self.menu.open = false;
                }
            },
        }
    }

    fn drawBanner(
        self: *Ui,
        buffer: *draw.PixelBuffer,
        bounds: layout.Rect,
        metrics: layout.Metrics,
        snapshot: *const Snapshot,
    ) void {
        buffer.fillRect(bounds, draw.palette.banner);
        const scale = metrics.scale;
        var x = metrics.pad;
        const tab_y = bounds.y + @divTrunc(bounds.height - font.textHeight(scale), 2);

        self.imui.pushId("workspace-tabs");
        for (layout.workspace_order) |workspace| {
            const active = workspace == snapshot.workspace;
            const label = workspace.label();
            const width = font.textWidth(label, scale) + 10 * scale;
            const hit = layout.rect(x, bounds.y + 4 * scale, width, bounds.height - 8 * scale);
            const widget = self.imui.hitRect(label, hit);
            if (widget.fired) self.events.workspace = workspace;
            if (active or widget.hovered) buffer.fillRect(hit, if (active) draw.palette.selected else draw.palette.hover);
            buffer.drawText(label, x + 5 * scale, tab_y, scale, if (active) draw.palette.text else draw.palette.text_dim);
            x += width + metrics.gap;
        }
        self.imui.popId();

        if (snapshot.recording_enabled) {
            var rec_buf: [32]u8 = undefined;
            const rec = std.fmt.bufPrint(&rec_buf, "REC {s}", .{layout.formatDuration(snapshot.recording_seconds, rec_buf[16..])}) catch "REC";
            const rec_width = font.textWidth(rec, scale);
            const rec_x = @divTrunc(bounds.width - rec_width, 2);
            if (rec_x > x + metrics.gap) buffer.drawText(rec, rec_x, tab_y, scale, draw.palette.record);
        }

        var status_buf: [96]u8 = undefined;
        const status = std.fmt.bufPrint(
            &status_buf,
            "{s}  {s}  {s}",
            .{ modeBanner(snapshot.mode, snapshot.sst_enabled), snapshot.frequency_band.name(), snapshot.colormap.name() },
        ) catch "";
        const status_width = font.textWidth(status, scale);
        const status_x = bounds.right() - status_width - metrics.pad;
        if (status_x > x) buffer.drawText(status, status_x, tab_y, scale, draw.palette.text_dim);
    }

    fn drawWaveform(self: *Ui, buffer: *draw.PixelBuffer, bounds: layout.Rect, snapshot: *const Snapshot) void {
        _ = self;
        buffer.fillRect(bounds, draw.palette.panel);
        buffer.drawWaveform(snapshot.waveform, bounds.inset(2), draw.palette.waveform);
        buffer.fillRect(layout.rect(bounds.x, bounds.bottom() - 1, bounds.width, 1), draw.palette.separator);
    }

    fn drawWorkspace(
        self: *Ui,
        buffer: *draw.PixelBuffer,
        frame_layout: layout.FrameLayout,
        metrics: layout.Metrics,
        snapshot: *const Snapshot,
    ) void {
        switch (snapshot.workspace) {
            .live => self.drawLive(buffer, frame_layout, metrics, snapshot),
            .recordings => self.drawRecordings(buffer, frame_layout.main, metrics, snapshot),
            .trim => self.drawTrim(buffer, frame_layout.main, metrics, snapshot),
            .spectrum => self.drawSpectrum(buffer, frame_layout, metrics, snapshot),
            .band_lab => self.drawBandLab(buffer, frame_layout, metrics, snapshot),
        }
    }

    fn drawLive(self: *Ui, buffer: *draw.PixelBuffer, frame_layout: layout.FrameLayout, metrics: layout.Metrics, snapshot: *const Snapshot) void {
        self.drawAxis(buffer, frame_layout.axis, metrics, snapshot.min_hz, snapshot.max_hz);
        const visible = self.live.visible();
        if (visible.valid()) {
            buffer.drawSpectrogram(
                visible.columns,
                visible.column_count,
                visible.row_count,
                frame_layout.plot,
                snapshot.colormap,
                visible.min_hz,
                visible.max_hz,
            );
        } else {
            buffer.fillRect(frame_layout.plot, draw.palette.background);
            self.centerMessage(buffer, frame_layout.plot, metrics.scale, "WAITING FOR AUDIO");
        }
    }

    fn drawRecordings(self: *Ui, buffer: *draw.PixelBuffer, bounds: layout.Rect, metrics: layout.Metrics, snapshot: *const Snapshot) void {
        const scale = metrics.scale;
        const inner = bounds.inset(metrics.pad);
        var y = inner.y;
        var line_buf: [128]u8 = undefined;
        const title = std.fmt.bufPrint(&line_buf, "RECS  {d} FILES", .{snapshot.recordings.len}) catch "RECS";
        buffer.drawText(title, inner.x, y, scale, draw.palette.text);
        y += (font.glyph_height + 8) * scale;

        const status: ?[]const u8 = switch (snapshot.recording_scan_state) {
            .failed => "SCAN FAILED",
            .scanning => "SCANNING recordings/",
            .complete => if (snapshot.recordings.len == 0) "NO WAV RECORDINGS" else null,
        };
        if (status) |text| {
            buffer.drawText(text, inner.x, y, scale, if (snapshot.recording_scan_state == .failed) draw.palette.record else draw.palette.text_dim);
            y += (font.glyph_height + 8) * scale;
        }

        if (snapshot.recordings.len == 0) return;

        const line_height = (font.glyph_height + 7) * scale;
        const max_rows = @max(1, @divTrunc(bounds.bottom() - y - metrics.pad, line_height));
        const first = if (snapshot.recording_index >= @as(usize, @intCast(max_rows)))
            snapshot.recording_index - @as(usize, @intCast(max_rows)) + 1
        else
            0;

        self.imui.pushId("recordings");
        var row: i32 = 0;
        while (row < max_rows) : (row += 1) {
            const index = first + @as(usize, @intCast(row));
            if (index >= snapshot.recordings.len) break;
            const summary = snapshot.recordings[index];
            const selected = index == snapshot.recording_index;
            const active = if (snapshot.active_recording_index) |active_index| active_index == index else false;
            const row_bounds = widgets.rowRect(inner.x, y, inner.width, scale, line_height);

            var id_buf: [32]u8 = undefined;
            const row_id = std.fmt.bufPrint(&id_buf, "row-{d}", .{index}) catch "row";
            self.imui.pushId(row_id);
            defer self.imui.popId();

            if (selected and snapshot.recording_rename_active) {
                self.drawRenameField(buffer, row_bounds, scale, summary, index);
            } else {
                var duration_buf: [32]u8 = undefined;
                const duration = layout.formatDuration(summary.seconds, &duration_buf);
                var row_buf: [192]u8 = undefined;
                const marker = if (selected) ">" else " ";
                const active_marker = if (active) "*" else " ";
                const metadata = if (summary.loaded) "" else " metadata";
                const text = std.fmt.bufPrint(
                    &row_buf,
                    "{s}{s}  {s}  {s}  {s}{s}",
                    .{ marker, active_marker, summary.label, duration, summary.created, metadata },
                ) catch summary.label;
                if (widgets.listRow(&self.imui, buffer, "body", text, selected, row_bounds)) {
                    if (selected) {
                        self.events.select_recording = true;
                    } else {
                        self.events.recording_delta += deltaBetween(snapshot.recording_index, index);
                    }
                }
                if (selected and snapshot.recording_delete_pending) {
                    buffer.drawText("CONFIRM D / ESC", row_bounds.right() - font.textWidth("CONFIRM D / ESC", scale) - 6 * scale, y, scale, draw.palette.record);
                }
            }
            y += line_height;
        }
        self.imui.popId();
    }

    fn drawRenameField(
        self: *Ui,
        buffer: *draw.PixelBuffer,
        bounds: layout.Rect,
        scale: i32,
        summary: RecordingSummary,
        index: usize,
    ) void {
        if (self.rename_index == null or self.rename_index.? != index) {
            const stem = stripWav(summary.label);
            self.rename_len = @min(stem.len, self.rename_storage.len);
            @memcpy(self.rename_storage[0..self.rename_len], stem[0..self.rename_len]);
            self.rename_index = index;
            self.imui.focusTextField("rename", self.rename_storage[0..self.rename_len]);
        }

        var value = core.TextBuffer{ .storage = &self.rename_storage, .len = self.rename_len };
        const result = widgets.textField(&self.imui, buffer, "rename", &value, bounds);
        self.rename_len = value.len;
        if (result.committed) {
            self.events.commit_recording_rename = true;
            self.events.recording_rename_len = @min(self.rename_len, self.events.recording_rename_text.len);
            @memcpy(
                self.events.recording_rename_text[0..self.events.recording_rename_len],
                self.rename_storage[0..self.events.recording_rename_len],
            );
            self.rename_index = null;
            self.imui.clearFocus();
        }
        if (self.input.key_escape) {
            self.events.cancel_recording_rename = true;
            self.rename_index = null;
            self.imui.clearFocus();
        }
        _ = scale;
    }

    fn drawTrim(self: *Ui, buffer: *draw.PixelBuffer, bounds: layout.Rect, metrics: layout.Metrics, snapshot: *const Snapshot) void {
        const inner = bounds.inset(metrics.pad);
        if (snapshot.trim.samples.len == 0) {
            self.centerMessage(buffer, inner, metrics.scale, "NO CLIP LOADED");
            return;
        }

        buffer.drawWaveform(snapshot.trim.samples, inner, draw.palette.waveform);
        self.drawTrimMarkers(buffer, inner, metrics.scale, snapshot.trim);
    }

    fn drawSpectrum(self: *Ui, buffer: *draw.PixelBuffer, frame_layout: layout.FrameLayout, metrics: layout.Metrics, snapshot: *const Snapshot) void {
        self.drawAxis(buffer, frame_layout.axis, metrics, snapshot.min_hz, snapshot.max_hz);
        if (snapshot.spectrum.db_rows.len == 0) {
            self.centerMessage(buffer, frame_layout.plot, metrics.scale, "NO SPECTRUM");
            return;
        }
        buffer.drawSpectrumBars(snapshot.spectrum.db_rows, frame_layout.plot, snapshot.colormap, snapshot.min_hz, snapshot.max_hz);
    }

    fn drawBandLab(self: *Ui, buffer: *draw.PixelBuffer, frame_layout: layout.FrameLayout, metrics: layout.Metrics, snapshot: *const Snapshot) void {
        self.drawAxis(buffer, frame_layout.axis, metrics, snapshot.min_hz, snapshot.max_hz);
        const plot = frame_layout.plot;
        const band = snapshot.band_lab;
        if (band.spectrogram.valid()) {
            buffer.drawSpectrogram(
                band.spectrogram.columns,
                band.spectrogram.column_count,
                band.spectrogram.row_count,
                plot,
                snapshot.colormap,
                band.spectrogram.min_hz,
                band.spectrogram.max_hz,
            );
        } else if (snapshot.spectrum.db_rows.len > 0) {
            buffer.drawSpectrumBars(snapshot.spectrum.db_rows, plot, snapshot.colormap, snapshot.min_hz, snapshot.max_hz);
        } else {
            self.centerMessage(buffer, plot, metrics.scale, "NO BAND DATA");
        }
        self.drawBandMarkers(buffer, plot, metrics.scale, snapshot);
    }

    fn drawAxis(self: *Ui, buffer: *draw.PixelBuffer, bounds: layout.Rect, metrics: layout.Metrics, min_hz: f64, max_hz: f64) void {
        _ = self;
        buffer.fillRect(bounds, draw.palette.banner);
        const scale = metrics.scale;
        var last_bottom = bounds.y - 4 * scale;
        for (layout.frequency_ticks) |tick| {
            if (tick.hz < min_hz or tick.hz > max_hz) continue;
            const row = layout.rowForFrequency(min_hz, max_hz, tick.hz, bounds.height) orelse continue;
            const y = bounds.y + row;
            buffer.fillRect(layout.rect(bounds.right() - 5 * scale, y, 5 * scale, 1), draw.palette.border);
            const text_height = font.textHeight(scale);
            var text_y = y - @divTrunc(text_height, 2);
            text_y = std.math.clamp(text_y, bounds.y, bounds.bottom() - text_height);
            if (text_y < last_bottom + 3 * scale) continue;
            const text_x = bounds.right() - 8 * scale - font.textWidth(tick.label, scale);
            buffer.drawText(tick.label, @max(bounds.x + scale, text_x), text_y, scale, draw.palette.text_dim);
            last_bottom = text_y + text_height;
        }
    }

    fn drawTrimMarkers(self: *Ui, buffer: *draw.PixelBuffer, area: layout.Rect, scale: i32, trim: TrimState) void {
        _ = self;
        const source_count = if (trim.source_sample_count > 0) trim.source_sample_count else trim.samples.len;
        if (source_count == 0) return;
        const start_x = layout.sampleMarkerX(area, trim.trim_start_sample, source_count);
        const end_sample = if (trim.trim_end_sample == 0) source_count else trim.trim_end_sample;
        const end_x = layout.sampleMarkerX(area, end_sample, source_count);
        buffer.blendRect(layout.rect(area.x, area.y, @max(0, start_x - area.x), area.height), draw.palette.background, 0.55);
        buffer.blendRect(layout.rect(end_x, area.y, @max(0, area.right() - end_x), area.height), draw.palette.background, 0.55);
        buffer.fillRect(layout.rect(start_x, area.y, @max(1, scale), area.height), if (trim.trim_end_selected) draw.palette.marker_dim else draw.palette.marker);
        buffer.fillRect(layout.rect(end_x, area.y, @max(1, scale), area.height), if (trim.trim_end_selected) draw.palette.marker else draw.palette.marker_dim);
        buffer.drawText("START", start_x + 4 * scale, area.y + 4 * scale, scale, draw.palette.marker);
        buffer.drawText("END", end_x + 4 * scale, area.y + (font.glyph_height + 8) * scale, scale, draw.palette.marker_dim);
        if (trim.playback_visible) {
            const play_x = layout.sampleMarkerX(area, trim.playback_sample, source_count);
            buffer.fillRect(layout.rect(play_x, area.y, @max(1, scale), area.height), draw.palette.playhead);
        }
    }

    fn drawBandMarkers(self: *Ui, buffer: *draw.PixelBuffer, area: layout.Rect, scale: i32, snapshot: *const Snapshot) void {
        _ = self;
        const band = snapshot.band_lab;
        const low_row = layout.rowForFrequency(snapshot.min_hz, snapshot.max_hz, band.low_hz, area.height) orelse return;
        const high_row = layout.rowForFrequency(snapshot.min_hz, snapshot.max_hz, band.high_hz, area.height) orelse return;
        const low_y = area.y + low_row;
        const high_y = area.y + high_row;
        buffer.blendRect(layout.rect(area.x, area.y, area.width, @min(low_y, high_y) - area.y), draw.palette.background, 0.35);
        buffer.blendRect(layout.rect(area.x, @max(low_y, high_y), area.width, area.bottom() - @max(low_y, high_y)), draw.palette.background, 0.35);
        buffer.fillRect(layout.rect(area.x, low_y, area.width, @max(1, scale)), if (band.upper_selected) draw.palette.marker_dim else draw.palette.marker);
        buffer.fillRect(layout.rect(area.x, high_y, area.width, @max(1, scale)), if (band.upper_selected) draw.palette.marker else draw.palette.marker_dim);

        var readout: [128]u8 = undefined;
        const text = std.fmt.bufPrint(
            &readout,
            "LOW {d:.0}  HIGH {d:.0}  {s}  {s}",
            .{ band.low_hz, band.high_hz, band_render.methodShortName(band.method), band.audition.label() },
        ) catch "";
        buffer.fillRect(layout.rect(area.x, area.y, area.width, (font.glyph_height + 8) * scale), draw.blend(draw.palette.panel, draw.palette.background, 0.25));
        buffer.drawText(text, area.x + 8 * scale, area.y + 4 * scale, scale, draw.palette.text);
    }

    fn drawMenu(self: *Ui, buffer: *draw.PixelBuffer, metrics: layout.Metrics, snapshot: *const Snapshot) void {
        buffer.dim();
        const scale = metrics.scale;
        const line_height = (font.glyph_height + 6) * scale;
        const padding = 16 * scale;
        const visible_rows: i32 = @intCast(@max(7, self.menu.rowCount() + 4));
        const panel = layout.centeredPanel(
            self.width,
            self.height,
            620 * scale,
            visible_rows * line_height + padding * 2,
            12 * scale,
        );
        buffer.fillRect(panel, draw.palette.panel);
        buffer.outline(panel, @max(1, scale), draw.palette.border);

        var y = panel.y + padding;
        const x = panel.x + padding;
        const width = panel.width - padding * 2;
        buffer.drawText("SOUNDS", x, y, scale, draw.palette.text);
        y += line_height;

        const tab_gap = 6 * scale;
        const tab_width = @divTrunc(width - tab_gap * 2, 3);
        for (menu_tabs, 0..) |tab, index| {
            const tab_rect = layout.rect(x + @as(i32, @intCast(index)) * (tab_width + tab_gap), y - 2 * scale, tab_width, line_height);
            const selected = self.menu.tab == tab;
            if (widgets.listRow(&self.imui, buffer, tab.label(), tab.label(), selected, tab_rect)) self.menu.tab = tab;
        }
        y += line_height * 2;

        switch (self.menu.tab) {
            .analysis => self.drawAnalysisMenu(buffer, x, y, width, scale, line_height, snapshot),
            .bands => self.drawBandsMenu(buffer, x, y, width, scale, line_height, snapshot),
            .colors => self.drawColorsMenu(buffer, panel, x, y, width, scale, line_height, snapshot),
        }
    }

    fn drawAnalysisMenu(self: *Ui, buffer: *draw.PixelBuffer, x: i32, y_arg: i32, width: i32, scale: i32, line_height: i32, snapshot: *const Snapshot) void {
        var y = y_arg;
        const cursor = self.menu.cursors[MenuTab.analysis.index()];
        for (analysis_order, 0..) |mode, index| {
            var text_buf: [96]u8 = undefined;
            const text = std.fmt.bufPrint(
                &text_buf,
                "{s} {d}  {s}",
                .{ if (mode == snapshot.mode) "SET" else "   ", index + 1, modeTitle(mode, snapshot.sst_enabled) },
            ) catch "";
            const row = widgets.rowRect(x, y, width, scale, line_height);
            if (widgets.listRow(&self.imui, buffer, text, text, cursor == index, row)) {
                self.menu.cursors[MenuTab.analysis.index()] = index;
                self.events.mode = mode;
                self.menu.open = false;
            }
            y += line_height;
        }

        var tonal_buf: [64]u8 = undefined;
        const tonal = std.fmt.bufPrint(&tonal_buf, "SET TONAL VIEW  {s}", .{if (snapshot.sst_enabled) "SST" else "RAW"}) catch "TONAL VIEW";
        if (widgets.listRow(&self.imui, buffer, "tonal-view", tonal, cursor == analysis_order.len, widgets.rowRect(x, y, width, scale, line_height))) {
            self.menu.cursors[MenuTab.analysis.index()] = analysis_order.len;
            self.events.toggle_sst = true;
        }
    }

    fn drawBandsMenu(self: *Ui, buffer: *draw.PixelBuffer, x: i32, y_arg: i32, width: i32, scale: i32, line_height: i32, snapshot: *const Snapshot) void {
        var y = y_arg;
        const cursor = self.menu.cursors[MenuTab.bands.index()];
        for (frequency_band.order, 0..) |band, index| {
            var text_buf: [128]u8 = undefined;
            const range = if (band == .custom) "typed range" else band.rangeLabel();
            const text = std.fmt.bufPrint(
                &text_buf,
                "{s} {s}  {s}",
                .{ if (band == snapshot.frequency_band) "SET" else "   ", band.title(), range },
            ) catch "";
            if (widgets.listRow(&self.imui, buffer, text, text, cursor == index, widgets.rowRect(x, y, width, scale, line_height))) {
                self.menu.cursors[MenuTab.bands.index()] = index;
                self.events.frequency_band = band;
                self.menu.open = false;
            }
            y += line_height;
        }

        self.drawCustomFrequencyRow(buffer, x, y, width, scale, line_height, false, cursor == frequency_band.count);
        y += line_height;
        self.drawCustomFrequencyRow(buffer, x, y, width, scale, line_height, true, cursor == frequency_band.count + 1);
    }

    fn drawCustomFrequencyRow(self: *Ui, buffer: *draw.PixelBuffer, x: i32, y: i32, width: i32, scale: i32, line_height: i32, high: bool, cursor: bool) void {
        const label = if (high) "custom high" else "custom low";
        const field_width = 96 * scale;
        const field = layout.rect(x + width - field_width - font.textWidth("HZ", scale), y - 2 * scale, field_width, line_height);
        const row = widgets.rowRect(x, y, width, scale, line_height);
        if (cursor) buffer.fillRect(row, draw.palette.selected);
        buffer.drawText(label, x + core.padding(scale), y + @divTrunc(line_height - font.textHeight(scale), 2), scale, if (cursor) draw.palette.text else draw.palette.text_dim);

        var value = if (high)
            core.TextBuffer{ .storage = &self.menu.custom_high_storage, .len = self.menu.custom_high_len }
        else
            core.TextBuffer{ .storage = &self.menu.custom_low_storage, .len = self.menu.custom_low_len };
        const result = widgets.textField(&self.imui, buffer, if (high) "custom-high" else "custom-low", &value, field);
        if (high) {
            self.menu.custom_high_len = value.len;
        } else {
            self.menu.custom_low_len = value.len;
        }
        if (result.focused) self.menu.cursors[MenuTab.bands.index()] = if (high) frequency_band.count + 1 else frequency_band.count;
        if (result.committed) {
            const parsed = std.fmt.parseFloat(f64, value.slice()) catch null;
            if (parsed) |hz| {
                if (high) self.events.custom_high_hz = hz else self.events.custom_low_hz = hz;
            }
            self.imui.clearFocus();
        }
        buffer.drawText("HZ", field.right() + 4 * scale, y + @divTrunc(line_height - font.textHeight(scale), 2), scale, draw.palette.text_dim);
    }

    fn drawColorsMenu(self: *Ui, buffer: *draw.PixelBuffer, panel: layout.Rect, x: i32, y_arg: i32, width: i32, scale: i32, line_height: i32, snapshot: *const Snapshot) void {
        var y = y_arg;
        const cursor = self.menu.cursors[MenuTab.colors.index()];
        for (colormap.order, 0..) |map, index| {
            var text_buf: [64]u8 = undefined;
            const text = std.fmt.bufPrint(&text_buf, "{s} {s}", .{ if (map == snapshot.colormap) "SET" else "   ", map.name() }) catch "";
            if (widgets.listRow(&self.imui, buffer, text, text, cursor == index, widgets.rowRect(x, y, width, scale, line_height))) {
                self.menu.cursors[MenuTab.colors.index()] = index;
                self.events.colormap = map;
                self.menu.open = false;
            }
            const preview = layout.rect(panel.right() - 112 * scale, y + 2 * scale, 92 * scale, 5 * scale);
            buffer.drawColormapPreview(map, preview);
            y += line_height;
        }
    }

    fn drawHints(self: *Ui, buffer: *draw.PixelBuffer, bounds: layout.Rect, metrics: layout.Metrics, snapshot: *const Snapshot) void {
        buffer.fillRect(bounds, draw.palette.banner);
        const text = layout.keyHints(.{
            .workspace = snapshot.workspace,
            .menu_open = self.menu.open,
            .text_editing = self.imui.hasTextFocus() or snapshot.recording_rename_active,
        });
        buffer.drawText(text, bounds.x + metrics.pad, bounds.y + @divTrunc(bounds.height - font.textHeight(metrics.scale), 2), metrics.scale, draw.palette.text_dim);
    }

    fn centerMessage(self: *Ui, buffer: *draw.PixelBuffer, bounds: layout.Rect, scale: i32, text: []const u8) void {
        _ = self;
        buffer.fillRect(bounds, draw.palette.background);
        buffer.drawTextCentered(text, bounds, scale, draw.palette.text_dim);
    }

    fn ensureRenderTarget(self: *Ui) !void {
        var width: c_int = 0;
        var height: c_int = 0;
        if (!sdl.SDL_GetWindowSizeInPixels(self.window, &width, &height)) {
            std.log.err("SDL_GetWindowSizeInPixels failed: {s}", .{sdl.SDL_GetError()});
            return Error.SdlWindowSize;
        }

        if (width <= 0 or height <= 0) return;
        if (width == self.width and height == self.height and self.texture != null) return;
        const pixel_width: usize = @intCast(width);
        const pixel_height: usize = @intCast(height);
        if (pixel_width > std.math.maxInt(usize) / pixel_height) return Error.TooLarge;
        const pixel_count = pixel_width * pixel_height;

        if (self.texture) |texture| {
            sdl.SDL_DestroyTexture(texture);
            self.texture = null;
        }
        if (self.pixels.len > 0) {
            self.allocator.free(self.pixels);
            self.pixels = &.{};
        }

        self.texture = sdl.SDL_CreateTexture(
            self.renderer,
            sdl.SDL_PIXELFORMAT_ARGB8888,
            sdl.SDL_TEXTUREACCESS_STREAMING,
            width,
            height,
        ) orelse {
            std.log.err("SDL_CreateTexture failed: {s}", .{sdl.SDL_GetError()});
            return Error.SdlTexture;
        };
        self.pixels = try self.allocator.alloc(draw.Color, pixel_count);
        self.width = width;
        self.height = height;
    }

    fn present(self: *Ui) !void {
        const texture = self.texture orelse return Error.SdlTexture;
        const pitch: c_int = self.width * @as(c_int, @intCast(@sizeOf(draw.Color)));
        if (!sdl.SDL_UpdateTexture(texture, null, self.pixels.ptr, pitch)) {
            std.log.err("SDL_UpdateTexture failed: {s}", .{sdl.SDL_GetError()});
            return Error.SdlUpdateTexture;
        }
        if (!sdl.SDL_RenderTexture(self.renderer, texture, null, null)) {
            std.log.err("SDL_RenderTexture failed: {s}", .{sdl.SDL_GetError()});
            return Error.SdlRender;
        }
        if (!sdl.SDL_RenderPresent(self.renderer)) {
            std.log.err("SDL_RenderPresent failed: {s}", .{sdl.SDL_GetError()});
            return Error.SdlRender;
        }
    }
};

fn analysisCursorForMode(mode: AnalysisMode) usize {
    for (analysis_order, 0..) |candidate, index| {
        if (candidate == mode) return index;
    }
    return 0;
}

fn nextWorkspace(workspace: Workspace, delta: isize) Workspace {
    const count: isize = @intCast(layout.workspace_order.len);
    var index: isize = @as(isize, @intCast(workspace.index())) + delta;
    while (index < 0) index += count;
    return layout.workspace_order[@intCast(@mod(index, count))];
}

fn modeTitle(mode: AnalysisMode, sst_enabled: bool) []const u8 {
    return switch (mode) {
        .tonal => if (sst_enabled) "TONAL WAVELET SST" else "TONAL WAVELET RAW",
        .transient => "TRANSIENT STFT",
        .sparse => "SPARSE RIDGES",
        .reassigned => "REASSIGNED STFT",
        .squeezed => "SQUEEZED STFT",
        .superlet => "SUPERLET",
        .multitaper => "MULTITAPER",
        .s_transform => "S TRANSFORM",
    };
}

fn modeBanner(mode: AnalysisMode, sst_enabled: bool) []const u8 {
    return switch (mode) {
        .tonal => if (sst_enabled) "TONAL SST" else "TONAL RAW",
        .transient => "TRANSIENT",
        .sparse => "SPARSE",
        .reassigned => "REASSIGNED",
        .squeezed => "SQUEEZED",
        .superlet => "SUPERLET",
        .multitaper => "MULTITAPER",
        .s_transform => "S TRANSFORM",
    };
}

fn writeFrequencyNumber(buffer: []u8, hz: f64) usize {
    const text = if (hz >= 1000.0)
        std.fmt.bufPrint(buffer, "{d:.0}", .{hz}) catch ""
    else
        std.fmt.bufPrint(buffer, "{d:.1}", .{hz}) catch "";
    return text.len;
}

fn deltaBetween(selected: usize, clicked: usize) isize {
    if (clicked >= selected) return @intCast(clicked - selected);
    return -@as(isize, @intCast(selected - clicked));
}

fn stripWav(label: []const u8) []const u8 {
    if (std.ascii.endsWithIgnoreCase(label, ".wav")) return label[0 .. label.len - 4];
    return if (label.len == 0) "recording" else label;
}

test "menu cursor follows active mode and clamps by tab" {
    var menu = MenuState{};
    const snapshot = Snapshot{ .mode = .squeezed, .frequency_band = .high, .colormap = .turbo };
    menu.openWithSnapshot(&snapshot);
    try std.testing.expect(menu.open);
    try std.testing.expectEqual(analysisCursorForMode(.squeezed), menu.cursors[MenuTab.analysis.index()]);
    try std.testing.expectEqual(FrequencyBand.high.index(), menu.cursors[MenuTab.bands.index()]);
    try std.testing.expectEqual(Colormap.turbo.index(), menu.cursors[MenuTab.colors.index()]);

    menu.moveCursor(100);
    try std.testing.expectEqual(menu.rowCount() - 1, menu.cursors[menu.tab.index()]);
    menu.switchTab(1);
    try std.testing.expectEqual(MenuTab.bands, menu.tab);
}

test "workspace cycling follows banner order" {
    try std.testing.expectEqual(Workspace.recordings, nextWorkspace(.live, 1));
    try std.testing.expectEqual(Workspace.live, nextWorkspace(.band_lab, 1));
    try std.testing.expectEqual(Workspace.band_lab, nextWorkspace(.live, -1));
}

test "live history keeps the newest columns in column-major order" {
    var history = LiveHistory.init(std.testing.allocator);
    defer history.deinit();

    const columns = [_]f32{
        1, 2,
        3, 4,
        5, 6,
    };
    try history.append(.{
        .columns = &columns,
        .column_count = 3,
        .row_count = 2,
    }, 2);

    const visible = history.visible();
    try std.testing.expectEqual(@as(usize, 2), visible.column_count);
    try std.testing.expectEqualSlices(f32, &.{ 3, 4, 5, 6 }, visible.columns);
}
