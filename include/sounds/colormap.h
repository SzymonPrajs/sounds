#ifndef SOUNDS_COLORMAP_H
#define SOUNDS_COLORMAP_H

typedef struct SoundColor {
    float red;
    float green;
    float blue;
} SoundColor;

typedef enum SoundColormap {
    SOUND_COLORMAP_VIRIDIS,
    SOUND_COLORMAP_MAGMA,
    SOUND_COLORMAP_INFERNO,
    SOUND_COLORMAP_PLASMA,
    SOUND_COLORMAP_CIVIDIS,
    SOUND_COLORMAP_TURBO,
    SOUND_COLORMAP_COUNT,
} SoundColormap;

int sound_colormap_count(void);
SoundColormap sound_colormap_at(int index);
int sound_colormap_index(SoundColormap colormap);
const char *sound_colormap_name(SoundColormap colormap);
SoundColor sound_colormap_sample(SoundColormap colormap, float unit);

SoundColor sound_colormap_viridis(float unit);
SoundColor sound_colormap_magma(float unit);
SoundColor sound_colormap_inferno(float unit);
SoundColor sound_colormap_plasma(float unit);
SoundColor sound_colormap_cividis(float unit);
SoundColor sound_colormap_turbo(float unit);

#endif
