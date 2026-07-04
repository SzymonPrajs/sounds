#ifndef SOUNDS_COLORMAP_H
#define SOUNDS_COLORMAP_H

typedef struct SoundColor {
    float red;
    float green;
    float blue;
} SoundColor;

/* Viridis perceptual colormap; unit in [0, 1], channels in [0, 1]. */
SoundColor sound_colormap_viridis(float unit);

#endif
