/*
 * live-sound: a real-time microphone dashboard for truecolor terminals.
 *
 * Three views of the default input device share the screen: the raw
 * waveform, RMS and peak levels, and a scrolling log-frequency
 * spectrogram. Every frame is composed into an off-screen buffer and
 * written to the terminal in a single call, so the display never tears.
 */

#include "sounds/analysis.h"
#include "sounds/args.h"
#include "sounds/capture.h"
#include "sounds/colormap.h"
#include "sounds/error.h"
#include "sounds/ring_buffer.h"

#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* The dBFS range mapped onto the spectrogram palette. */
static const float floor_db = -90.0F;
static const float ceiling_db = -15.0F;

typedef struct Rgb {
    int red;
    int green;
    int blue;
} Rgb;

typedef struct Layout {
    int rows;
    int cols;
    int waveform_rows;
    int spectrogram_rows;
} Layout;

/* One terminal frame, composed off-screen and written in a single call. */
typedef struct Frame {
    char *bytes;
    size_t length;
    size_t capacity;
    bool out_of_memory;
} Frame;

/* Scrolling history of spectrum columns; column `cols - 1` is newest. */
typedef struct Spectrogram {
    float *cells;
    int rows;
    int cols;
} Spectrogram;

static volatile sig_atomic_t should_stop = 0;

static void handle_signal(int signal_number) {
    (void)signal_number;
    should_stop = 1;
}

static void write_ring_callback(
    const float *samples,
    uint32_t sample_count,
    void *user_data
) {
    sound_ring_buffer_write(user_data, samples, sample_count);
}

static void terminal_enter(void) {
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H", stdout);
    fflush(stdout);
}

static void terminal_leave(void) {
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
}

static void terminal_size(int *rows, int *cols) {
    struct winsize size;

    *rows = 36;
    *cols = 100;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
        size.ws_row >= 18 &&
        size.ws_col >= 60) {
        *rows = size.ws_row;
        *cols = size.ws_col;
    }
}

static double monotonic_seconds(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }

    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void sleep_frame(void) {
    const struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = 33333333L,
    };

    (void)nanosleep(&interval, NULL);
}

static void print_usage(const char *program) {
    fprintf(
        stderr,
        "Usage: %s [duration-seconds] [fft-size]\n"
        "Defaults: duration=0 for until Ctrl-C, fft-size=4096.\n",
        program
    );
}

static bool frame_reserve(Frame *frame, size_t extra) {
    size_t needed = frame->length + extra;
    if (needed <= frame->capacity) {
        return true;
    }

    size_t capacity = frame->capacity ? frame->capacity : 4096;
    while (capacity < needed) {
        capacity *= 2;
    }

    char *grown = realloc(frame->bytes, capacity);
    if (!grown) {
        frame->out_of_memory = true;
        return false;
    }

    frame->bytes = grown;
    frame->capacity = capacity;
    return true;
}

static void frame_bytes(Frame *frame, const char *bytes, size_t length) {
    if (!frame_reserve(frame, length)) {
        return;
    }

    memcpy(frame->bytes + frame->length, bytes, length);
    frame->length += length;
}

__attribute__((format(printf, 2, 3)))
static void frame_text(Frame *frame, const char *format, ...) {
    va_list args;
    va_list sizing;

    va_start(args, format);
    va_copy(sizing, args);
    int length = vsnprintf(NULL, 0, format, sizing);
    va_end(sizing);

    if (length > 0 && frame_reserve(frame, (size_t)length + 1)) {
        (void)vsnprintf(frame->bytes + frame->length, (size_t)length + 1, format, args);
        frame->length += (size_t)length;
    }

    va_end(args);
}

static bool rgb_equal(Rgb a, Rgb b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static Rgb color_for_db(float db) {
    float unit = (db - floor_db) / (ceiling_db - floor_db);
    SoundColor color = sound_colormap_viridis(unit);

    return (Rgb){
        .red = (int)lrintf(color.red * 255.0F),
        .green = (int)lrintf(color.green * 255.0F),
        .blue = (int)lrintf(color.blue * 255.0F),
    };
}

static Layout layout_for_terminal(void) {
    int rows = 0;
    int cols = 0;
    terminal_size(&rows, &cols);

    /* Fixed lines: two headers, three captions, and the palette bar. */
    const int fixed_rows = 6;
    int waveform_rows = rows >= 30 ? 9 : 6;
    int spectrogram_rows = rows - fixed_rows - waveform_rows;

    if (spectrogram_rows < 5) {
        spectrogram_rows = 5;
        waveform_rows = rows - fixed_rows - spectrogram_rows;
        if (waveform_rows < 3) {
            waveform_rows = 3;
        }
    }

    return (Layout){
        .rows = rows,
        .cols = cols,
        .waveform_rows = waveform_rows,
        .spectrogram_rows = spectrogram_rows,
    };
}

static bool spectrogram_resize(Spectrogram *spectrogram, int rows, int cols) {
    if (spectrogram->cells && spectrogram->rows == rows && spectrogram->cols == cols) {
        return true;
    }

    float *cells = malloc(sizeof(float) * (size_t)rows * (size_t)cols);
    if (!cells) {
        return false;
    }

    for (int i = 0; i < rows * cols; ++i) {
        cells[i] = floor_db;
    }

    free(spectrogram->cells);
    spectrogram->cells = cells;
    spectrogram->rows = rows;
    spectrogram->cols = cols;
    return true;
}

/* Map screen rows onto a logarithmic frequency axis, low notes at the bottom. */
static double frequency_for_row(int row, int rows, double sample_rate) {
    double max_hz = sample_rate * 0.5;
    double min_hz = max_hz > 20.0 ? 20.0 : 1.0;
    double position = rows <= 1 ? 0.0 : (double)(rows - 1 - row) / (double)(rows - 1);

    return min_hz * pow(max_hz / min_hz, position);
}

static uint64_t bin_for_frequency(
    double hz,
    double sample_rate,
    uint64_t fft_size,
    uint64_t bin_count
) {
    uint64_t bin = (uint64_t)llround(hz * (double)fft_size / sample_rate);

    if (bin < 1) {
        return 1;
    }

    if (bin >= bin_count) {
        return bin_count - 1U;
    }

    return bin;
}

static void spectrogram_push(
    Spectrogram *spectrogram,
    const float *spectrum,
    uint64_t bin_count,
    double sample_rate,
    uint64_t fft_size
) {
    for (int row = 0; row < spectrogram->rows; ++row) {
        float *line = spectrogram->cells + (size_t)row * (size_t)spectrogram->cols;

        if (spectrogram->cols > 1) {
            memmove(line, line + 1, sizeof(float) * (size_t)(spectrogram->cols - 1));
        }

        float db = floor_db;
        if (spectrum && bin_count > 1) {
            double hz = frequency_for_row(row, spectrogram->rows, sample_rate);
            db = spectrum[bin_for_frequency(hz, sample_rate, fft_size, bin_count)];
        }

        line[spectrogram->cols - 1] = db;
    }
}

static double amplitude_db(double value) {
    return 20.0 * log10(fmax(value, 1.0e-12));
}

static int waveform_row(double value, double gain, int rows) {
    double scaled = value * gain;

    if (scaled > 1.0) {
        scaled = 1.0;
    } else if (scaled < -1.0) {
        scaled = -1.0;
    }

    int center = rows / 2;
    int row = center - (int)lrint(scaled * (double)center);

    if (row < 0) {
        return 0;
    }

    if (row >= rows) {
        return rows - 1;
    }

    return row;
}

static void compose_waveform(
    Frame *frame,
    const float *samples,
    uint64_t sample_count,
    int rows,
    int cols,
    double peak
) {
    char *grid = malloc((size_t)rows * (size_t)cols);
    if (!grid) {
        for (int row = 0; row < rows; ++row) {
            frame_text(frame, "\x1b[2K\n");
        }
        return;
    }

    memset(grid, ' ', (size_t)rows * (size_t)cols);
    memset(grid + (size_t)(rows / 2) * (size_t)cols, '-', (size_t)cols);

    double gain = peak > 1.0e-7 ? fmin(40.0, 0.85 / peak) : 1.0;

    for (int col = 0; sample_count > 0 && col < cols; ++col) {
        uint64_t begin = (uint64_t)col * sample_count / (uint64_t)cols;
        uint64_t end = (uint64_t)(col + 1) * sample_count / (uint64_t)cols;

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

        int high_row = waveform_row(high, gain, rows);
        int low_row = waveform_row(low, gain, rows);

        for (int row = high_row; row <= low_row; ++row) {
            grid[(size_t)row * (size_t)cols + (size_t)col] = '|';
        }
    }

    for (int row = 0; row < rows; ++row) {
        frame_text(frame, "\x1b[2K\x1b[38;2;149;221;255m");
        frame_bytes(frame, grid + (size_t)row * (size_t)cols, (size_t)cols);
        frame_text(frame, "\x1b[0m\n");
    }

    free(grid);
}

static void frame_background(Frame *frame, Rgb color) {
    frame_text(frame, "\x1b[48;2;%d;%d;%dm", color.red, color.green, color.blue);
}

static void compose_spectrogram(Frame *frame, const Spectrogram *spectrogram) {
    for (int row = 0; row < spectrogram->rows; ++row) {
        const float *line = spectrogram->cells + (size_t)row * (size_t)spectrogram->cols;
        Rgb previous = {-1, -1, -1};

        frame_text(frame, "\x1b[2K");

        for (int col = 0; col < spectrogram->cols; ++col) {
            Rgb color = color_for_db(line[col]);

            if (!rgb_equal(color, previous)) {
                frame_background(frame, color);
                previous = color;
            }

            frame_bytes(frame, " ", 1);
        }

        frame_text(frame, "\x1b[0m\n");
    }
}

static void compose_palette(Frame *frame, int cols) {
    frame_text(frame, "\x1b[2K");

    for (int col = 0; col < cols; ++col) {
        double unit = cols <= 1 ? 0.0 : (double)col / (double)(cols - 1);
        frame_background(frame, color_for_db(floor_db + (float)unit * (ceiling_db - floor_db)));
        frame_bytes(frame, " ", 1);
    }

    frame_text(frame, "\x1b[0m\n");
}

static void compose_dashboard(
    Frame *frame,
    const Layout *layout,
    const SoundInputFormat *format,
    const float *waveform,
    uint64_t waveform_count,
    const Spectrogram *spectrogram,
    uint64_t frames_drawn,
    uint64_t fft_size
) {
    double rms = 0.0;
    double peak = 0.0;
    sound_compute_levels(waveform, waveform_count, &rms, &peak);

    /* CSI ?2026 brackets the frame as one synchronized update. */
    frame_text(frame, "\x1b[?2026h\x1b[H");
    frame_text(
        frame,
        "\x1b[2K\x1b[1;38;2;235;244;255mCore Audio live view\x1b[0m  "
        "raw waveform + vDSP spectrogram  %.0f Hz  FFT %" PRIu64 "\n",
        format->sample_rate,
        fft_size
    );
    frame_text(
        frame,
        "\x1b[2KRMS %.1f dBFS  Peak %.1f dBFS  Samples %" PRIu64
        "  Spectrogram range %.0f..%.0f dBFS\n",
        amplitude_db(rms),
        amplitude_db(peak),
        waveform_count,
        (double)floor_db,
        (double)ceiling_db
    );
    frame_text(frame, "\x1b[2KWaveform: raw microphone amplitude over time, auto-scaled for visibility\n");

    compose_waveform(frame, waveform, waveform_count, layout->waveform_rows, layout->cols, peak);

    frame_text(
        frame,
        "\x1b[2KSpectrogram: low frequencies at bottom, time moves left to right%s\n",
        frames_drawn == 0 ? "  warming up" : ""
    );

    compose_spectrogram(frame, spectrogram);
    compose_palette(frame, layout->cols);

    frame_text(frame, "\x1b[2Kquiet%*s\x1b[?2026l", layout->cols - 6, "loud");
}

int main(int argc, char **argv) {
    double duration = 0.0;
    uint64_t fft_size = 4096;

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

    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);

    int status = 1;
    SoundRingBuffer *ring = NULL;
    SoundInputStream *stream = NULL;
    SoundSpectrumAnalyzer *analyzer = NULL;
    float *fft_samples = NULL;
    float *spectrum = NULL;
    float *waveform = NULL;
    Frame frame = {0};
    Spectrogram spectrogram = {0};

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
    uint64_t waveform_capacity = fft_size > 4096 ? fft_size : 4096;
    fft_samples = malloc(sizeof(float) * (size_t)fft_size);
    spectrum = malloc(sizeof(float) * (size_t)bin_count);
    waveform = malloc(sizeof(float) * (size_t)waveform_capacity);

    if (!fft_samples || !spectrum || !waveform) {
        sound_error_set(&error, "could not allocate live dashboard buffers");
        goto fail;
    }

    if (!sound_input_stream_start(stream, &error)) {
        goto fail;
    }

    uint64_t frames_drawn = 0;
    double deadline = monotonic_seconds() + duration;

    terminal_enter();

    while (!should_stop) {
        if (duration > 0.0 && monotonic_seconds() >= deadline) {
            break;
        }

        Layout layout = layout_for_terminal();
        if (!spectrogram_resize(&spectrogram, layout.spectrogram_rows, layout.cols)) {
            break;
        }

        uint64_t waveform_count = sound_ring_buffer_read_latest(ring, waveform, waveform_capacity);
        uint64_t fft_count = sound_ring_buffer_read_latest(ring, fft_samples, fft_size);

        const float *newest_column = NULL;
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
            newest_column = spectrum;
            ++frames_drawn;
        }

        spectrogram_push(&spectrogram, newest_column, bin_count, format.sample_rate, fft_size);

        frame.length = 0;
        compose_dashboard(
            &frame,
            &layout,
            &format,
            waveform,
            waveform_count,
            &spectrogram,
            frames_drawn,
            fft_size
        );

        if (frame.out_of_memory) {
            break;
        }

        fwrite(frame.bytes, 1, frame.length, stdout);
        fflush(stdout);
        sleep_frame();
    }

    terminal_leave();
    (void)sound_input_stream_stop(stream, &error);

    printf("live view stopped after %" PRIu64 " spectrogram frame(s)\n", frames_drawn);
    status = 0;
    goto done;

fail:
    fprintf(stderr, "%s\n", sound_error_message(&error));

done:
    sound_spectrum_analyzer_destroy(analyzer);
    sound_input_stream_close(stream);
    sound_ring_buffer_destroy(ring);
    free(spectrogram.cells);
    free(frame.bytes);
    free(fft_samples);
    free(spectrum);
    free(waveform);
    return status;
}
