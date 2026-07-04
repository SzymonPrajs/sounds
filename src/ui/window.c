#include "internal.h"
#include "font.h"

#include <math.h>
#include <stdlib.h>

static const int separator_height = 2;

bool sound_ui_create(
    const SoundUiConfig *config,
    SoundUi **ui_out,
    SoundError *error
) {
    *ui_out = NULL;

    SoundUi *ui = calloc(1, sizeof(*ui));
    if (!ui) {
        sound_error_set(error, "could not allocate UI");
        return false;
    }

    ui->min_hz = config->min_hz;
    ui->max_hz = config->max_hz;

    (void)SDL_SetAppMetadata(
        config->app_name,
        config->app_version,
        config->app_identifier
    );

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        sound_error_set(error, "SDL_Init failed: %s", SDL_GetError());
        sound_ui_destroy(ui);
        return false;
    }

    ui->sdl_ready = true;

    if (!SDL_CreateWindowAndRenderer(
            config->app_name,
            config->initial_width,
            config->initial_height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
            &ui->window,
            &ui->renderer
        )) {
        sound_error_set(error, "SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        sound_ui_destroy(ui);
        return false;
    }

    (void)SDL_SetWindowMinimumSize(
        ui->window,
        config->minimum_width,
        config->minimum_height
    );
    (void)SDL_ShowWindow(ui->window);
    (void)SDL_RaiseWindow(ui->window);
    ui->vsync = SDL_SetRenderVSync(ui->renderer, 1);

    if (!sound_ui_sync(ui, error)) {
        sound_ui_destroy(ui);
        return false;
    }

    *ui_out = ui;
    return true;
}

void sound_ui_destroy(SoundUi *ui) {
    if (!ui) {
        return;
    }

    if (ui->texture) {
        SDL_DestroyTexture(ui->texture);
    }

    if (ui->renderer) {
        SDL_DestroyRenderer(ui->renderer);
    }

    if (ui->window) {
        SDL_DestroyWindow(ui->window);
    }

    if (ui->sdl_ready) {
        SDL_Quit();
    }

    free(ui->pixels);
    free(ui->bands);
    free(ui->grid_flags);
    free(ui);
}

void sound_ui_poll_events(
    SoundUi *ui,
    SoundAppMode current_mode,
    SoundUiEvents *events
) {
    (void)ui;

    *events = (SoundUiEvents){
        .quit = false,
        .toggle_sst = false,
        .mode_changed = false,
        .mode = current_mode,
    };

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            events->quit = true;
        }

        if (event.type != SDL_EVENT_KEY_DOWN) {
            continue;
        }

        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
            events->quit = true;
        } else if (event.key.key == SDLK_S) {
            events->toggle_sst = true;
        } else if (event.key.key == SDLK_1) {
            events->mode = SOUND_APP_MODE_TRANSIENT;
            events->mode_changed = current_mode != events->mode;
        } else if (event.key.key == SDLK_2) {
            events->mode = SOUND_APP_MODE_TONAL;
            events->mode_changed = current_mode != events->mode;
        } else if (event.key.key == SDLK_3) {
            events->mode = SOUND_APP_MODE_ROOM_DECAY;
            events->mode_changed = current_mode != events->mode;
        }
    }
}

bool sound_ui_sync(SoundUi *ui, SoundError *error) {
    int width = 0;
    int height = 0;

    if (!SDL_GetWindowSizeInPixels(ui->window, &width, &height)) {
        sound_error_set(error, "SDL_GetWindowSizeInPixels failed: %s", SDL_GetError());
        return false;
    }

    if (ui->texture && width == ui->width && height == ui->height) {
        return true;
    }

    int logical_width = 0;
    int logical_height = 0;
    (void)SDL_GetWindowSize(ui->window, &logical_width, &logical_height);
    (void)logical_height;

    int text_scale = logical_width > 0 ?
        (int)lround((double)width / (double)logical_width) :
        1;

    if (text_scale < 1) {
        text_scale = 1;
    }

    if (text_scale > 4) {
        text_scale = 4;
    }

    int banner_height = SOUND_UI_GLYPH_HEIGHT * text_scale + 6 * text_scale;
    int waveform_height = (height - banner_height) / 4;
    if (waveform_height > 260) {
        waveform_height = 260;
    }

    int spectrogram_top = banner_height + waveform_height + separator_height;
    int spectrogram_height = height - spectrogram_top;
    int spectrogram_left = (3 * (SOUND_UI_GLYPH_WIDTH + 1) + 8) * text_scale;

    if (width <= 0 || height <= 0 ||
        spectrogram_height < 16 || spectrogram_left >= width / 2) {
        sound_error_set(error, "could not allocate a %dx%d view", width, height);
        return false;
    }

    SDL_Texture *texture = SDL_CreateTexture(
        ui->renderer,
        SDL_PIXELFORMAT_XRGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height
    );

    if (!texture) {
        sound_error_set(error, "SDL_CreateTexture failed: %s", SDL_GetError());
        return false;
    }

    (void)SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);

    uint32_t *pixels = malloc(sizeof(uint32_t) * (size_t)width * (size_t)height);
    float *bands = malloc(sizeof(float) * (size_t)spectrogram_height);
    uint8_t *grid_flags = malloc((size_t)spectrogram_height);

    if (!pixels || !bands || !grid_flags) {
        SDL_DestroyTexture(texture);
        free(pixels);
        free(bands);
        free(grid_flags);
        sound_error_set(error, "could not allocate a %dx%d view", width, height);
        return false;
    }

    if (ui->texture) {
        SDL_DestroyTexture(ui->texture);
    }

    free(ui->pixels);
    free(ui->bands);
    free(ui->grid_flags);

    ui->texture = texture;
    ui->pixels = pixels;
    ui->bands = bands;
    ui->grid_flags = grid_flags;
    ui->width = width;
    ui->height = height;
    ui->banner_height = banner_height;
    ui->waveform_height = waveform_height;
    ui->spectrogram_top = spectrogram_top;
    ui->spectrogram_height = spectrogram_height;
    ui->spectrogram_left = spectrogram_left;
    ui->text_scale = text_scale;

    sound_ui_prepare_resized_buffer(ui);
    return true;
}

uint64_t sound_ui_spectrogram_rows(const SoundUi *ui) {
    return ui->spectrogram_height > 0 ? (uint64_t)ui->spectrogram_height : 0;
}

void sound_ui_present(SoundUi *ui) {
    (void)SDL_UpdateTexture(
        ui->texture,
        NULL,
        ui->pixels,
        ui->width * (int)sizeof(uint32_t)
    );
    (void)SDL_RenderClear(ui->renderer);
    (void)SDL_RenderTexture(ui->renderer, ui->texture, NULL, NULL);
    (void)SDL_RenderPresent(ui->renderer);

    if (!ui->vsync) {
        SDL_Delay(16);
    }
}
