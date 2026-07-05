//! Centered multi-resolution STFT spectrum primitives.
//! Rewrite target; C reference: src_c/src/analysis/spectrum.c

const std = @import("std");
const vdsp = @import("../apple/vdsp.zig");
const ring_buffer = @import("../audio/ring_buffer.zig");
const frequency_band = @import("../support/frequency_band.zig");

pub const Error = error{
    InvalidRequest,
    InvalidRange,
    NotEnoughAudio,
    SetupFailed,
    TooLarge,
};

pub const Mode = enum {
    transient,
    reassigned,
    squeezed,
    superlet,
    multitaper,
    s_transform,
    sparse,
};

const spectrum_count = 6;
const multitaper_count = 3;

const spectrum_lengths = [_]usize{
    512,
    1024,
    2048,
    4096,
    8192,
    16384,
};

const pi_value = std.math.pi;
const log_two = 0.693147180559945309417232121458176568;
const transient_cycles_per_window = 10.0;
const s_transform_cycles_per_window = 5.0;
const focused_transient_cycles_limit = 18.0;
const focused_s_transform_cycles_limit = 9.0;
const display_db_floor = -120.0;
const minimum_linear_power = 1.0e-12;
const maximum_spectrum_sample_rate = 512000.0;

const Window = struct {
    length: usize = 0,
    half_length: usize = 0,
    window: []f32 = &.{},
    tapers: [multitaper_count][]f32 = .{ &.{}, &.{}, &.{} },
    window_sum: f64 = 0.0,
    power_scale: f64 = 0.0,
    taper_power_scales: [multitaper_count]f64 = .{0.0} ** multitaper_count,
    setup: vdsp.DFTSetup = null,

    fn init(allocator: std.mem.Allocator, length: usize, previous: vdsp.DFTSetup) !Window {
        if (length == 0 or (length & 1) != 0) return Error.InvalidRequest;

        var result = Window{
            .length = length,
            .half_length = length / 2,
        };
        errdefer result.deinit(allocator);

        result.setup = vdsp.createDft(previous, length, .forward) catch return Error.SetupFailed;
        result.window = try allocator.alloc(f32, length);
        for (&result.tapers) |*taper| taper.* = try allocator.alloc(f32, length);

        var sum: f64 = 0.0;
        for (result.window, 0..) |*sample, i| {
            const unit = @as(f64, @floatFromInt(i)) / @as(f64, @floatFromInt(length - 1));
            const value = 0.5 - 0.5 * @cos(2.0 * pi_value * unit);

            sample.* = @floatCast(value);
            sum += value;
        }

        result.window_sum = @max(sum, 1.0e-12);
        result.power_scale = 1.0 / (result.window_sum * result.window_sum);

        for (0..multitaper_count) |taper_index| {
            const order = taper_index * 2 + 1;
            var taper_sum: f64 = 0.0;

            for (result.tapers[taper_index], 0..) |*sample, i| {
                const angle = pi_value *
                    @as(f64, @floatFromInt(order)) *
                    @as(f64, @floatFromInt(i + 1)) /
                    @as(f64, @floatFromInt(length + 1));
                const value = @sin(angle);

                sample.* = @floatCast(value);
                taper_sum += value;
            }

            taper_sum = @max(@abs(taper_sum), 1.0e-12);
            result.taper_power_scales[taper_index] = 1.0 / (taper_sum * taper_sum);
        }

        return result;
    }

    fn deinit(self: *Window, allocator: std.mem.Allocator) void {
        vdsp.destroyDft(self.setup);
        if (self.window.len > 0) allocator.free(self.window);
        for (&self.tapers) |*taper| {
            if (taper.len > 0) allocator.free(taper.*);
            taper.* = &.{};
        }
        self.* = .{};
    }
};

const BinMap = struct {
    lower: usize = 0,
    upper: usize = 0,
    first_bin: usize = 0,
    last_bin: usize = 0,
    unit: f64 = 0.0,
    integrate: bool = false,
};

const RowMap = struct {
    hz: f64 = 0.0,
    first_spectrum: usize = 0,
    second_spectrum: usize = 0,
    first_weight: f64 = 1.0,
    second_weight: f64 = 0.0,
    bins: [spectrum_count]BinMap = .{BinMap{}} ** spectrum_count,
};

pub const Analyzer = struct {
    allocator: std.mem.Allocator,
    sample_rate: f64,
    full_min_hz: f64,
    full_max_hz: f64,
    min_hz: f64,
    max_hz: f64,
    transient_target_cycles: f64,
    s_transform_target_cycles: f64,
    latency_samples: u64,
    spectra: [spectrum_count]Window,
    samples: []f32,
    input_even: []f32,
    input_odd: []f32,
    output_real: []f32,
    output_imag: []f32,
    power_rows: []f32,
    work_rows: []f32,
    window_rows: []f32,
    row_maps: []RowMap,
    row_capacity: usize,
    row_map_count: usize,

    pub fn init(
        allocator: std.mem.Allocator,
        sample_rate: f64,
        columns_per_second: f64,
    ) !Analyzer {
        if (!std.math.isFinite(sample_rate) or
            sample_rate <= 0.0 or
            sample_rate > maximum_spectrum_sample_rate or
            !std.math.isFinite(columns_per_second) or
            columns_per_second <= 0.0)
        {
            return Error.InvalidRequest;
        }

        var created = Analyzer{
            .allocator = allocator,
            .sample_rate = sample_rate,
            .full_min_hz = frequency_band.full_min_hz,
            .full_max_hz = @min(frequency_band.full_max_hz, sample_rate * 0.5),
            .min_hz = frequency_band.full_min_hz,
            .max_hz = @min(frequency_band.full_max_hz, sample_rate * 0.5),
            .transient_target_cycles = transient_cycles_per_window,
            .s_transform_target_cycles = s_transform_cycles_per_window,
            .latency_samples = spectrum_lengths[spectrum_count - 1] / 2,
            .spectra = .{Window{}} ** spectrum_count,
            .samples = &.{},
            .input_even = &.{},
            .input_odd = &.{},
            .output_real = &.{},
            .output_imag = &.{},
            .power_rows = &.{},
            .work_rows = &.{},
            .window_rows = &.{},
            .row_maps = &.{},
            .row_capacity = 0,
            .row_map_count = 0,
        };
        errdefer created.deinit();

        created.updateRangeTuning();

        var previous: vdsp.DFTSetup = null;
        for (&created.spectra, spectrum_lengths) |*window, length| {
            window.* = try Window.init(allocator, length, previous);
            previous = window.setup;
        }

        const maximum_length = spectrum_lengths[spectrum_count - 1];
        const maximum_half = maximum_length / 2;
        created.samples = try allocator.alloc(f32, maximum_length);
        created.input_even = try allocator.alloc(f32, maximum_half);
        created.input_odd = try allocator.alloc(f32, maximum_half);
        created.output_real = try allocator.alloc(f32, maximum_half);
        created.output_imag = try allocator.alloc(f32, maximum_half);

        return created;
    }

    pub fn deinit(self: *Analyzer) void {
        for (&self.spectra) |*window| window.deinit(self.allocator);
        if (self.samples.len > 0) self.allocator.free(self.samples);
        if (self.input_even.len > 0) self.allocator.free(self.input_even);
        if (self.input_odd.len > 0) self.allocator.free(self.input_odd);
        if (self.output_real.len > 0) self.allocator.free(self.output_real);
        if (self.output_imag.len > 0) self.allocator.free(self.output_imag);
        if (self.power_rows.len > 0) self.allocator.free(self.power_rows);
        if (self.work_rows.len > 0) self.allocator.free(self.work_rows);
        if (self.window_rows.len > 0) self.allocator.free(self.window_rows);
        if (self.row_maps.len > 0) self.allocator.free(self.row_maps);
        self.* = undefined;
    }

    pub fn minFrequency(self: *const Analyzer) f64 {
        return self.min_hz;
    }

    pub fn maxFrequency(self: *const Analyzer) f64 {
        return self.max_hz;
    }

    pub fn latencySamples(self: *const Analyzer) u64 {
        return self.latency_samples;
    }

    pub fn setFrequencyRange(self: *Analyzer, min_hz_arg: f64, max_hz_arg: f64) !void {
        var min_hz = min_hz_arg;
        var max_hz = max_hz_arg;

        if (min_hz < self.full_min_hz) min_hz = self.full_min_hz;
        if (max_hz > self.full_max_hz) max_hz = self.full_max_hz;

        if (min_hz <= 0.0 or max_hz <= min_hz) return Error.InvalidRange;

        if (self.min_hz != min_hz or self.max_hz != max_hz) {
            self.min_hz = min_hz;
            self.max_hz = max_hz;
            self.updateRangeTuning();
            self.row_map_count = 0;
        }
    }

    pub fn frequencyForRow(self: *const Analyzer, row: usize, row_count: usize) f64 {
        if (row_count == 0) return 0.0;

        const unit = if (row_count == 1)
            0.5
        else
            (@as(f64, @floatFromInt(row)) + 0.5) / @as(f64, @floatFromInt(row_count));
        const log_min = log2Double(self.min_hz);
        const log_max = log2Double(self.max_hz);
        const log_hz = log_max + (log_min - log_max) * clamp(unit, 0.0, 1.0);

        return std.math.pow(f64, 2.0, log_hz);
    }

    pub fn columnDb(
        self: *Analyzer,
        ring: *const ring_buffer.RingBuffer,
        center_sample: u64,
        mode: Mode,
        dbfs_rows: []f32,
    ) !void {
        if (dbfs_rows.len == 0) return Error.InvalidRequest;
        try self.ensureRows(dbfs_rows.len);

        if (mode == .multitaper) {
            try self.fillMultitaperWindowRows(ring, center_sample, dbfs_rows.len);
        } else {
            try self.fillHannWindowRows(ring, center_sample, dbfs_rows.len);
        }

        switch (mode) {
            .transient, .multitaper => self.buildWeightedRows(dbfs_rows.len, self.transient_target_cycles, self.power_rows[0..dbfs_rows.len]),
            .reassigned => self.buildReassignedRows(dbfs_rows.len),
            .squeezed => self.buildSqueezedRows(dbfs_rows.len),
            .superlet => self.buildSuperletRows(dbfs_rows.len),
            .s_transform => self.buildWeightedRows(dbfs_rows.len, self.s_transform_target_cycles, self.power_rows[0..dbfs_rows.len]),
            .sparse => self.buildSparseRows(dbfs_rows.len),
        }

        self.convertPowerRowsToDb(dbfs_rows);
    }

    fn updateRangeTuning(self: *Analyzer) void {
        const full_octaves = log2Double(self.full_max_hz / self.full_min_hz);
        const range_octaves = log2Double(self.max_hz / self.min_hz);
        const focus = if (range_octaves > 0.0)
            clamp(full_octaves / range_octaves, 1.0, 8.0)
        else
            1.0;

        self.transient_target_cycles = clamp(
            transient_cycles_per_window * (1.0 + 0.16 * (focus - 1.0)),
            transient_cycles_per_window,
            focused_transient_cycles_limit,
        );
        self.s_transform_target_cycles = clamp(
            s_transform_cycles_per_window * (1.0 + 0.14 * (focus - 1.0)),
            s_transform_cycles_per_window,
            focused_s_transform_cycles_limit,
        );
    }

    fn frequencyForRowEdge(self: *const Analyzer, edge: usize, row_count: usize) f64 {
        const unit = if (row_count == 0)
            0.0
        else
            @as(f64, @floatFromInt(edge)) / @as(f64, @floatFromInt(row_count));
        const log_min = log2Double(self.min_hz);
        const log_max = log2Double(self.max_hz);
        const log_hz = log_max + (log_min - log_max) * clamp(unit, 0.0, 1.0);

        return std.math.pow(f64, 2.0, log_hz);
    }

    fn ensureRows(self: *Analyzer, row_count: usize) !void {
        if (row_count <= self.row_capacity) {
            if (self.row_map_count != row_count) self.rebuildRowMaps(row_count);
            return;
        }

        if (row_count > std.math.maxInt(usize) / spectrum_count) return Error.TooLarge;

        if (self.power_rows.len > 0) self.allocator.free(self.power_rows);
        if (self.work_rows.len > 0) self.allocator.free(self.work_rows);
        if (self.window_rows.len > 0) self.allocator.free(self.window_rows);
        if (self.row_maps.len > 0) self.allocator.free(self.row_maps);

        self.power_rows = try self.allocator.alloc(f32, row_count);
        self.work_rows = try self.allocator.alloc(f32, row_count);
        self.window_rows = try self.allocator.alloc(f32, row_count * spectrum_count);
        self.row_maps = try self.allocator.alloc(RowMap, row_count);
        self.row_capacity = row_count;
        self.row_map_count = 0;
        self.rebuildRowMaps(row_count);
    }

    fn rebuildRowMaps(self: *Analyzer, row_count: usize) void {
        for (self.row_maps[0..row_count], 0..) |*map, row| {
            const hz = self.frequencyForRow(row, row_count);
            const high_hz = self.frequencyForRowEdge(row, row_count);
            const low_hz = self.frequencyForRowEdge(row + 1, row_count);
            const choice = self.chooseSpectra(hz, self.transient_target_cycles);

            map.* = .{
                .hz = hz,
                .first_spectrum = choice.first,
                .second_spectrum = choice.second,
                .first_weight = 1.0 - choice.second_amount,
                .second_weight = if (choice.first == choice.second) 0.0 else choice.second_amount,
                .bins = undefined,
            };

            for (&map.bins, 0..) |*bin_map, index| {
                bin_map.* = self.spectrumBinMap(index, low_hz, hz, high_hz);
            }
        }

        self.row_map_count = row_count;
    }

    fn evaluateWindowWithTaper(
        self: *Analyzer,
        ring: *const ring_buffer.RingBuffer,
        center_sample: u64,
        spectrum_index: usize,
        taper: []const f32,
    ) !void {
        const spectrum_window = &self.spectra[spectrum_index];
        if (center_sample > std.math.maxInt(u64) - spectrum_window.half_length) {
            return Error.InvalidRequest;
        }

        const end_sample = center_sample + @as(u64, @intCast(spectrum_window.half_length));
        const window_samples = self.samples[0..spectrum_window.length];
        if (ring.readEndingAt(end_sample, window_samples) != spectrum_window.length) {
            return Error.NotEnoughAudio;
        }

        const half = spectrum_window.half_length;
        vdsp.mulStrided(window_samples.ptr, 2, taper.ptr, 2, self.input_even[0..half]);
        vdsp.mulStrided(window_samples[1..].ptr, 2, taper[1..].ptr, 2, self.input_odd[0..half]);
        vdsp.executeDft(
            spectrum_window.setup,
            self.input_even[0..half],
            self.input_odd[0..half],
            self.output_real[0..half],
            self.output_imag[0..half],
        );
    }

    fn fillHannWindowRows(
        self: *Analyzer,
        ring: *const ring_buffer.RingBuffer,
        center_sample: u64,
        row_count: usize,
    ) !void {
        for (0..spectrum_count) |spectrum_index| {
            try self.evaluateWindowWithTaper(
                ring,
                center_sample,
                spectrum_index,
                self.spectra[spectrum_index].window,
            );

            const rows = self.windowRowsFor(spectrum_index, row_count);
            const scale = self.spectra[spectrum_index].power_scale;

            for (rows, self.row_maps[0..row_count]) |*row_power, map| {
                row_power.* = @floatCast(self.mappedSpectrumPower(spectrum_index, &map.bins[spectrum_index], scale));
            }
        }
    }

    fn fillMultitaperWindowRows(
        self: *Analyzer,
        ring: *const ring_buffer.RingBuffer,
        center_sample: u64,
        row_count: usize,
    ) !void {
        for (0..spectrum_count) |spectrum_index| {
            const rows = self.windowRowsFor(spectrum_index, row_count);
            @memset(rows, 0.0);

            for (0..multitaper_count) |taper_index| {
                try self.evaluateWindowWithTaper(
                    ring,
                    center_sample,
                    spectrum_index,
                    self.spectra[spectrum_index].tapers[taper_index],
                );

                const scale = self.spectra[spectrum_index].taper_power_scales[taper_index];

                for (rows, self.row_maps[0..row_count]) |*row_power, map| {
                    const power = self.mappedSpectrumPower(spectrum_index, &map.bins[spectrum_index], scale);
                    row_power.* += @floatCast(power / @as(f64, @floatFromInt(multitaper_count)));
                }
            }
        }
    }

    fn windowRowsFor(self: *Analyzer, spectrum_index: usize, row_count: usize) []f32 {
        const start = spectrum_index * row_count;
        return self.window_rows[start..][0..row_count];
    }

    fn binPower(self: *const Analyzer, spectrum_window: *const Window, bin: usize, power_scale: f64) f64 {
        var real: f64 = undefined;
        var imag: f64 = undefined;

        if (bin == 0) {
            real = self.output_real[0];
            imag = 0.0;
        } else if (bin >= spectrum_window.half_length) {
            real = self.output_imag[0];
            imag = 0.0;
        } else {
            real = self.output_real[bin];
            imag = self.output_imag[bin];
        }

        return (real * real + imag * imag) * power_scale;
    }

    fn mappedSpectrumPower(
        self: *const Analyzer,
        spectrum_index: usize,
        map: *const BinMap,
        power_scale: f64,
    ) f64 {
        const spectrum_window = &self.spectra[spectrum_index];

        if (map.integrate) {
            var total: f64 = 0.0;
            var bin = map.first_bin;
            while (bin <= map.last_bin) : (bin += 1) {
                total += self.binPower(spectrum_window, bin, power_scale);
                if (bin == spectrum_window.half_length) break;
            }

            return total / @as(f64, @floatFromInt(map.last_bin - map.first_bin + 1));
        }

        const low_power = self.binPower(spectrum_window, map.lower, power_scale);
        const high_power = self.binPower(spectrum_window, map.upper, power_scale);

        return low_power + (high_power - low_power) * map.unit;
    }

    fn desiredWindowSeconds(self: *const Analyzer, hz: f64, target_cycles: f64) f64 {
        const shortest = @as(f64, @floatFromInt(spectrum_lengths[0])) / self.sample_rate;
        const longest = @as(f64, @floatFromInt(spectrum_lengths[spectrum_count - 1])) / self.sample_rate;
        const seconds = target_cycles / @max(hz, 1.0);

        return clamp(seconds, shortest, longest);
    }

    const SpectrumChoice = struct {
        first: usize,
        second: usize,
        second_amount: f64,
    };

    fn chooseSpectra(self: *const Analyzer, hz: f64, target_cycles: f64) SpectrumChoice {
        const desired = self.desiredWindowSeconds(hz, target_cycles);
        var chosen: usize = 0;

        while (chosen + 1 < spectrum_count and
            @as(f64, @floatFromInt(self.spectra[chosen + 1].length)) / self.sample_rate < desired)
        {
            chosen += 1;
        }

        if (chosen + 1 >= spectrum_count) {
            return .{
                .first = spectrum_count - 1,
                .second = spectrum_count - 1,
                .second_amount = 0.0,
            };
        }

        const low = @as(f64, @floatFromInt(self.spectra[chosen].length)) / self.sample_rate;
        const high = @as(f64, @floatFromInt(self.spectra[chosen + 1].length)) / self.sample_rate;
        const amount = (@log(desired) - @log(low)) / (@log(high) - @log(low));

        return .{
            .first = chosen,
            .second = chosen + 1,
            .second_amount = clamp(amount, 0.0, 1.0),
        };
    }

    fn spectrumBinMap(
        self: *const Analyzer,
        spectrum_index: usize,
        low_hz: f64,
        hz: f64,
        high_hz: f64,
    ) BinMap {
        const spectrum_window = &self.spectra[spectrum_index];
        const length_f = @as(f64, @floatFromInt(spectrum_window.length));
        const half_f = @as(f64, @floatFromInt(spectrum_window.half_length));
        const bin_position = hz * length_f / self.sample_rate;
        const clamped = clamp(bin_position, 0.0, half_f);
        const lower: usize = @intFromFloat(@floor(clamped));
        var upper = lower + 1;
        const low_position = low_hz * length_f / self.sample_rate;
        const high_position = high_hz * length_f / self.sample_rate;
        var first: i64 = @intFromFloat(@ceil(@min(low_position, high_position)));
        var last: i64 = @intFromFloat(@floor(@max(low_position, high_position)));

        if (upper > spectrum_window.half_length) upper = spectrum_window.half_length;

        if (first < 0) first = 0;
        if (last < 0) last = 0;
        if (first > @as(i64, @intCast(spectrum_window.half_length))) {
            first = @intCast(spectrum_window.half_length);
        }
        if (last > @as(i64, @intCast(spectrum_window.half_length))) {
            last = @intCast(spectrum_window.half_length);
        }

        return .{
            .lower = lower,
            .upper = upper,
            .first_bin = @intCast(first),
            .last_bin = @intCast(last),
            .unit = clamped - @as(f64, @floatFromInt(lower)),
            .integrate = first <= last,
        };
    }

    fn buildWeightedRows(self: *Analyzer, row_count: usize, target_cycles: f64, rows: []f32) void {
        for (rows[0..row_count], self.row_maps[0..row_count], 0..) |*out, map, row| {
            const choice = self.chooseSpectra(map.hz, target_cycles);
            const first_power = self.windowRowsFor(choice.first, row_count)[row];
            var power = @as(f64, first_power);

            if (choice.second != choice.first) {
                const second_power = self.windowRowsFor(choice.second, row_count)[row];
                power = @as(f64, first_power) * (1.0 - choice.second_amount) +
                    @as(f64, second_power) * choice.second_amount;
            }

            out.* = @floatCast(power);
        }
    }

    fn buildReassignedRows(self: *Analyzer, row_count: usize) void {
        self.buildWeightedRows(row_count, self.transient_target_cycles, self.work_rows[0..row_count]);
        @memset(self.power_rows[0..row_count], 0.0);

        for (0..row_count) |row| {
            const power = @as(f64, self.work_rows[row]);
            const peak = nearestPeak(self.work_rows[0..row_count], row, 6);
            const local = neighborhoodAverage(self.work_rows[0..row_count], peak, 8);
            const contrast = @as(f64, self.work_rows[peak]) / @max(local, minimum_linear_power);

            if (contrast > 1.35) {
                const target = @as(f64, @floatFromInt(peak)) +
                    parabolicPeakOffset(self.work_rows[0..row_count], peak);

                depositPower(self.power_rows[0..row_count], target, power * 0.85, 1.0);
                depositPower(self.power_rows[0..row_count], @floatFromInt(row), power * 0.15, 1.6);
            } else {
                depositPower(self.power_rows[0..row_count], @floatFromInt(row), power, 1.3);
            }
        }
    }

    fn buildSqueezedRows(self: *Analyzer, row_count: usize) void {
        self.buildWeightedRows(row_count, self.transient_target_cycles, self.work_rows[0..row_count]);
        @memset(self.power_rows[0..row_count], 0.0);

        for (0..row_count) |row| {
            const power = @as(f64, self.work_rows[row]);
            const peak = nearestPeak(self.work_rows[0..row_count], row, 10);
            const local = neighborhoodAverage(self.work_rows[0..row_count], peak, 12);
            const contrast = @as(f64, self.work_rows[peak]) / @max(local, minimum_linear_power);

            if (contrast > 1.18) {
                const target = @as(f64, @floatFromInt(peak)) +
                    parabolicPeakOffset(self.work_rows[0..row_count], peak);

                depositPower(self.power_rows[0..row_count], target, power * 0.95, 0.65);
                depositPower(self.power_rows[0..row_count], @floatFromInt(row), power * 0.05, 1.8);
            } else {
                depositPower(self.power_rows[0..row_count], @floatFromInt(row), power, 1.1);
            }
        }
    }

    fn buildSuperletRows(self: *Analyzer, row_count: usize) void {
        for (0..row_count) |row| {
            const map = &self.row_maps[row];
            const choice = self.chooseSpectra(map.hz, self.transient_target_cycles);
            const last = choice.second;
            const count = last + 1;
            var log_sum: f64 = 0.0;

            for (0..last + 1) |i| {
                const power = self.windowRowsFor(i, row_count)[row];
                log_sum += @log(@max(@as(f64, power), minimum_linear_power));
            }

            self.power_rows[row] = @floatCast(@exp(log_sum / @as(f64, @floatFromInt(count))));
        }
    }

    fn buildSparseRows(self: *Analyzer, row_count: usize) void {
        self.buildWeightedRows(row_count, self.transient_target_cycles, self.work_rows[0..row_count]);

        var maximum: f32 = 0.0;
        for (self.work_rows[0..row_count]) |power| {
            if (power > maximum) maximum = power;
        }

        const threshold = maximum * 1.0e-4;
        @memset(self.power_rows[0..row_count], 0.0);

        for (0..row_count) |row| {
            const power = @as(f64, self.work_rows[row]);

            if (isLocalMaximum(self.work_rows[0..row_count], row) and self.work_rows[row] >= threshold) {
                const target = @as(f64, @floatFromInt(row)) +
                    parabolicPeakOffset(self.work_rows[0..row_count], row);

                depositPower(self.power_rows[0..row_count], target, power, 0.75);
            } else {
                depositPower(self.power_rows[0..row_count], @floatFromInt(row), power * 0.025, 1.4);
            }
        }
    }

    fn convertPowerRowsToDb(self: *const Analyzer, dbfs_rows: []f32) void {
        for (dbfs_rows, self.power_rows[0..dbfs_rows.len]) |*db, power| {
            var value = 10.0 * @log10(@max(@as(f64, power), minimum_linear_power));
            if (value < display_db_floor) value = display_db_floor;
            db.* = @floatCast(value);
        }
    }
};

fn clamp(value: f64, minimum: f64, maximum: f64) f64 {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

fn log2Double(value: f64) f64 {
    return @log(value) / log_two;
}

fn depositPower(rows: []f32, center: f64, power: f64, sigma_rows: f64) void {
    if (power <= 0.0 or rows.len == 0) return;

    var first: isize = @intFromFloat(@floor(center - 3.0 * sigma_rows));
    var last: isize = @intFromFloat(@ceil(center + 3.0 * sigma_rows));

    if (first < 0) first = 0;
    if (last >= @as(isize, @intCast(rows.len))) last = @as(isize, @intCast(rows.len)) - 1;

    var weight_sum: f64 = 0.0;
    var row = first;
    while (row <= last) : (row += 1) {
        const distance = (@as(f64, @floatFromInt(row)) - center) / sigma_rows;
        weight_sum += @exp(-0.5 * distance * distance);
    }

    if (weight_sum <= 0.0) return;

    row = first;
    while (row <= last) : (row += 1) {
        const distance = (@as(f64, @floatFromInt(row)) - center) / sigma_rows;
        const weight = @exp(-0.5 * distance * distance) / weight_sum;
        rows[@intCast(row)] += @floatCast(power * weight);
    }
}

fn nearestPeak(rows: []const f32, row: usize, radius: usize) usize {
    const first = if (row > radius) row - radius else 0;
    const last = @min(row + radius, rows.len - 1);

    var best = row;
    var best_power = rows[row];

    for (first..last + 1) |i| {
        if (rows[i] > best_power) {
            best_power = rows[i];
            best = i;
        }
    }

    return best;
}

fn neighborhoodAverage(rows: []const f32, row: usize, radius: usize) f64 {
    const first = if (row > radius) row - radius else 0;
    const last = @min(row + radius, rows.len - 1);

    var sum: f64 = 0.0;
    for (first..last + 1) |i| sum += rows[i];

    return sum / @as(f64, @floatFromInt(last - first + 1));
}

fn parabolicPeakOffset(rows: []const f32, peak: usize) f64 {
    if (peak == 0 or peak + 1 >= rows.len) return 0.0;

    const left = @log(@max(@as(f64, rows[peak - 1]), minimum_linear_power));
    const center = @log(@max(@as(f64, rows[peak]), minimum_linear_power));
    const right = @log(@max(@as(f64, rows[peak + 1]), minimum_linear_power));
    const denominator = left - 2.0 * center + right;

    if (@abs(denominator) < 1.0e-12) return 0.0;
    return clamp(0.5 * (left - right) / denominator, -0.5, 0.5);
}

fn isLocalMaximum(rows: []const f32, row: usize) bool {
    const center = rows[row];
    const before = if (row == 0) -1.0 else rows[row - 1];
    const after = if (row + 1 >= rows.len) -1.0 else rows[row + 1];

    return center >= before and center >= after;
}

test "spectrum impulse stays centered across target rows" {
    const sample_rate = 48000.0;
    const columns_per_second = 240.0;
    const row_count = 512;
    const min_hz = 10.0;
    const max_hz = 24000.0;
    const sample_count: usize = @intFromFloat(sample_rate * 3.0);
    const impulse_sample: usize = @intFromFloat(sample_rate);
    const column_samples: i64 = @intFromFloat(@round(sample_rate / columns_per_second));

    var samples = try std.testing.allocator.alloc(f32, sample_count);
    defer std.testing.allocator.free(samples);
    @memset(samples, 0.0);
    samples[impulse_sample] = 1.0;

    const rows = try std.testing.allocator.alloc(f32, row_count);
    defer std.testing.allocator.free(rows);

    var ring = try ring_buffer.RingBuffer.init(std.testing.allocator, sample_count);
    defer ring.deinit();
    ring.write(samples);

    var analyzer = try Analyzer.init(std.testing.allocator, sample_rate, columns_per_second);
    defer analyzer.deinit();

    const Target = struct {
        hz: f64,
        row: usize,
        peak_db: f32,
        peak_offset: i64,
    };
    var targets = [_]Target{
        .{ .hz = 20000.0, .row = 0, .peak_db = -1.0e30, .peak_offset = 0 },
        .{ .hz = 10000.0, .row = 0, .peak_db = -1.0e30, .peak_offset = 0 },
        .{ .hz = 1000.0, .row = 0, .peak_db = -1.0e30, .peak_offset = 0 },
        .{ .hz = 100.0, .row = 0, .peak_db = -1.0e30, .peak_offset = 0 },
        .{ .hz = 20.0, .row = 0, .peak_db = -1.0e30, .peak_offset = 0 },
        .{ .hz = 10.0, .row = 0, .peak_db = -1.0e30, .peak_offset = 0 },
    };

    for (&targets) |*target| target.row = testRowForFrequency(target.hz, min_hz, max_hz, row_count);

    var center: i64 = @as(i64, @intCast(impulse_sample)) - @as(i64, @intFromFloat(sample_rate * 0.5));
    const last_center: i64 = @as(i64, @intCast(impulse_sample)) + @as(i64, @intFromFloat(sample_rate * 0.5));
    while (center <= last_center) : (center += column_samples) {
        try analyzer.columnDb(&ring, @intCast(center), .transient, rows);

        for (&targets) |*target| {
            const db = rows[target.row];
            if (db > target.peak_db) {
                target.peak_db = db;
                target.peak_offset = center - @as(i64, @intCast(impulse_sample));
            }
        }
    }

    for (targets) |target| {
        try std.testing.expect(@abs(target.peak_offset) <= column_samples);
    }
}

test "all spectrum modes produce finite impulse energy and focused ranges" {
    const sample_rate = 48000.0;
    const columns_per_second = 240.0;
    const row_count = 512;
    const sample_count: usize = @intFromFloat(sample_rate * 3.0);
    const impulse_sample: usize = @intFromFloat(sample_rate);

    var samples = try std.testing.allocator.alloc(f32, sample_count);
    defer std.testing.allocator.free(samples);
    @memset(samples, 0.0);
    samples[impulse_sample] = 1.0;

    const rows = try std.testing.allocator.alloc(f32, row_count);
    defer std.testing.allocator.free(rows);

    var ring = try ring_buffer.RingBuffer.init(std.testing.allocator, sample_count);
    defer ring.deinit();
    ring.write(samples);

    var analyzer = try Analyzer.init(std.testing.allocator, sample_rate, columns_per_second);
    defer analyzer.deinit();

    const modes = [_]Mode{
        .transient,
        .reassigned,
        .squeezed,
        .superlet,
        .multitaper,
        .s_transform,
        .sparse,
    };

    for (modes) |mode| {
        try analyzer.columnDb(&ring, impulse_sample, mode, rows);
        try expectFiniteVisible(rows);
    }

    try analyzer.setFrequencyRange(120.0, 1000.0);
    try analyzer.columnDb(&ring, impulse_sample, .transient, rows);
    try expectFiniteVisible(rows);

    try analyzer.setFrequencyRange(250.0, 750.0);
    try analyzer.columnDb(&ring, impulse_sample, .reassigned, rows);
    try expectFiniteVisible(rows);
    try std.testing.expectApproxEqAbs(@as(f64, 250.0), analyzer.minFrequency(), 0.01);
    try std.testing.expectApproxEqAbs(@as(f64, 750.0), analyzer.maxFrequency(), 0.01);
}

fn testRowForFrequency(hz: f64, min_hz: f64, max_hz: f64, row_count: usize) usize {
    const unit = (log2Double(max_hz) - log2Double(hz)) /
        (log2Double(max_hz) - log2Double(min_hz));
    const row: i64 = @intFromFloat(@round(unit * @as(f64, @floatFromInt(row_count)) - 0.5));

    if (row < 0) return 0;
    if (row >= @as(i64, @intCast(row_count))) return row_count - 1;
    return @intCast(row);
}

fn expectFiniteVisible(rows: []const f32) !void {
    var maximum: f32 = -1.0e30;
    for (rows) |row| {
        try std.testing.expect(std.math.isFinite(row));
        if (row > maximum) maximum = row;
    }

    try std.testing.expect(maximum > -119.0);
}
