#include "sounds/colormap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void expect_near(float actual, float expected, float tolerance, const char *label) {
    if (fabsf(actual - expected) > tolerance) {
        fprintf(
            stderr,
            "%s: expected %.4f within %.4f, got %.4f\n",
            label,
            (double)expected,
            (double)tolerance,
            (double)actual
        );
        exit(EXIT_FAILURE);
    }
}

int main(void) {
    /* Reference values from matplotlib's viridis at 0, 0.5, and 1. */
    SoundColor darkest = sound_colormap_viridis(0.0F);
    expect_near(darkest.red, 0.2670F, 0.001F, "viridis(0) red");
    expect_near(darkest.green, 0.0049F, 0.001F, "viridis(0) green");
    expect_near(darkest.blue, 0.3294F, 0.001F, "viridis(0) blue");

    SoundColor middle = sound_colormap_viridis(0.5F);
    expect_near(middle.red, 0.1276F, 0.001F, "viridis(0.5) red");
    expect_near(middle.green, 0.5669F, 0.001F, "viridis(0.5) green");
    expect_near(middle.blue, 0.5506F, 0.001F, "viridis(0.5) blue");

    SoundColor brightest = sound_colormap_viridis(1.0F);
    expect_near(brightest.red, 0.9932F, 0.001F, "viridis(1) red");
    expect_near(brightest.green, 0.9062F, 0.001F, "viridis(1) green");
    expect_near(brightest.blue, 0.1439F, 0.001F, "viridis(1) blue");

    /* Out-of-range inputs clamp to the endpoints. */
    SoundColor below = sound_colormap_viridis(-1.0F);
    expect_near(below.red, darkest.red, 1.0e-6F, "clamped low red");

    SoundColor above = sound_colormap_viridis(2.0F);
    expect_near(above.red, brightest.red, 1.0e-6F, "clamped high red");

    /* Perceived brightness rises monotonically with input. */
    float previous = -1.0F;

    for (int step = 0; step <= 100; ++step) {
        SoundColor color = sound_colormap_viridis((float)step / 100.0F);
        float luminance = 0.2126F * color.red + 0.7152F * color.green + 0.0722F * color.blue;

        if (luminance + 1.0e-3F < previous) {
            fprintf(stderr, "luminance dips at step %d\n", step);
            return EXIT_FAILURE;
        }

        previous = luminance;
    }

    printf("colormap tests passed\n");
    return EXIT_SUCCESS;
}
