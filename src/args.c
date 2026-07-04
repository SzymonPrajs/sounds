#include "sounds/args.h"

#include "sounds/analysis.h"

#include <errno.h>
#include <stdlib.h>

bool sound_parse_seconds(const char *text, double *seconds) {
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);

    if (errno != 0 || end == text || *end != '\0' || value < 0.0) {
        return false;
    }

    *seconds = value;
    return true;
}

bool sound_parse_fft_size(const char *text, uint64_t *fft_size) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *fft_size = (uint64_t)value;
    return sound_is_power_of_two(*fft_size) && *fft_size >= 1024;
}
