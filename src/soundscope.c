/*
 * soundscope: a real-time microphone view in a window.
 *
 * Core Audio HAL capture and vDSP analysis feed a software-rendered
 * pixel buffer: raw waveform on top, a scrolling log-frequency
 * spectrogram below, and a Viridis palette strip along the bottom.
 * SDL only opens the window and presents the pixels; every drawn pixel
 * comes from the plain C in this file.
 */

#include "sounds/analysis.h"
#include "sounds/args.h"
#include "sounds/capture.h"
#include "sounds/colormap.h"
#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <SDL3/SDL.h>

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The dBFS range mapped onto the spectrogram palette. */
static const float floor_db = -90.0F;
static const float ceiling_db = -15.0F;
static const double display_floor_hz = 20.0;
static const uint64_t default_fft_size = 8192;

/* How quickly a spectrogram band may fall. */
static const float release_db_per_second = 90.0F;

static const uint32_t background_color = 0x05070C;
static const uint32_t separator_color = 0x10141C;
static const uint32_t midline_color = 0x1B2433;
static const uint32_t waveform_color = 0x95DDFF;

static const int separator_height = 2;
static const int palette_height = 8;

typedef struct View {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t *pixels;
    float *bands; /* smoothed dBFS per spectrogram row */
    float *column_db;
    int width;
    int height;
    int waveform_height;
    int spectrogram_top;
    int spectrogram_height;
} View;

static void print_usage(const char *program) {
    fprintf(
        stderr,
        "Usage: %s [duration-seconds] [fft-size]\n"
        "Defaults: duration=0 for until the window closes, fft-size=%" PRIu64 ".\n",
        program,
        default_fft_size
    );
}

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

    uint32_t *pixels = malloc(sizeof(uint32_t) * (size_t)width * (size_t)height);
    int waveform_height = height / 4;
    if (waveform_height > 260) {
        waveform_height = 260;
    }

    int spectrogram_top = waveform_height + separator_height;
    int spectrogram_height =
        height - spectrogram_top - separator_height - palette_height;
    float *bands = malloc(sizeof(float) * (size_t)spectrogram_height);
    float *column_db = malloc(sizeof(float) * (size_t)spectrogram_height);

    if (!pixels || !bands || !column_db || spectrogram_height < 16) {
        SDL_DestroyTexture(texture);
        free(pixels);
        free(bands);
        free(column_db);
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

    view->texture = texture;
    view->pixels = pixels;
    view->bands = bands;
    view->column_db = column_db;
    view->width = width;
    view->height = height;
    view->waveform_height = waveform_height;
    view->spectrogram_top = spectrogram_top;
    view->spectrogram_height = spectrogram_height;

    view_fill_rows(view, 0, height - palette_height, background_color);
    view_fill_rows(view, waveform_height, separator_height, separator_color);
    view_fill_rows(view, spectrogram_top + spectrogram_height, separator_height, separator_color);
    view_draw_palette(view);
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
}

/* Map spectrogram rows onto a logarithmic frequency axis, low notes at the bottom. */
static double frequency_at(double position, double sample_rate) {
    double max_hz = sample_rate * 0.5;
    double min_hz = max_hz > display_floor_hz ? display_floor_hz : 1.0;

    return min_hz * pow(max_hz / min_hz, position);
}

static double clamp_double(double value, double minimum, double maximum) {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static double bin_position_for_frequency(
    double hz,
    double sample_rate,
    uint64_t fft_size,
    uint64_t bin_count
) {
    double bin = hz * (double)fft_size / sample_rate;

    return clamp_double(bin, 1.0, (double)(bin_count - 1U));
}

static double amplitude_for_db(float db) {
    return pow(10.0, (double)db / 20.0);
}

static float interpolated_db_at_bin(
    const float *spectrum,
    uint64_t bin_count,
    double position
) {
    position = clamp_double(position, 1.0, (double)(bin_count - 1U));

    uint64_t lower = (uint64_t)floor(position);
    uint64_t upper = lower + 1U;

    if (upper >= bin_count) {
        return spectrum[lower];
    }

    double unit = position - (double)lower;
    double lower_amplitude = amplitude_for_db(spectrum[lower]);
    double upper_amplitude = amplitude_for_db(spectrum[upper]);
    double amplitude = lower_amplitude + (upper_amplitude - lower_amplitude) * unit;

    return (float)(20.0 * log10(fmax(amplitude, 1.0e-12)));
}

static float db_for_band(
    const float *spectrum,
    uint64_t bin_count,
    double first,
    double last
) {
    double amplitude_sum = 0.0;
    double weight_sum = 0.0;
    float peak_db = floor_db;

    if (last < first) {
        double swap = first;
        first = last;
        last = swap;
    }

    first = clamp_double(first, 1.0, (double)(bin_count - 1U));
    last = clamp_double(last, 1.0, (double)(bin_count - 1U));

    if (last - first < 1.0) {
        return interpolated_db_at_bin(spectrum, bin_count, (first + last) * 0.5);
    }

    uint64_t start = (uint64_t)floor(first);
    uint64_t end = (uint64_t)ceil(last);

    for (uint64_t bin = start; bin <= end && bin < bin_count; ++bin) {
        double left = fmax(first, (double)bin - 0.5);
        double right = fmin(last, (double)bin + 0.5);
        double weight = right - left;

        if (weight <= 0.0) {
            continue;
        }

        if (spectrum[bin] > peak_db) {
            peak_db = spectrum[bin];
        }

        amplitude_sum += amplitude_for_db(spectrum[bin]) * weight;
        weight_sum += weight;
    }

    if (weight_sum <= 0.0) {
        return interpolated_db_at_bin(spectrum, bin_count, (first + last) * 0.5);
    }

    float average_db = (float)(20.0 * log10(fmax(amplitude_sum / weight_sum, 1.0e-12)));

    return peak_db * 0.55F + average_db * 0.45F;
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

static uint64_t spectrogram_hop_size(uint64_t fft_size) {
    uint64_t hop = fft_size / 32U;

    if (hop < 256U) {
        return 256U;
    }

    if (hop > 1024U) {
        return 1024U;
    }

    return hop;
}

/*
 * Scroll the spectrogram one pixel left and draw the newest column from
 * the smoothed bands. Each row uses an anti-aliased log-frequency band,
 * with fast attack and fixed release so short impulses remain visible.
 */
static void view_push_spectrum(
    View *view,
    const float *spectrum,
    uint64_t bin_count,
    double sample_rate,
    uint64_t fft_size,
    float release_db
) {
    int rows = view->spectrogram_height;

    for (int y = 0; y < rows; ++y) {
        float db = floor_db;

        if (spectrum && bin_count > 1) {
            double top_hz = frequency_at(1.0 - (double)y / (double)rows, sample_rate);
            double bottom_hz = frequency_at(1.0 - (double)(y + 1) / (double)rows, sample_rate);
            double first = bin_position_for_frequency(bottom_hz, sample_rate, fft_size, bin_count);
            double last = bin_position_for_frequency(top_hz, sample_rate, fft_size, bin_count);

            db = db_for_band(spectrum, bin_count, first, last);
        }

        view->column_db[y] = db;
    }

    for (int y = 0; y < rows; ++y) {
        uint32_t *row = view_row(view, view->spectrogram_top + y);
        memmove(row, row + 1, sizeof(uint32_t) * (size_t)(view->width - 1));

        float db = vertically_smoothed_band(view, y);
        float fallen = view->bands[y] - release_db;
        view->bands[y] = db > fallen ? db : fallen;
        row[view->width - 1] = color_for_db(view->bands[y]);
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
    uint64_t fft_size,
    double rms,
    double peak
) {
    char title[128];

    (void)snprintf(
        title,
        sizeof(title),
        "soundscope   RMS %.1f dBFS   peak %.1f dBFS   %.0f Hz   FFT %" PRIu64 "   %.0f-%.0f Hz",
        amplitude_db(rms),
        amplitude_db(peak),
        format->sample_rate,
        fft_size,
        display_floor_hz,
        format->sample_rate * 0.5
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

static bool handle_events(void) {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            return false;
        }

        if (event.type == SDL_EVENT_KEY_DOWN &&
            (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q)) {
            return false;
        }
    }

    return true;
}

int main(int argc, char **argv) {
    double duration = 0.0;
    uint64_t fft_size = default_fft_size;

    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc >= 2 && !sound_parse_seconds(argv[1], &duration)) {
        fprintf(stderr, "Invalid duration: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    if (argc >= 3 && !sound_parse_fft_size(argv[2], &fft_size)) {
        fprintf(stderr, "Invalid FFT size: %s. Use a power of two of at least 1024.\n", argv[2]);
        return 1;
    }

    if (argc > 3) {
        print_usage(argv[0]);
        return 1;
    }

    int status = 1;
    SoundRingBuffer *ring = NULL;
    SoundInputStream *stream = NULL;
    SoundSpectrumAnalyzer *analyzer = NULL;
    float *fft_samples = NULL;
    float *spectrum = NULL;
    float *waveform = NULL;
    View view = {0};
    bool sdl_ready = false;

    SoundError error;
    sound_error_clear(&error);

    SoundInputFormat format;
    if (!sound_default_input_format(&format, &error)) {
        goto fail;
    }

    uint64_t ring_capacity = (uint64_t)(format.sample_rate * 2.0);
    if (fft_size <= UINT64_MAX / 4U && ring_capacity < fft_size * 4U) {
        ring_capacity = fft_size * 4U;
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

    if (!sound_spectrum_analyzer_create(fft_size, &analyzer, &error)) {
        goto fail;
    }

    uint64_t bin_count = sound_spectrum_analyzer_bin_count(analyzer);
    uint64_t hop_size = spectrogram_hop_size(fft_size);
    uint64_t waveform_capacity = (uint64_t)(format.sample_rate * 0.08);
    if (waveform_capacity < 1024U) {
        waveform_capacity = 1024U;
    }

    fft_samples = malloc(sizeof(float) * (size_t)fft_size);
    spectrum = malloc(sizeof(float) * (size_t)bin_count);
    waveform = malloc(sizeof(float) * (size_t)waveform_capacity);

    if (!fft_samples || !spectrum || !waveform) {
        sound_error_set(&error, "could not allocate soundscope buffers");
        goto fail;
    }

    (void)SDL_SetAppMetadata("soundscope", "1.0", "dev.szymon.soundscope");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        sound_error_set(&error, "SDL_Init failed: %s", SDL_GetError());
        goto fail;
    }

    sdl_ready = true;

    if (!SDL_CreateWindowAndRenderer(
            "soundscope",
            1100,
            640,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
            &view.window,
            &view.renderer
        )) {
        sound_error_set(&error, "SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        goto fail;
    }

    (void)SDL_SetWindowMinimumSize(view.window, 480, 320);
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
    uint64_t next_spectrum_end = 0;
    double start_time = (double)SDL_GetTicks() / 1000.0;
    double deadline = start_time + duration;

    while (handle_events()) {
        double now = (double)SDL_GetTicks() / 1000.0;
        if (duration > 0.0 && now >= deadline) {
            break;
        }

        if (!view_sync(&view, &error)) {
            goto fail;
        }

        uint64_t waveform_count = sound_ring_buffer_read_latest(ring, waveform, waveform_capacity);
        uint64_t written = sound_ring_buffer_written(ring);

        if (next_spectrum_end == 0 && written >= fft_size) {
            next_spectrum_end = fft_size;
        }

        if (next_spectrum_end > 0 && written > ring_capacity &&
            next_spectrum_end < written - ring_capacity + fft_size) {
            next_spectrum_end = written - ring_capacity + fft_size;
        }

        int columns_this_frame = 0;
        float release_per_column =
            release_db_per_second * (float)((double)hop_size / format.sample_rate);

        while (next_spectrum_end > 0 &&
               next_spectrum_end <= written &&
               columns_this_frame < 8) {
            uint64_t fft_count = sound_ring_buffer_read_ending_at(
                ring,
                next_spectrum_end,
                fft_samples,
                fft_size
            );

            if (fft_count == fft_size &&
                sound_spectrum_analyzer_compute(
                    analyzer,
                    fft_samples,
                    fft_count,
                    format.sample_rate,
                    spectrum,
                    bin_count,
                    &error
                )) {
                view_push_spectrum(
                    &view,
                    spectrum,
                    bin_count,
                    format.sample_rate,
                    fft_size,
                    release_per_column
                );
                ++columns_drawn;
                ++columns_this_frame;
            }

            next_spectrum_end += hop_size;
        }

        double rms = 0.0;
        double peak = 0.0;
        sound_compute_levels(waveform, waveform_count, &rms, &peak);
        view_draw_waveform(&view, waveform, waveform_count, peak);

        if (columns_drawn % 30 == 0 || columns_this_frame == 0) {
            update_title(&view, &format, fft_size, rms, peak);
        }

        (void)SDL_UpdateTexture(view.texture, NULL, view.pixels, view.width * (int)sizeof(uint32_t));
        (void)SDL_RenderClear(view.renderer);
        (void)SDL_RenderTexture(view.renderer, view.texture, NULL, NULL);
        (void)SDL_RenderPresent(view.renderer);

        if (!vsync) {
            SDL_Delay(16);
        }
    }

    printf("soundscope stopped after %" PRIu64 " column(s)\n", columns_drawn);
    status = 0;
    goto done;

fail:
    fprintf(stderr, "%s\n", sound_error_message(&error));

done:
    if (stream) {
        (void)sound_input_stream_stop(stream, &error);
    }

    sound_spectrum_analyzer_destroy(analyzer);
    sound_input_stream_close(stream);
    sound_ring_buffer_destroy(ring);
    view_destroy(&view);

    if (sdl_ready) {
        SDL_Quit();
    }

    free(fft_samples);
    free(spectrum);
    free(waveform);
    return status;
}
