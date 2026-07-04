/*
 * Sounds: a real-time microphone view in a window.
 *
 * Core Audio HAL capture and vDSP analysis feed a software-rendered
 * pixel buffer: raw waveform on top, a scrolling log-frequency
 * spectrogram below, and a Viridis palette strip along the bottom.
 * SDL only opens the window and presents the pixels; every drawn pixel
 * comes from the plain C in this file.
 */

#include "sounds/analysis.h"
#include "sounds/capture.h"
#include "sounds/colormap.h"
#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char app_name[] = "Sounds";
static const char app_identifier[] = "dev.szymon.sounds";
static const char recording_directory[] = "recordings";
static const char recording_file_prefix[] = "sounds-";
static const char recording_file_extension[] = ".f32";

static const int initial_window_width = 1100;
static const int initial_window_height = 640;
static const int minimum_window_width = 480;
static const int minimum_window_height = 320;

/*
 * Display and retention constants. The analysis range, Morlet omega0, and
 * voices per octave are the SOUND_WAVELET_* constants in analysis.h:
 * 20 Hz up to a 24 kHz display ceiling with analysis voices to ~23 kHz
 * (the built-in microphone's measured usable band; the empty strip at the
 * very top shows the ADC's 23 kHz anti-alias brick wall), 48 voices per
 * octave, analytic Morlet omega0 = 8, synchrosqueezing enabled by default.
 * Press S at runtime to compare raw CWT magnitude with synchrosqueezed
 * energy.
 *
 * Spectrogram rows span that range in log frequency, highest at the top,
 * with a labeled axis in the left gutter. The analyzer emits calibrated
 * dBFS per band (a full-scale sine reads about 0 dBFS), so the color range
 * below is absolute.
 */
static const double waveform_seconds = 0.08;
static const double raw_recording_seconds = 30.0;

static const float floor_db = -95.0F;
static const float ceiling_db = -15.0F;
static const float band_smoothing = 0.6F; /* per-frame approach toward new dB */

static const uint32_t background_color = 0x05070C;
static const uint32_t separator_color = 0x10141C;
static const uint32_t midline_color = 0x1B2433;
static const uint32_t waveform_color = 0x95DDFF;
static const uint32_t axis_background_color = 0x0B0F16;
static const uint32_t axis_text_color = 0x8FA3BF;
static const uint32_t axis_tick_color = 0x415068;
static const uint32_t gridline_color = 0x3E4A66;

static const int separator_height = 2;
static const int palette_height = 8;
static const uint64_t minimum_waveform_samples = 1024;

/*
 * The spectrogram advances on the audio clock, not the display clock: one
 * column per this many columns-per-second of captured audio, so a display
 * frame may shift the image by several columns at once. 240 columns/s
 * makes each column about 4 ms of signal.
 */
static const double spectrogram_columns_per_second = 240.0;

enum {
    maximum_columns_per_frame = 64,
};

enum {
    recording_path_capacity = 128,
    glyph_width = 5,
    glyph_height = 7,
};

/* 5x7 bitmap glyphs for the axis labels: '0'-'9', '.', 'k'. Bit 4 is the
 * leftmost column of each row. */
static const uint8_t glyph_bitmaps[12][glyph_height] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, /* 9 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}, /* . */
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, /* k */
};

typedef struct FrequencyTick {
    double hz;
    const char *label;
    bool gridline; /* decades also get a faint line across the plot */
} FrequencyTick;

static const FrequencyTick frequency_ticks[] = {
    {20000.0, "20k", false},
    {10000.0, "10k", true},
    {5000.0, "5k", false},
    {2000.0, "2k", false},
    {1000.0, "1k", true},
    {500.0, "500", false},
    {200.0, "200", false},
    {100.0, "100", true},
    {50.0, "50", false},
    {20.0, "20", false},
    {10.0, "10", true},
    {5.0, "5", false},
    {2.0, "2", false},
    {1.0, "1", true},
    {0.5, "0.5", false},
};

typedef struct View {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t *pixels;
    float *bands; /* smoothed dBFS per spectrogram row */
    float *column_db;
    uint8_t *grid_flags; /* per spectrogram row: paint a faint gridline */
    double min_hz;
    double max_hz;
    int width;
    int height;
    int waveform_height;
    int spectrogram_top;
    int spectrogram_height;
    int spectrogram_left; /* axis gutter occupies pixels left of this */
    int text_scale;
} View;

static uint32_t pack_color(SoundColor color) {
    uint32_t red = (uint32_t)lrintf(color.red * 255.0F);
    uint32_t green = (uint32_t)lrintf(color.green * 255.0F);
    uint32_t blue = (uint32_t)lrintf(color.blue * 255.0F);

    return (red << 16) | (green << 8) | blue;
}

static uint32_t color_for_db(float db) {
    float unit = (db - floor_db) / (ceiling_db - floor_db);
    return pack_color(sound_colormap_viridis(unit));
}

static double amplitude_db(double value) {
    return 20.0 * log10(fmax(value, 1.0e-12));
}

static uint32_t *view_row(View *view, int y) {
    return view->pixels + (size_t)y * (size_t)view->width;
}

static uint32_t blend_color(uint32_t base, uint32_t over, float amount) {
    float keep = 1.0F - amount;
    uint32_t red = (uint32_t)lrintf(
        (float)((base >> 16) & 0xFFU) * keep + (float)((over >> 16) & 0xFFU) * amount
    );
    uint32_t green = (uint32_t)lrintf(
        (float)((base >> 8) & 0xFFU) * keep + (float)((over >> 8) & 0xFFU) * amount
    );
    uint32_t blue = (uint32_t)lrintf(
        (float)(base & 0xFFU) * keep + (float)(over & 0xFFU) * amount
    );

    return (red << 16) | (green << 8) | blue;
}

static int glyph_index(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    if (character == '.') {
        return 10;
    }

    if (character == 'k') {
        return 11;
    }

    return -1;
}

static int text_width_pixels(const char *text, int scale) {
    int glyphs = (int)strlen(text);
    return glyphs > 0 ? (glyphs * (glyph_width + 1) - 1) * scale : 0;
}

static void draw_text(View *view, const char *text, int x, int y, uint32_t color) {
    int scale = view->text_scale;

    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        int glyph = glyph_index(*cursor);

        if (glyph >= 0) {
            for (int row = 0; row < glyph_height; ++row) {
                uint8_t bits = glyph_bitmaps[glyph][row];

                for (int column = 0; column < glyph_width; ++column) {
                    if ((bits & (uint8_t)(1U << (glyph_width - 1 - column))) == 0U) {
                        continue;
                    }

                    for (int dy = 0; dy < scale; ++dy) {
                        int py = y + row * scale + dy;

                        if (py < 0 || py >= view->height) {
                            continue;
                        }

                        uint32_t *pixels = view_row(view, py);

                        for (int dx = 0; dx < scale; ++dx) {
                            int px = x + column * scale + dx;

                            if (px >= 0 && px < view->width) {
                                pixels[px] = color;
                            }
                        }
                    }
                }
            }
        }

        x += (glyph_width + 1) * scale;
    }
}

/*
 * Inverse of sound_wavelet_analyzer_frequency_for_row: the spectrogram row
 * whose center frequency is closest to hz, in log-frequency space.
 */
static int spectrogram_row_for_frequency(const View *view, double hz) {
    double log_min = log2(view->min_hz);
    double log_max = log2(view->max_hz);

    if (log_max <= log_min || hz <= 0.0) {
        return -1;
    }

    double unit = (log_max - log2(hz)) / (log_max - log_min);
    int row = (int)lrint(unit * (double)view->spectrogram_height - 0.5);

    if (row < 0 || row >= view->spectrogram_height) {
        return -1;
    }

    return row;
}

/*
 * Draw the log-frequency axis: gutter background, tick marks and labels
 * for the standard 1-2-5 frequencies, and remember which rows carry faint
 * gridlines across the scrolling plot.
 */
static void view_draw_axis(View *view) {
    int top = view->spectrogram_top;
    int gutter = view->spectrogram_left;
    int scale = view->text_scale;

    for (int y = top; y < top + view->spectrogram_height; ++y) {
        uint32_t *pixels = view_row(view, y);

        for (int x = 0; x < gutter; ++x) {
            pixels[x] = axis_background_color;
        }
    }

    memset(view->grid_flags, 0, (size_t)view->spectrogram_height);

    int label_gap = 2 * scale;
    int last_label_bottom = top - label_gap;
    size_t tick_count = sizeof(frequency_ticks) / sizeof(frequency_ticks[0]);

    for (size_t i = 0; i < tick_count; ++i) {
        const FrequencyTick *tick = &frequency_ticks[i];

        if (tick->hz < view->min_hz || tick->hz > view->max_hz) {
            continue;
        }

        int row = spectrogram_row_for_frequency(view, tick->hz);

        if (row < 0) {
            continue;
        }

        int y = top + row;
        uint32_t *pixels = view_row(view, y);

        for (int x = gutter - 4 * scale; x < gutter; ++x) {
            if (x >= 0) {
                pixels[x] = axis_tick_color;
            }
        }

        if (tick->gridline) {
            view->grid_flags[row] = 1;
        }

        int text_height = glyph_height * scale;
        int text_top = y - text_height / 2;

        if (text_top < top) {
            text_top = top;
        }

        if (text_top + text_height > top + view->spectrogram_height) {
            text_top = top + view->spectrogram_height - text_height;
        }

        if (text_top < last_label_bottom + label_gap) {
            continue;
        }

        int text_x = gutter - 5 * scale - text_width_pixels(tick->label, scale);

        if (text_x < scale) {
            text_x = scale;
        }

        draw_text(view, tick->label, text_x, text_top, axis_text_color);
        last_label_bottom = text_top + text_height;
    }
}

static void view_fill_rows(View *view, int from, int rows, uint32_t color) {
    for (int y = from; y < from + rows; ++y) {
        uint32_t *row = view_row(view, y);

        for (int x = 0; x < view->width; ++x) {
            row[x] = color;
        }
    }
}

static void view_draw_palette(View *view) {
    int top = view->height - palette_height;

    for (int x = 0; x < view->width; ++x) {
        float unit = view->width <= 1 ? 0.0F : (float)x / (float)(view->width - 1);
        uint32_t color = pack_color(sound_colormap_viridis(unit));

        for (int y = top; y < view->height; ++y) {
            view_row(view, y)[x] = color;
        }
    }
}

/*
 * Match the pixel buffer and streaming texture to the window's current
 * pixel size, laying the panes out top to bottom: waveform, separator,
 * spectrogram, separator, palette strip.
 */
static bool view_sync(View *view, SoundError *error) {
    int width = 0;
    int height = 0;

    if (!SDL_GetWindowSizeInPixels(view->window, &width, &height)) {
        sound_error_set(error, "SDL_GetWindowSizeInPixels failed: %s", SDL_GetError());
        return false;
    }

    if (view->texture && width == view->width && height == view->height) {
        return true;
    }

    SDL_Texture *texture = SDL_CreateTexture(
        view->renderer,
        SDL_PIXELFORMAT_XRGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height
    );

    if (!texture) {
        sound_error_set(error, "SDL_CreateTexture failed: %s", SDL_GetError());
        return false;
    }

    (void)SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);

    int logical_width = 0;
    int logical_height = 0;
    (void)SDL_GetWindowSize(view->window, &logical_width, &logical_height);

    int text_scale = logical_width > 0 ?
        (int)lround((double)width / (double)logical_width) :
        1;

    if (text_scale < 1) {
        text_scale = 1;
    }

    if (text_scale > 4) {
        text_scale = 4;
    }

    uint32_t *pixels = malloc(sizeof(uint32_t) * (size_t)width * (size_t)height);
    int waveform_height = height / 4;
    if (waveform_height > 260) {
        waveform_height = 260;
    }

    int spectrogram_top = waveform_height + separator_height;
    int spectrogram_height =
        height - spectrogram_top - separator_height - palette_height;
    int spectrogram_left = (3 * (glyph_width + 1) + 8) * text_scale;
    float *bands = malloc(sizeof(float) * (size_t)spectrogram_height);
    float *column_db = malloc(sizeof(float) * (size_t)spectrogram_height);
    uint8_t *grid_flags = malloc((size_t)spectrogram_height);

    if (!pixels || !bands || !column_db || !grid_flags ||
        spectrogram_height < 16 || spectrogram_left >= width / 2) {
        SDL_DestroyTexture(texture);
        free(pixels);
        free(bands);
        free(column_db);
        free(grid_flags);
        sound_error_set(error, "could not allocate a %dx%d view", width, height);
        return false;
    }

    for (int i = 0; i < spectrogram_height; ++i) {
        bands[i] = floor_db;
    }

    if (view->texture) {
        SDL_DestroyTexture(view->texture);
    }

    free(view->pixels);
    free(view->bands);
    free(view->column_db);
    free(view->grid_flags);

    view->texture = texture;
    view->pixels = pixels;
    view->bands = bands;
    view->column_db = column_db;
    view->grid_flags = grid_flags;
    view->width = width;
    view->height = height;
    view->waveform_height = waveform_height;
    view->spectrogram_top = spectrogram_top;
    view->spectrogram_height = spectrogram_height;
    view->spectrogram_left = spectrogram_left;
    view->text_scale = text_scale;

    view_fill_rows(view, 0, height - palette_height, background_color);
    view_fill_rows(view, waveform_height, separator_height, separator_color);
    view_fill_rows(view, spectrogram_top + spectrogram_height, separator_height, separator_color);
    view_draw_palette(view);
    view_draw_axis(view);
    return true;
}

static void view_destroy(View *view) {
    if (view->texture) {
        SDL_DestroyTexture(view->texture);
    }

    if (view->renderer) {
        SDL_DestroyRenderer(view->renderer);
    }

    if (view->window) {
        SDL_DestroyWindow(view->window);
    }

    free(view->pixels);
    free(view->bands);
    free(view->column_db);
    free(view->grid_flags);
}

static float vertically_smoothed_band(const View *view, int y) {
    int rows = view->spectrogram_height;
    float center = view->column_db[y];

    if (rows <= 2) {
        return center;
    }

    if (y == 0) {
        return center * 0.70F + view->column_db[y + 1] * 0.30F;
    }

    if (y == rows - 1) {
        return view->column_db[y - 1] * 0.30F + center * 0.70F;
    }

    return view->column_db[y - 1] * 0.20F + center * 0.60F + view->column_db[y + 1] * 0.20F;
}

/*
 * Scroll the spectrogram plot left by however many columns of audio became
 * due this frame (leaving the axis gutter in place). The columns are then
 * rendered one at a time as their audio is analyzed.
 */
static void view_shift_columns(View *view, int columns) {
    int left = view->spectrogram_left;
    int plot_width = view->width - left;

    if (columns <= 0) {
        return;
    }

    if (columns > plot_width) {
        columns = plot_width;
    }

    size_t scroll_count = (size_t)(plot_width - columns);

    for (int y = 0; y < view->spectrogram_height; ++y) {
        uint32_t *row = view_row(view, view->spectrogram_top + y);
        memmove(row + left, row + left + columns, sizeof(uint32_t) * scroll_count);
    }
}

/*
 * Draw one analyzer column at pixel column x. The analyzer already
 * integrates power over time per band, so the display only adds a light
 * column-to-column smoothing and a faint gridline at decade frequencies.
 */
static void view_render_column(View *view, int x) {
    int rows = view->spectrogram_height;

    if (x < view->spectrogram_left || x >= view->width) {
        return;
    }

    for (int y = 0; y < rows; ++y) {
        float db = vertically_smoothed_band(view, y);
        view->bands[y] += band_smoothing * (db - view->bands[y]);

        uint32_t color = color_for_db(view->bands[y]);

        if (view->grid_flags[y]) {
            color = blend_color(color, gridline_color, 0.25F);
        }

        view_row(view, view->spectrogram_top + y)[x] = color;
    }
}

static int waveform_y(double value, double gain, int rows) {
    double scaled = value * gain;

    if (scaled > 1.0) {
        scaled = 1.0;
    } else if (scaled < -1.0) {
        scaled = -1.0;
    }

    int center = rows / 2;
    int y = center - (int)lrint(scaled * (double)center);

    if (y < 0) {
        return 0;
    }

    if (y >= rows) {
        return rows - 1;
    }

    return y;
}

static void view_draw_waveform(
    View *view,
    const float *samples,
    uint64_t sample_count,
    double peak
) {
    int rows = view->waveform_height;

    view_fill_rows(view, 0, rows, background_color);
    view_fill_rows(view, rows / 2, 1, midline_color);

    double gain = peak > 1.0e-7 ? fmin(40.0, 0.85 / peak) : 1.0;

    for (int x = 0; sample_count > 0 && x < view->width; ++x) {
        uint64_t begin = (uint64_t)x * sample_count / (uint64_t)view->width;
        uint64_t end = (uint64_t)(x + 1) * sample_count / (uint64_t)view->width;

        if (end <= begin) {
            end = begin + 1;
        }

        if (end > sample_count) {
            end = sample_count;
        }

        double low = samples[begin];
        double high = samples[begin];

        for (uint64_t i = begin + 1; i < end; ++i) {
            if (samples[i] < low) {
                low = samples[i];
            }

            if (samples[i] > high) {
                high = samples[i];
            }
        }

        int top = waveform_y(high, gain, rows);
        int bottom = waveform_y(low, gain, rows);

        for (int y = top; y <= bottom; ++y) {
            view_row(view, y)[x] = waveform_color;
        }
    }
}

static void update_title(
    View *view,
    const SoundInputFormat *format,
    const SoundWaveletAnalyzer *analyzer,
    double rms,
    double peak
) {
    char title[192];
    const char *mode = sound_wavelet_analyzer_synchrosqueezed(analyzer) ? "SST" : "raw CWT";

    (void)snprintf(
        title,
        sizeof(title),
        "Sounds   RMS %.1f dBFS   peak %.1f dBFS   %.0f Hz   %s   %.1f-%.0f Hz   %" PRIu64 " octaves   %" PRIu64 " voices",
        amplitude_db(rms),
        amplitude_db(peak),
        format->sample_rate,
        mode,
        sound_wavelet_analyzer_min_frequency(analyzer),
        sound_wavelet_analyzer_max_frequency(analyzer),
        sound_wavelet_analyzer_octave_count(analyzer),
        sound_wavelet_analyzer_voice_count(analyzer)
    );
    (void)SDL_SetWindowTitle(view->window, title);
}

static void write_ring_callback(
    const float *samples,
    uint32_t sample_count,
    void *user_data
) {
    sound_ring_buffer_write(user_data, samples, sample_count);
}

static bool handle_events(SoundWaveletAnalyzer *analyzer) {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            return false;
        }

        if (event.type == SDL_EVENT_KEY_DOWN &&
            (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q)) {
            return false;
        }

        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_S) {
            bool enabled = !sound_wavelet_analyzer_synchrosqueezed(analyzer);
            sound_wavelet_analyzer_set_synchrosqueezed(analyzer, enabled);
        }
    }

    return true;
}

static bool ensure_recording_directory(SoundError *error) {
    if (mkdir(recording_directory, 0755) == 0 || errno == EEXIST) {
        return true;
    }

    sound_error_set(error, "could not create %s", recording_directory);
    return false;
}

static bool write_raw_samples(
    const char *path,
    const float *samples,
    uint64_t sample_count,
    SoundError *error
) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        sound_error_set(error, "could not open %s", path);
        return false;
    }

    size_t written = fwrite(samples, sizeof(float), (size_t)sample_count, file);
    bool closed = fclose(file) == 0;

    if (written != (size_t)sample_count || !closed) {
        sound_error_set(error, "could not write %s", path);
        return false;
    }

    return true;
}

static bool build_recording_path(char *path, size_t path_size, SoundError *error) {
    time_t now = time(NULL);
    struct tm local_time;
    char timestamp[32];

    if (now == (time_t)-1 || !localtime_r(&now, &local_time)) {
        sound_error_set(error, "could not read current time");
        return false;
    }

    if (strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time) == 0) {
        sound_error_set(error, "could not format recording timestamp");
        return false;
    }

    int written = snprintf(
        path,
        path_size,
        "%s/%s%s%s",
        recording_directory,
        recording_file_prefix,
        timestamp,
        recording_file_extension
    );

    if (written <= 0 || (size_t)written >= path_size) {
        sound_error_set(error, "recording path is too long");
        return false;
    }

    return true;
}

static bool save_recording(
    const SoundRingBuffer *ring,
    uint64_t *sample_count,
    char *path,
    size_t path_size,
    SoundError *error
) {
    *sample_count = 0;
    path[0] = '\0';

    uint64_t capacity = sound_ring_buffer_capacity(ring);
    if (capacity == 0) {
        return true;
    }

    float *samples = malloc(sizeof(float) * (size_t)capacity);
    if (!samples) {
        sound_error_set(error, "could not allocate raw recording buffer");
        return false;
    }

    uint64_t count = sound_ring_buffer_read_latest(ring, samples, capacity);
    bool ok = ensure_recording_directory(error) &&
        build_recording_path(path, path_size, error) &&
        write_raw_samples(path, samples, count, error);

    free(samples);
    *sample_count = ok ? count : 0;
    return ok;
}

int main(void) {
    int status = 1;
    SoundRingBuffer *ring = NULL;
    SoundInputStream *stream = NULL;
    SoundWaveletAnalyzer *analyzer = NULL;
    float *analysis_samples = NULL;
    float *waveform = NULL;
    View view = {0};
    bool sdl_ready = false;

    SoundError error;
    sound_error_clear(&error);

    SoundInputFormat format;
    if (!sound_default_input_format(&format, &error)) {
        goto fail;
    }

    uint64_t column_samples =
        (uint64_t)llround(format.sample_rate / spectrogram_columns_per_second);
    if (column_samples < 32U) {
        column_samples = 32U;
    }

    uint64_t ring_capacity = (uint64_t)(format.sample_rate * 2.0);
    uint64_t recording_capacity = (uint64_t)(format.sample_rate * raw_recording_seconds);
    if (ring_capacity < recording_capacity) {
        ring_capacity = recording_capacity;
    }

    if (!sound_ring_buffer_create(ring_capacity, &ring, &error)) {
        goto fail;
    }

    SoundInputStreamOptions options = {
        .callback = write_ring_callback,
        .user_data = ring,
    };

    if (!sound_input_stream_open(&options, &stream, &format, &error)) {
        goto fail;
    }

    if (!sound_wavelet_analyzer_create(format.sample_rate, &analyzer, &error)) {
        goto fail;
    }

    view.min_hz = sound_wavelet_analyzer_min_frequency(analyzer);
    view.max_hz = sound_wavelet_analyzer_max_frequency(analyzer);

    uint64_t waveform_capacity = (uint64_t)(format.sample_rate * waveform_seconds);
    if (waveform_capacity < minimum_waveform_samples) {
        waveform_capacity = minimum_waveform_samples;
    }

    analysis_samples = malloc(sizeof(float) * (size_t)column_samples);
    waveform = malloc(sizeof(float) * (size_t)waveform_capacity);

    if (!analysis_samples || !waveform) {
        sound_error_set(&error, "could not allocate app buffers");
        goto fail;
    }

    (void)SDL_SetAppMetadata(app_name, "1.0", app_identifier);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        sound_error_set(&error, "SDL_Init failed: %s", SDL_GetError());
        goto fail;
    }

    sdl_ready = true;

    if (!SDL_CreateWindowAndRenderer(
            app_name,
            initial_window_width,
            initial_window_height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
            &view.window,
            &view.renderer
        )) {
        sound_error_set(&error, "SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        goto fail;
    }

    (void)SDL_SetWindowMinimumSize(view.window, minimum_window_width, minimum_window_height);
    (void)SDL_ShowWindow(view.window);
    (void)SDL_RaiseWindow(view.window);
    bool vsync = SDL_SetRenderVSync(view.renderer, 1);

    if (!view_sync(&view, &error)) {
        goto fail;
    }

    if (!sound_input_stream_start(stream, &error)) {
        goto fail;
    }

    uint64_t columns_drawn = 0;
    uint64_t frames_drawn = 0;
    uint64_t last_analyzed = 0;

    while (handle_events(analyzer)) {
        if (!view_sync(&view, &error)) {
            goto fail;
        }

        uint64_t waveform_count = sound_ring_buffer_read_latest(ring, waveform, waveform_capacity);
        uint64_t written = sound_ring_buffer_written(ring);

        if (written > last_analyzed + ring_capacity) {
            last_analyzed = written - ring_capacity;
        }

        uint64_t pending_columns = (written - last_analyzed) / column_samples;
        if (pending_columns > maximum_columns_per_frame) {
            pending_columns = maximum_columns_per_frame;
        }

        view_shift_columns(&view, (int)pending_columns);

        for (uint64_t column = 0; column < pending_columns; ++column) {
            uint64_t read_count = sound_ring_buffer_read_ending_at(
                ring,
                last_analyzed + column_samples,
                analysis_samples,
                column_samples
            );

            if (read_count != column_samples) {
                last_analyzed = written;
                break;
            }

            if (!sound_wavelet_analyzer_push(analyzer, analysis_samples, read_count, &error)) {
                goto fail;
            }

            last_analyzed += read_count;

            if (!sound_wavelet_analyzer_snapshot_db(
                    analyzer,
                    view.column_db,
                    (uint64_t)view.spectrogram_height,
                    &error
                )) {
                goto fail;
            }

            view_render_column(
                &view,
                view.width - (int)pending_columns + (int)column
            );
            ++columns_drawn;
        }

        double rms = 0.0;
        double peak = 0.0;
        sound_compute_levels(waveform, waveform_count, &rms, &peak);
        view_draw_waveform(&view, waveform, waveform_count, peak);

        ++frames_drawn;

        if (frames_drawn % 30U == 0U || columns_drawn == 0U) {
            update_title(&view, &format, analyzer, rms, peak);
        }

        (void)SDL_UpdateTexture(view.texture, NULL, view.pixels, view.width * (int)sizeof(uint32_t));
        (void)SDL_RenderClear(view.renderer);
        (void)SDL_RenderTexture(view.renderer, view.texture, NULL, NULL);
        (void)SDL_RenderPresent(view.renderer);

        if (!vsync) {
            SDL_Delay(16);
        }
    }

    status = 0;
    goto done;

fail:
    fprintf(stderr, "%s\n", sound_error_message(&error));

done:
    if (stream) {
        (void)sound_input_stream_stop(stream, &error);
    }

    if (status == 0 && ring) {
        uint64_t sample_count = 0;
        char recording_path[recording_path_capacity];
        if (save_recording(ring, &sample_count, recording_path, sizeof(recording_path), &error)) {
            printf("saved %" PRIu64 " raw sample(s) to %s\n", sample_count, recording_path);
        } else {
            fprintf(stderr, "%s\n", sound_error_message(&error));
            status = 1;
        }
    }

    sound_wavelet_analyzer_destroy(analyzer);
    sound_input_stream_close(stream);
    sound_ring_buffer_destroy(ring);
    view_destroy(&view);

    if (sdl_ready) {
        SDL_Quit();
    }

    free(analysis_samples);
    free(waveform);
    return status;
}
