#ifndef SOUNDS_ARGS_H
#define SOUNDS_ARGS_H

#include <stdbool.h>
#include <stdint.h>

/* Parses a non-negative decimal duration in seconds. */
bool sound_parse_seconds(const char *text, double *seconds);

/* Parses an FFT size: a power of two of at least 1024. */
bool sound_parse_fft_size(const char *text, uint64_t *fft_size);

#endif
