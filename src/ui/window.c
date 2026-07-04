#include "internal.h"
#include "font.h"

#include <math.h>
#include <stdlib.h>

static const int separator_height = 2;

static bool mode_for_digit(SDL_Keycode key, SoundAppMode *mode) {
    if (key < SDLK_1 || key > SDLK_9) {
        return false;
    }

    int index = (int)(key - SDLK_1);
    if (index >= sound_app_mode_count()) {
        return false;
    }

    *mode = sound_app_mode_at(index);
    return true;
}

static SoundColormap offset_colormap(SoundColormap colormap, int offset) {
    int count = sound_colormap_count();
    int index = sound_colormap_index(colormap) + offset;

    if (count <= 0) {
        return SOUND_COLORMAP_VIRIDIS;
    }

    while (index < 0) {
        index += count;
    }

    return sound_colormap_at(index % count);
}

static SoundUiMenuTab next_menu_tab(SoundUiMenuTab tab) {
    return tab == SOUND_UI_MENU_ANALYSIS ?
        SOUND_UI_MENU_SETTINGS :
        SOUND_UI_MENU_ANALYSIS;
}

static bool workspace_for_key(SDL_Keycode key, SoundWorkspace *workspace) {
    switch (key) {
        case SDLK_L:
            *workspace = SOUND_WORKSPACE_LIVE;
            return true;
        case SDLK_V:
            *workspace = SOUND_WORKSPACE_CLIPS;
            return true;
        case SDLK_O:
            *workspace = SOUND_WORKSPACE_SPECTRUM;
            return true;
        case SDLK_B:
            *workspace = SOUND_WORKSPACE_BAND;
            return true;
        case SDLK_X:
            *workspace = SOUND_WORKSPACE_COMPARE;
            return true;
        case SDLK_S:
            *workspace = SOUND_WORKSPACE_SETTINGS;
            return true;
        default:
            break;
    }

    return false;
}

static void render_texture_rect(
    SoundUi *ui,
    int source_x,
    int source_y,
    int width,
    int height,
    int target_x,
    int target_y
) {
    if (width <= 0 || height <= 0) {
        return;
    }

    SDL_FRect source = {
        .x = (float)source_x,
        .y = (float)source_y,
        .w = (float)width,
        .h = (float)height,
    };
    SDL_FRect target = {
        .x = (float)target_x,
        .y = (float)target_y,
        .w = (float)width,
        .h = (float)height,
    };

    (void)SDL_RenderTexture(ui->renderer, ui->texture, &source, &target);
}

static void render_present_texture(SoundUi *ui) {
    int plot_width = ui->width - ui->spectrogram_left;

    if (plot_width <= 0 ||
        ui->spectrogram_height <= 0 ||
        ui->spectrogram_origin == 0 ||
        ui->menu_open) {
        (void)SDL_RenderTexture(ui->renderer, ui->texture, NULL, NULL);
        return;
    }

    int top = ui->spectrogram_top;
    int left = ui->spectrogram_left;
    int origin = ui->spectrogram_origin % plot_width;
    int first_width = plot_width - origin;

    render_texture_rect(ui, 0, 0, ui->width, top, 0, 0);
    render_texture_rect(ui, 0, top, left, ui->spectrogram_height, 0, top);
    render_texture_rect(
        ui,
        left + origin,
        top,
        first_width,
        ui->spectrogram_height,
        left,
        top
    );
    render_texture_rect(
        ui,
        left,
        top,
        origin,
        ui->spectrogram_height,
        left + first_width,
        top
    );
}

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
    ui->colormap = config->colormap;

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
    SoundWorkspace current_workspace,
    SoundUiEvents *events
) {
    *events = (SoundUiEvents){
        .quit = false,
        .toggle_sst = false,
        .toggle_recording = false,
        .toggle_playback = false,
        .capture_clip = false,
        .cycle_audition = false,
        .cycle_band_method = false,
        .cycle_band_handle = false,
        .trim_clear = false,
        .mode_changed = false,
        .colormap_changed = false,
        .workspace_changed = false,
        .selected_band_delta = 0,
        .lower_band_delta = 0,
        .upper_band_delta = 0,
        .trim_start_delta = 0,
        .trim_end_delta = 0,
        .mode = current_mode,
        .colormap = ui->colormap,
        .workspace = current_workspace,
    };

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            events->quit = true;
        }

        if (event.type != SDL_EVENT_KEY_DOWN) {
            continue;
        }

        SDL_Keycode key = event.key.key;
        SoundAppMode selected_mode;
        SoundWorkspace selected_workspace;

        if (mode_for_digit(key, &selected_mode)) {
            events->mode = selected_mode;
            events->mode_changed = current_mode != events->mode;
            events->workspace = SOUND_WORKSPACE_LIVE;
            events->workspace_changed = current_workspace != events->workspace;
            ui->menu_open = false;
            continue;
        }

        if (workspace_for_key(key, &selected_workspace) && key != SDLK_S) {
            events->workspace = selected_workspace;
            events->workspace_changed = current_workspace != events->workspace;
            ui->menu_open = false;
            continue;
        }

        if (key == SDLK_ESCAPE) {
            if (ui->menu_open) {
                ui->menu_open = false;
            } else {
                events->quit = true;
            }
        } else if (key == SDLK_Q) {
            events->quit = true;
        } else if (key == SDLK_M) {
            ui->menu_open = !ui->menu_open;
        } else if (key == SDLK_TAB) {
            if (ui->menu_open) {
                ui->menu_tab = next_menu_tab(ui->menu_tab);
            } else {
                events->workspace = sound_workspace_offset(current_workspace, 1);
                events->workspace_changed = true;
            }
        } else if (key == SDLK_S) {
            if (ui->menu_open) {
                ui->menu_tab = SOUND_UI_MENU_SETTINGS;
            } else {
                events->workspace = SOUND_WORKSPACE_SETTINGS;
                events->workspace_changed = current_workspace != events->workspace;
            }
        } else if (key == SDLK_T) {
            events->toggle_sst = true;
        } else if (key == SDLK_R) {
            events->toggle_recording = true;
        } else if (key == SDLK_SPACE || key == SDLK_P) {
            events->toggle_playback = true;
        } else if (key == SDLK_RETURN) {
            events->capture_clip = true;
        } else if (key == SDLK_A) {
            events->cycle_audition = true;
        } else if (key == SDLK_F) {
            events->cycle_band_method = true;
        } else if (key == SDLK_H) {
            events->cycle_band_handle = true;
        } else if (key == SDLK_UP) {
            events->selected_band_delta = 1;
        } else if (key == SDLK_DOWN) {
            events->selected_band_delta = -1;
        } else if (key == SDLK_RIGHT && !ui->menu_open) {
            events->workspace = sound_workspace_offset(current_workspace, 1);
            events->workspace_changed = true;
        } else if (key == SDLK_LEFT && !ui->menu_open) {
            events->workspace = sound_workspace_offset(current_workspace, -1);
            events->workspace_changed = true;
        } else if (key == SDLK_LEFTBRACKET) {
            events->lower_band_delta = -1;
        } else if (key == SDLK_RIGHTBRACKET) {
            events->lower_band_delta = 1;
        } else if (key == SDLK_MINUS) {
            events->upper_band_delta = -1;
        } else if (key == SDLK_EQUALS) {
            events->upper_band_delta = 1;
        } else if (key == SDLK_COMMA) {
            events->trim_start_delta = 1;
        } else if (key == SDLK_PERIOD) {
            events->trim_end_delta = 1;
        } else if (key == SDLK_SLASH) {
            events->trim_clear = true;
        } else if (key == SDLK_C || (ui->menu_open && key == SDLK_RIGHT)) {
            ui->colormap = offset_colormap(ui->colormap, 1);
            events->colormap = ui->colormap;
            events->colormap_changed = true;
        } else if (ui->menu_open && key == SDLK_LEFT) {
            ui->colormap = offset_colormap(ui->colormap, -1);
            events->colormap = ui->colormap;
            events->colormap_changed = true;
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

    int banner_height = (SOUND_UI_GLYPH_HEIGHT * 2 + 10) * text_scale;
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
    ui->spectrogram_origin = 0;
    ui->text_scale = text_scale;

    sound_ui_prepare_resized_buffer(ui);
    return true;
}

uint64_t sound_ui_spectrogram_rows(const SoundUi *ui) {
    return ui->spectrogram_height > 0 ? (uint64_t)ui->spectrogram_height : 0;
}

SoundColormap sound_ui_colormap(const SoundUi *ui) {
    return ui->colormap;
}

bool sound_ui_menu_open(const SoundUi *ui) {
    return ui->menu_open;
}

void sound_ui_present(SoundUi *ui) {
    (void)SDL_UpdateTexture(
        ui->texture,
        NULL,
        ui->pixels,
        ui->width * (int)sizeof(uint32_t)
    );
    (void)SDL_RenderClear(ui->renderer);
    render_present_texture(ui);
    (void)SDL_RenderPresent(ui->renderer);

    if (!ui->vsync) {
        SDL_Delay(16);
    }
}
