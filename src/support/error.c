#include "sounds/error.h"

#include <stdarg.h>
#include <stdio.h>

void sound_error_clear(SoundError *error) {
    if (error) {
        error->message[0] = '\0';
    }
}

void sound_error_set(SoundError *error, const char *format, ...) {
    if (!error) {
        return;
    }

    va_list args;
    va_start(args, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
}

const char *sound_error_message(const SoundError *error) {
    if (!error || error->message[0] == '\0') {
        return "unknown error";
    }

    return error->message;
}
