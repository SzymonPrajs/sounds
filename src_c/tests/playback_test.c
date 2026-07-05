#include "sounds/error.h"
#include "sounds/playback.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static bool expect(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
    }

    return condition;
}

int main(void) {
    SoundError error;
    bool ok = true;

    sound_error_clear(&error);

    ok = expect(!sound_playback_open(NULL, &error), "playback open accepted NULL") &&
        expect(
            sound_error_message(&error)[0] != '\0',
            "playback open did not report an error"
        );

    ok = expect(!sound_playback_is_playing(NULL), "NULL playback is playing") && ok;
    ok = expect(sound_playback_position(NULL) == 0, "NULL playback has a position") && ok;

    float sample = 0.0F;
    ok = expect(
        !sound_playback_start(NULL, &sample, 1, 48000.0, &error),
        "playback start accepted NULL"
    ) && ok;

    ok = expect(
        !sound_playback_stop(NULL, &error),
        "playback stop accepted NULL"
    ) && ok;

    if (ok) {
        printf("playback tests passed\n");
    }

    return ok ? 0 : 1;
}
