#ifndef SOUNDS_ERROR_H
#define SOUNDS_ERROR_H

#include <stddef.h>

typedef struct SoundError {
    char message[256];
} SoundError;

void sound_error_clear(SoundError *error);
void sound_error_set(SoundError *error, const char *format, ...);
const char *sound_error_message(const SoundError *error);

#endif
