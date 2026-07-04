#include "sounds/colormap.h"

static const SoundColor viridis[] = {
    {0.267004F, 0.004874F, 0.329415F},
    {0.282623F, 0.140926F, 0.457517F},
    {0.253935F, 0.265254F, 0.529983F},
    {0.206756F, 0.371758F, 0.553117F},
    {0.163625F, 0.471133F, 0.558148F},
    {0.127568F, 0.566949F, 0.550556F},
    {0.134692F, 0.658636F, 0.517649F},
    {0.266941F, 0.748751F, 0.440573F},
    {0.477504F, 0.821444F, 0.318195F},
    {0.741388F, 0.873449F, 0.149561F},
    {0.993248F, 0.906157F, 0.143936F},
};

static float clamp_unit(float value) {
    if (value < 0.0F) {
        return 0.0F;
    }

    if (value > 1.0F) {
        return 1.0F;
    }

    return value;
}

static float mix_channel(float a, float b, float amount) {
    return a + (b - a) * amount;
}

SoundColor sound_colormap_viridis(float unit) {
    float t = clamp_unit(unit);
    int stop_count = (int)(sizeof(viridis) / sizeof(viridis[0]));
    float scaled = t * (float)(stop_count - 1);
    int index = (int)scaled;

    if (index >= stop_count - 1) {
        return viridis[stop_count - 1];
    }

    SoundColor low = viridis[index];
    SoundColor high = viridis[index + 1];
    float amount = scaled - (float)index;

    return (SoundColor){
        .red = mix_channel(low.red, high.red, amount),
        .green = mix_channel(low.green, high.green, amount),
        .blue = mix_channel(low.blue, high.blue, amount),
    };
}
