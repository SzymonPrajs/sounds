#include "sounds/colormap.h"

#include <stddef.h>

typedef struct SoundColormapData {
    const char *name;
    const SoundColor *stops;
    int stop_count;
} SoundColormapData;

static const SoundColor viridis_stops[] = {
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

static const SoundColor magma_stops[] = {
    {0.001462F, 0.000466F, 0.013866F},
    {0.078815F, 0.054184F, 0.211667F},
    {0.232077F, 0.059889F, 0.437695F},
    {0.390384F, 0.100379F, 0.501864F},
    {0.550287F, 0.161158F, 0.505719F},
    {0.716387F, 0.214982F, 0.475290F},
    {0.868793F, 0.287728F, 0.409303F},
    {0.967671F, 0.439703F, 0.359810F},
    {0.994738F, 0.624350F, 0.427397F},
    {0.995680F, 0.812706F, 0.572645F},
    {0.987053F, 0.991438F, 0.749504F},
};

static const SoundColor inferno_stops[] = {
    {0.001462F, 0.000466F, 0.013866F},
    {0.087411F, 0.044556F, 0.224813F},
    {0.258234F, 0.038571F, 0.406485F},
    {0.416331F, 0.090203F, 0.432943F},
    {0.578304F, 0.148039F, 0.404411F},
    {0.735683F, 0.215906F, 0.330245F},
    {0.865006F, 0.316822F, 0.226055F},
    {0.954506F, 0.468744F, 0.099874F},
    {0.987622F, 0.645320F, 0.039886F},
    {0.964394F, 0.843848F, 0.273391F},
    {0.988362F, 0.998364F, 0.644924F},
};

static const SoundColor plasma_stops[] = {
    {0.050383F, 0.029803F, 0.527975F},
    {0.254627F, 0.013882F, 0.615419F},
    {0.417642F, 0.000564F, 0.658390F},
    {0.562738F, 0.051545F, 0.641509F},
    {0.692840F, 0.165141F, 0.564522F},
    {0.798216F, 0.280197F, 0.469538F},
    {0.881443F, 0.392529F, 0.383229F},
    {0.949217F, 0.517763F, 0.295662F},
    {0.988260F, 0.652325F, 0.211364F},
    {0.988648F, 0.809579F, 0.145357F},
    {0.940015F, 0.975158F, 0.131326F},
};

static const SoundColor cividis_stops[] = {
    {0.000000F, 0.135112F, 0.304751F},
    {0.032110F, 0.201199F, 0.440785F},
    {0.208926F, 0.272546F, 0.424809F},
    {0.309601F, 0.340399F, 0.424790F},
    {0.401418F, 0.411790F, 0.440708F},
    {0.488697F, 0.485318F, 0.471008F},
    {0.582087F, 0.558670F, 0.468118F},
    {0.683950F, 0.638793F, 0.444251F},
    {0.785965F, 0.720438F, 0.399613F},
    {0.896818F, 0.811030F, 0.320982F},
    {0.995737F, 0.909344F, 0.217772F},
};

static const SoundColor turbo_stops[] = {
    {0.189950F, 0.071760F, 0.232170F},
    {0.269670F, 0.348780F, 0.796310F},
    {0.244270F, 0.609370F, 0.996970F},
    {0.097500F, 0.837140F, 0.803420F},
    {0.275970F, 0.970920F, 0.516530F},
    {0.643620F, 0.989990F, 0.233560F},
    {0.883310F, 0.865530F, 0.217190F},
    {0.996070F, 0.642770F, 0.191650F},
    {0.940840F, 0.355660F, 0.070310F},
    {0.764760F, 0.143740F, 0.010410F},
    {0.479600F, 0.015830F, 0.010550F},
};

#define STOP_COUNT(stops) ((int)(sizeof(stops) / sizeof((stops)[0])))

static const SoundColormapData colormaps[SOUND_COLORMAP_COUNT] = {
    [SOUND_COLORMAP_VIRIDIS] = {"viridis", viridis_stops, STOP_COUNT(viridis_stops)},
    [SOUND_COLORMAP_MAGMA] = {"magma", magma_stops, STOP_COUNT(magma_stops)},
    [SOUND_COLORMAP_INFERNO] = {"inferno", inferno_stops, STOP_COUNT(inferno_stops)},
    [SOUND_COLORMAP_PLASMA] = {"plasma", plasma_stops, STOP_COUNT(plasma_stops)},
    [SOUND_COLORMAP_CIVIDIS] = {"cividis", cividis_stops, STOP_COUNT(cividis_stops)},
    [SOUND_COLORMAP_TURBO] = {"turbo", turbo_stops, STOP_COUNT(turbo_stops)},
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

static const SoundColormapData *colormap_data(SoundColormap colormap) {
    if (colormap < 0 || colormap >= SOUND_COLORMAP_COUNT) {
        return &colormaps[SOUND_COLORMAP_VIRIDIS];
    }

    return &colormaps[colormap];
}

static SoundColor sample_stops(
    const SoundColor *stops,
    int stop_count,
    float unit
) {
    float t = clamp_unit(unit);
    float scaled = t * (float)(stop_count - 1);
    int index = (int)scaled;

    if (index >= stop_count - 1) {
        return stops[stop_count - 1];
    }

    SoundColor low = stops[index];
    SoundColor high = stops[index + 1];
    float amount = scaled - (float)index;

    return (SoundColor){
        .red = mix_channel(low.red, high.red, amount),
        .green = mix_channel(low.green, high.green, amount),
        .blue = mix_channel(low.blue, high.blue, amount),
    };
}

const char *sound_colormap_name(SoundColormap colormap) {
    return colormap_data(colormap)->name;
}

SoundColor sound_colormap_sample(SoundColormap colormap, float unit) {
    const SoundColormapData *data = colormap_data(colormap);
    return sample_stops(data->stops, data->stop_count, unit);
}

SoundColor sound_colormap_viridis(float unit) {
    return sound_colormap_sample(SOUND_COLORMAP_VIRIDIS, unit);
}

SoundColor sound_colormap_magma(float unit) {
    return sound_colormap_sample(SOUND_COLORMAP_MAGMA, unit);
}

SoundColor sound_colormap_inferno(float unit) {
    return sound_colormap_sample(SOUND_COLORMAP_INFERNO, unit);
}

SoundColor sound_colormap_plasma(float unit) {
    return sound_colormap_sample(SOUND_COLORMAP_PLASMA, unit);
}

SoundColor sound_colormap_cividis(float unit) {
    return sound_colormap_sample(SOUND_COLORMAP_CIVIDIS, unit);
}

SoundColor sound_colormap_turbo(float unit) {
    return sound_colormap_sample(SOUND_COLORMAP_TURBO, unit);
}
