#include "sounds/analysis.h"
#include "sounds/args.h"
#include "sounds/capture.h"
#include "sounds/error.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *program) {
    fprintf(
        stderr,
        "Usage: %s [seconds] [fft-size] [output-directory]\n"
        "Defaults: seconds=5, fft-size=16384, output-directory=.\n",
        program
    );
}

static bool join_path(
    char *buffer,
    size_t buffer_size,
    const char *directory,
    const char *name
) {
    const char *separator = "";
    size_t length = strlen(directory);

    if (length > 0 && directory[length - 1] != '/') {
        separator = "/";
    }

    int written = snprintf(buffer, buffer_size, "%s%s%s", directory, separator, name);
    return written > 0 && (size_t)written < buffer_size;
}

static void print_format(const SoundInputFormat *format) {
    fprintf(stderr, "Input stream format:\n");
    fprintf(stderr, "  sample rate:        %.0f Hz\n", format->sample_rate);
    fprintf(stderr, "  format ID:          0x%08" PRIx32 "\n", format->format_id);
    fprintf(stderr, "  format flags:       0x%08" PRIx32 "\n", format->format_flags);
    fprintf(stderr, "  bits per channel:   %" PRIu32 "\n", format->bits_per_channel);
    fprintf(stderr, "  channels per frame: %" PRIu32 "\n", format->channels_per_frame);
    fprintf(stderr, "  bytes per frame:    %" PRIu32 "\n", format->bytes_per_frame);
}

int main(int argc, char **argv) {
    double seconds = 5.0;
    uint64_t fft_size = 16384;
    const char *output_directory = ".";

    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc >= 2 && (!sound_parse_seconds(argv[1], &seconds) || seconds <= 0.0)) {
        fprintf(stderr, "Invalid duration: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    if (argc >= 3 && !sound_parse_fft_size(argv[2], &fft_size)) {
        fprintf(stderr, "Invalid FFT size: %s. Use a power of two of at least 1024, such as 16384.\n", argv[2]);
        return 1;
    }

    if (argc >= 4) {
        output_directory = argv[3];
    }

    if (argc > 4) {
        print_usage(argv[0]);
        return 1;
    }

    char raw_path[PATH_MAX];
    char levels_path[PATH_MAX];
    char spectrum_path[PATH_MAX];

    if (!join_path(raw_path, sizeof(raw_path), output_directory, "mic_mono.f32") ||
        !join_path(levels_path, sizeof(levels_path), output_directory, "levels.csv") ||
        !join_path(spectrum_path, sizeof(spectrum_path), output_directory, "spectrum.csv")) {
        fprintf(stderr, "Output path is too long.\n");
        return 1;
    }

    SoundError error;
    sound_error_clear(&error);

    SoundInputFormat format;
    if (!sound_default_input_format(&format, &error)) {
        fprintf(stderr, "%s\n", sound_error_message(&error));
        return 1;
    }

    print_format(&format);
    fprintf(stderr, "\nRecording %.2f seconds from the default input...\n", seconds);

    SoundRecording recording;
    SoundCaptureOptions options = {
        .seconds = seconds,
    };

    if (!sound_capture_default_input(&options, &recording, &error)) {
        fprintf(stderr, "%s\n", sound_error_message(&error));
        return 1;
    }

    fprintf(stderr, "Captured %" PRIu64 " samples.\n", recording.sample_count);

    bool ok =
        sound_write_raw_f32(raw_path, recording.samples, recording.sample_count, &error) &&
        sound_write_levels_csv(
            levels_path,
            recording.samples,
            recording.sample_count,
            recording.format.sample_rate,
            1024,
            &error
        ) &&
        sound_write_spectrum_csv(
            spectrum_path,
            recording.samples,
            recording.sample_count,
            recording.format.sample_rate,
            fft_size,
            &error
        );

    sound_recording_free(&recording);

    if (!ok) {
        fprintf(stderr, "%s\n", sound_error_message(&error));
        return 1;
    }

    fprintf(stderr, "\nWrote:\n");
    fprintf(stderr, "  %s\n", raw_path);
    fprintf(stderr, "  %s\n", levels_path);
    fprintf(stderr, "  %s\n", spectrum_path);

    return 0;
}
