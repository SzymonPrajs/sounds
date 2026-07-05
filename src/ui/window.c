#include "internal.h"
#include "font.h"

#include "sounds/defer.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int separator_height = 2;
/* Keep the macOS Stage Manager side strip visible on the initial launch. */
static const int stage_manager_side_reserve = 240;

typedef struct SoundUiInitialWindow {
    int x;
    int y;
    int width;
    int height;
    bool has_position;
} SoundUiInitialWindow;

static int clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static SoundUiInitialWindow initial_window_layout(const SoundUiConfig *config) {
    SoundUiInitialWindow layout = {
        .x = 0,
        .y = 0,
        .width = config->initial_width,
        .height = config->initial_height,
        .has_position = false,
    };

    if (layout.width < config->minimum_width) {
        layout.width = config->minimum_width;
    }

    if (layout.height < config->minimum_height) {
        layout.height = config->minimum_height;
    }

    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect usable;
    if (display == 0 || !SDL_GetDisplayUsableBounds(display, &usable)) {
        return layout;
    }

    int reserved_width = stage_manager_side_reserve;
    if (usable.w - reserved_width < config->minimum_width) {
        reserved_width = 0;
    }

    int maximum_width = usable.w - reserved_width;
    int preferred_width = config->initial_width > 0 ?
        config->initial_width :
        (int)lround((double)usable.w * 0.72);

    layout.width = clamp_int(preferred_width, config->minimum_width, maximum_width);
    layout.height = usable.h > config->minimum_height ?
        usable.h :
        config->minimum_height;
    layout.x = usable.x + usable.w - layout.width;
    layout.y = usable.y;
    layout.has_position = true;
    return layout;
}

static int menu_item_count(SoundUiMenuTab tab) {
    switch (tab) {
        case SOUND_UI_MENU_ANALYSIS:
            return sound_app_mode_count() + 1;
        case SOUND_UI_MENU_BANDS:
            return sound_frequency_band_count() + 2;
        case SOUND_UI_MENU_COLORS:
            return sound_colormap_count();
        case SOUND_UI_MENU_COUNT:
            break;
    }

    return 0;
}

static int wrapped_index(int index, int count) {
    if (count <= 0) {
        return 0;
    }

    while (index < 0) {
        index += count;
    }

    return index % count;
}

static SoundUiMenuTab offset_menu_tab(SoundUiMenuTab tab, int offset) {
    return (SoundUiMenuTab)wrapped_index((int)tab + offset, SOUND_UI_MENU_COUNT);
}

static void sync_menu_cursor_to_current(
    SoundUi *ui,
    SoundUiMenuTab tab,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band
) {
    switch (tab) {
        case SOUND_UI_MENU_ANALYSIS:
            ui->menu_cursors[tab] = sound_app_mode_index(mode);
            break;
        case SOUND_UI_MENU_BANDS:
            ui->menu_cursors[tab] = sound_frequency_band_index(frequency_band);
            break;
        case SOUND_UI_MENU_COLORS:
            ui->menu_cursors[tab] = sound_colormap_index(ui->colormap);
            break;
        case SOUND_UI_MENU_COUNT:
            break;
    }
}

void sound_ui_open_menu(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band
) {
    ui->menu_open = true;
    ui->menu_opened_from_toolbar = false;
    ui->custom_range_editing = false;
    ui->menu_tab = SOUND_UI_MENU_ANALYSIS;

    for (int tab = 0; tab < SOUND_UI_MENU_COUNT; ++tab) {
        sync_menu_cursor_to_current(ui, (SoundUiMenuTab)tab, mode, frequency_band);
    }
}

void sound_ui_close_menu(SoundUi *ui) {
    ui->menu_open = false;
    ui->menu_opened_from_toolbar = false;
    ui->custom_range_editing = false;
    ui->imui.active_id = 0U;
    ui->imui.focus_id = 0U;
}

void sound_ui_select_menu_tab(
    SoundUi *ui,
    SoundUiMenuTab tab,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band
) {
    if (tab < 0 || tab >= SOUND_UI_MENU_COUNT) {
        return;
    }

    ui->custom_range_editing = false;
    ui->menu_tab = tab;
    sync_menu_cursor_to_current(ui, ui->menu_tab, mode, frequency_band);
}

void sound_ui_switch_menu_tab(
    SoundUi *ui,
    int offset,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band
) {
    sound_ui_select_menu_tab(
        ui,
        offset_menu_tab(ui->menu_tab, offset),
        mode,
        frequency_band
    );
}

void sound_ui_move_menu_cursor(SoundUi *ui, int offset) {
    ui->custom_range_editing = false;
    int count = menu_item_count(ui->menu_tab);
    ui->menu_cursors[ui->menu_tab] =
        wrapped_index(ui->menu_cursors[ui->menu_tab] + offset, count);
}

static double clamp_double(double value, double minimum, double maximum) {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static void format_custom_edit_value(double hz, char *text, size_t capacity) {
    if (hz >= 100.0) {
        (void)snprintf(text, capacity, "%.0f", hz);
    } else {
        (void)snprintf(text, capacity, "%.1f", hz);
    }
}

static void begin_custom_range_edit(SoundUi *ui, bool high) {
    ui->custom_range_editing = true;
    ui->custom_range_edit_high = high;
    format_custom_edit_value(
        high ? ui->custom_max_hz : ui->custom_min_hz,
        ui->custom_range_text,
        sizeof(ui->custom_range_text)
    );
}

static void append_custom_range_text(SoundUi *ui, const char *text) {
    size_t length = strlen(ui->custom_range_text);
    bool has_decimal = strchr(ui->custom_range_text, '.') != NULL;

    for (const char *cursor = text; cursor && *cursor != '\0'; ++cursor) {
        char value = *cursor;
        bool digit = value >= '0' && value <= '9';

        if (value == '.') {
            if (has_decimal) {
                continue;
            }

            has_decimal = true;
        } else if (!digit) {
            continue;
        }

        if (length + 1U >= sizeof(ui->custom_range_text)) {
            break;
        }

        ui->custom_range_text[length] = value;
        ++length;
        ui->custom_range_text[length] = '\0';
    }
}

static void backspace_custom_range_text(SoundUi *ui) {
    size_t length = strlen(ui->custom_range_text);

    if (length > 0U) {
        ui->custom_range_text[length - 1U] = '\0';
    }
}

static void commit_custom_range_edit(
    SoundUi *ui,
    SoundFrequencyBand current_frequency_band,
    SoundUiEvents *events
) {
    char *end = NULL;
    double value = strtod(ui->custom_range_text, &end);

    ui->custom_range_editing = false;
    if (end == ui->custom_range_text || !isfinite(value)) {
        return;
    }

    double full_min = ui->full_min_hz > 0.0 ? ui->full_min_hz : 10.0;
    double full_max = ui->full_max_hz > full_min ? ui->full_max_hz : 24000.0;
    double low = ui->custom_min_hz;
    double high = ui->custom_max_hz;

    if (ui->custom_range_edit_high) {
        high = clamp_double(value, full_min * 1.01, full_max);
        if (low >= high / 1.01) {
            low = fmax(full_min, high / 1.01);
        }
    } else {
        low = clamp_double(value, full_min, full_max / 1.01);
        if (high <= low * 1.01) {
            high = fmin(full_max, low * 1.01);
        }
    }

    ui->custom_min_hz = low;
    ui->custom_max_hz = high;
    ui->frequency_band = SOUND_FREQUENCY_BAND_CUSTOM;

    events->frequency_band = SOUND_FREQUENCY_BAND_CUSTOM;
    events->frequency_band_changed =
        current_frequency_band != SOUND_FREQUENCY_BAND_CUSTOM;
    events->custom_min_hz = low;
    events->custom_max_hz = high;
    events->custom_range_changed = true;
}

void sound_ui_commit_menu_item(
    SoundUi *ui,
    SoundAppMode current_mode,
    SoundFrequencyBand current_frequency_band,
    SoundWorkspace current_workspace,
    SoundUiEvents *events
) {
    int cursor = ui->menu_cursors[ui->menu_tab];

    switch (ui->menu_tab) {
        case SOUND_UI_MENU_ANALYSIS:
            ui->custom_range_editing = false;
            if (cursor < sound_app_mode_count()) {
                events->mode = sound_app_mode_at(cursor);
                events->mode_changed = current_mode != events->mode;
                events->workspace = SOUND_WORKSPACE_LIVE;
                events->workspace_changed = current_workspace != events->workspace;
            } else {
                events->toggle_sst = true;
            }
            break;
        case SOUND_UI_MENU_BANDS:
            if (cursor < sound_frequency_band_count()) {
                ui->custom_range_editing = false;
                events->frequency_band = sound_frequency_band_at(cursor);
                events->frequency_band_changed =
                    current_frequency_band != events->frequency_band;
            } else {
                begin_custom_range_edit(ui, cursor > sound_frequency_band_count());
            }
            break;
        case SOUND_UI_MENU_COLORS:
            ui->custom_range_editing = false;
            events->colormap = sound_colormap_at(cursor);
            events->colormap_changed = ui->colormap != events->colormap;
            ui->colormap = events->colormap;
            break;
        case SOUND_UI_MENU_COUNT:
            break;
    }
}

static bool workspace_for_key(SDL_Keycode key, SoundWorkspace *workspace) {
    switch (key) {
        case SDLK_L:
            *workspace = SOUND_WORKSPACE_LIVE;
            return true;
        case SDLK_V:
            *workspace = SOUND_WORKSPACE_CLIPS;
            return true;
        case SDLK_T:
            *workspace = SOUND_WORKSPACE_TRIM;
            return true;
        case SDLK_O:
            *workspace = SOUND_WORKSPACE_SPECTRUM;
            return true;
        case SDLK_B:
            *workspace = SOUND_WORKSPACE_BAND;
            return true;
        default:
            break;
    }

    return false;
}

static void append_recording_rename_text(
    SoundUiEvents *events,
    const char *text
) {
    size_t length = strlen(events->recording_rename_text);

    for (const char *cursor = text; cursor && *cursor != '\0'; ++cursor) {
        unsigned char value = (unsigned char)*cursor;
        if (value < 32U || value > 126U) {
            continue;
        }

        if (length + 1U >= sizeof(events->recording_rename_text)) {
            break;
        }

        events->recording_rename_text[length] = (char)value;
        ++length;
        events->recording_rename_text[length] = '\0';
    }
}

static void merge_pending_ui_events(SoundUi *ui, SoundUiEvents *events) {
    SoundUiEvents *pending = &ui->pending_ui_events;

    if (pending->mode_changed) {
        events->mode = pending->mode;
        events->mode_changed = true;
    }

    if (pending->workspace_changed) {
        events->workspace = pending->workspace;
        events->workspace_changed = true;
    }

    if (pending->toggle_sst) {
        events->toggle_sst = true;
    }

    if (pending->toggle_recording) {
        events->toggle_recording = true;
    }

    if (pending->toggle_playback) {
        events->toggle_playback = true;
    }

    if (pending->cycle_audition) {
        events->cycle_audition = true;
    }

    if (pending->cycle_band_method) {
        events->cycle_band_method = true;
    }

    if (pending->cycle_band_handle) {
        events->cycle_band_handle = true;
    }

    if (pending->frequency_band_changed || pending->custom_range_changed) {
        events->frequency_band = pending->frequency_band;
        events->frequency_band_changed = pending->frequency_band_changed;
    }

    if (pending->custom_range_changed) {
        events->custom_min_hz = pending->custom_min_hz;
        events->custom_max_hz = pending->custom_max_hz;
        events->custom_range_changed = true;
    }

    if (pending->colormap_changed) {
        events->colormap = pending->colormap;
        events->colormap_changed = true;
    }

    if (pending->recording_delta != 0) {
        events->recording_delta += pending->recording_delta;
    }

    if (pending->select_recording) {
        events->select_recording = true;
    }

    if (pending->delete_recording) {
        events->delete_recording = true;
    }

    if (pending->cancel_recording_delete) {
        events->cancel_recording_delete = true;
    }

    if (pending->begin_recording_rename) {
        events->begin_recording_rename = true;
    }

    if (pending->cancel_recording_rename) {
        events->cancel_recording_rename = true;
    }

    if (pending->commit_recording_rename) {
        events->commit_recording_rename = true;
    }

    if (pending->recording_rename_backspace) {
        events->recording_rename_backspace = true;
    }

    if (pending->recording_rename_text_replace) {
        events->recording_rename_text_replace = true;
        (void)snprintf(
            events->recording_rename_text,
            sizeof(events->recording_rename_text),
            "%s",
            pending->recording_rename_text
        );
    } else if (pending->recording_rename_text[0] != '\0') {
        append_recording_rename_text(events, pending->recording_rename_text);
    }

    if (pending->trim_set_handle) {
        events->trim_set_handle = true;
        events->trim_set_handle_end = pending->trim_set_handle_end;
        events->trim_set_sample = pending->trim_set_sample;
    }

    if (pending->band_set_edge) {
        events->band_set_edge = true;
        events->band_set_edge_upper = pending->band_set_edge_upper;
        events->band_set_hz = pending->band_set_hz;
    }

    if (pending->lower_band_delta != 0) {
        events->lower_band_delta += pending->lower_band_delta;
    }

    if (pending->upper_band_delta != 0) {
        events->upper_band_delta += pending->upper_band_delta;
    }

    if (pending->trim_commit) {
        events->trim_commit = true;
    }

    if (pending->trim_clear) {
        events->trim_clear = true;
    }

    ui->pending_ui_events = (SoundUiEvents){0};
}

static void scale_mouse_event_coordinates(SDL_Event *event, float scale_x, float scale_y) {
    switch (event->type) {
        case SDL_EVENT_MOUSE_MOTION:
            event->motion.x *= scale_x;
            event->motion.y *= scale_y;
            event->motion.xrel *= scale_x;
            event->motion.yrel *= scale_y;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            event->button.x *= scale_x;
            event->button.y *= scale_y;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            event->wheel.mouse_x *= scale_x;
            event->wheel.mouse_y *= scale_y;
            break;
        default:
            break;
    }
}

static void scale_window_event_to_pixels(SoundUi *ui, SDL_Event *event) {
    int logical_width = 0;
    int logical_height = 0;

    if (!SDL_GetWindowSize(ui->window, &logical_width, &logical_height) ||
        logical_width <= 0 ||
        logical_height <= 0) {
        return;
    }

    float scale_x = (float)((double)ui->width / (double)logical_width);
    float scale_y = (float)((double)ui->height / (double)logical_height);
    scale_mouse_event_coordinates(event, scale_x, scale_y);
}

static void scale_render_event_to_pixels_if_needed(SoundUi *ui, SDL_Event *event) {
    int render_width = 0;
    int render_height = 0;

    if (!SDL_GetRenderOutputSize(ui->renderer, &render_width, &render_height) ||
        render_width <= 0 ||
        render_height <= 0 ||
        (render_width == ui->width && render_height == ui->height)) {
        return;
    }

    float scale_x = (float)((double)ui->width / (double)render_width);
    float scale_y = (float)((double)ui->height / (double)render_height);
    scale_mouse_event_coordinates(event, scale_x, scale_y);
}

static void feed_imui_event(SoundUi *ui, const SDL_Event *event) {
    SDL_Event imui_event = *event;

    if (SDL_ConvertEventToRenderCoordinates(ui->renderer, &imui_event)) {
        scale_render_event_to_pixels_if_needed(ui, &imui_event);
    } else {
        scale_window_event_to_pixels(ui, &imui_event);
    }

    sound_imui_sdl_handle_event(&ui->imui_input, &imui_event);
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
        !ui->spectrogram_wrap_enabled ||
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

static void sync_imui_draw(SoundUi *ui) {
    sound_imui_set_draw(
        &ui->imui,
        sound_imui_sdl_draw(&ui->imui_adapter, ui)
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
    defer {
        sound_ui_destroy(ui);
    }

    ui->min_hz = config->min_hz;
    ui->max_hz = config->max_hz;
    ui->full_min_hz = config->full_min_hz > 0.0 ? config->full_min_hz : config->min_hz;
    ui->full_max_hz = config->full_max_hz > ui->full_min_hz ?
        config->full_max_hz :
        config->max_hz;
    ui->custom_min_hz = config->custom_min_hz;
    ui->custom_max_hz = config->custom_max_hz;
    ui->colormap = config->colormap;
    ui->frequency_band = config->frequency_band;

    (void)SDL_SetAppMetadata(
        config->app_name,
        config->app_version,
        config->app_identifier
    );

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        sound_error_set(error, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    ui->sdl_ready = true;

    SoundUiInitialWindow initial = initial_window_layout(config);

    if (!SDL_CreateWindowAndRenderer(
            config->app_name,
            initial.width,
            initial.height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN,
            &ui->window,
            &ui->renderer
        )) {
        sound_error_set(error, "SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return false;
    }

    (void)SDL_SetWindowMinimumSize(
        ui->window,
        config->minimum_width,
        config->minimum_height
    );

    if (initial.has_position) {
        (void)SDL_SetWindowPosition(ui->window, initial.x, initial.y);
        (void)SDL_SyncWindow(ui->window);
    }

    (void)SDL_ShowWindow(ui->window);
    (void)SDL_RaiseWindow(ui->window);
    ui->vsync = SDL_SetRenderVSync(ui->renderer, 1);
    sync_imui_draw(ui);

    if (!sound_ui_sync(ui, error)) {
        return false;
    }

    *ui_out = ui;
    ui = NULL;
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
    free(ui->spectrogram_db);
    free(ui->grid_flags);
    free(ui->spectrogram_filled);
    free(ui);
}

void sound_ui_poll_events(
    SoundUi *ui,
    SoundAppMode current_mode,
    SoundFrequencyBand current_frequency_band,
    SoundWorkspace current_workspace,
    bool recording_rename_active,
    SoundUiEvents *events
) {
    *events = (SoundUiEvents){
        .quit = false,
        .toggle_sst = false,
        .toggle_recording = false,
        .toggle_playback = false,
        .cycle_audition = false,
        .cycle_band_method = false,
        .cycle_band_handle = false,
        .select_recording = false,
        .delete_recording = false,
        .cancel_recording_delete = false,
        .begin_recording_rename = false,
        .cancel_recording_rename = false,
        .commit_recording_rename = false,
        .recording_rename_backspace = false,
        .recording_rename_text_replace = false,
        .trim_select_start = false,
        .trim_select_end = false,
        .trim_set_handle = false,
        .trim_set_handle_end = false,
        .band_set_edge = false,
        .band_set_edge_upper = false,
        .trim_commit = false,
        .trim_clear = false,
        .mode_changed = false,
        .colormap_changed = false,
        .frequency_band_changed = false,
        .custom_range_changed = false,
        .workspace_changed = false,
        .selected_band_delta = 0,
        .lower_band_delta = 0,
        .upper_band_delta = 0,
        .trim_move_delta = 0,
        .recording_delta = 0,
        .trim_set_sample = 0,
        .band_set_hz = 0.0,
        .mode = current_mode,
        .colormap = ui->colormap,
        .frequency_band = current_frequency_band,
        .custom_min_hz = ui->custom_min_hz,
        .custom_max_hz = ui->custom_max_hz,
        .workspace = current_workspace,
        .recording_rename_text = "",
    };

    merge_pending_ui_events(ui, events);
    sound_imui_input_begin_frame(&ui->imui_input);

    if (recording_rename_active || (ui->menu_open && ui->custom_range_editing)) {
        SDL_StartTextInput(ui->window);
    } else {
        SDL_StopTextInput(ui->window);
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        feed_imui_event(ui, &event);

        if (event.type == SDL_EVENT_QUIT) {
            events->quit = true;
        }

        if (recording_rename_active && ui->recording_rename_inline_active) {
            continue;
        }

        if (recording_rename_active) {
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                append_recording_rename_text(events, event.text.text);
                continue;
            }

            if (event.type != SDL_EVENT_KEY_DOWN) {
                continue;
            }

            SDL_Keycode key = event.key.key;
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                events->commit_recording_rename = true;
            } else if (key == SDLK_ESCAPE) {
                events->cancel_recording_rename = true;
            } else if (key == SDLK_BACKSPACE) {
                events->recording_rename_backspace = true;
            }
            continue;
        }

        if (ui->menu_open && ui->custom_range_editing) {
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                append_custom_range_text(ui, event.text.text);
                continue;
            }

            if (event.type != SDL_EVENT_KEY_DOWN) {
                continue;
            }

            SDL_Keycode edit_key = event.key.key;
            if (edit_key == SDLK_RETURN || edit_key == SDLK_KP_ENTER) {
                commit_custom_range_edit(ui, current_frequency_band, events);
            } else if (edit_key == SDLK_ESCAPE) {
                ui->custom_range_editing = false;
            } else if (edit_key == SDLK_BACKSPACE) {
                backspace_custom_range_text(ui);
            }
            continue;
        }

        if (event.type != SDL_EVENT_KEY_DOWN) {
            continue;
        }

        SDL_Keycode key = event.key.key;
        SoundWorkspace selected_workspace;

        if (key == SDLK_ESCAPE) {
            if (ui->menu_open) {
                sound_ui_close_menu(ui);
            } else {
                events->quit = true;
            }
        } else if (key == SDLK_Q) {
            events->quit = true;
        } else if (key == SDLK_M) {
            if (ui->menu_open) {
                sound_ui_close_menu(ui);
            } else {
                sound_ui_open_menu(ui, current_mode, current_frequency_band);
            }
        } else if (ui->menu_open &&
            (key == SDLK_RIGHT || key == SDLK_TAB)) {
            sound_ui_switch_menu_tab(ui, 1, current_mode, current_frequency_band);
        } else if (ui->menu_open && key == SDLK_LEFT) {
            sound_ui_switch_menu_tab(ui, -1, current_mode, current_frequency_band);
        } else if (ui->menu_open && key == SDLK_DOWN) {
            sound_ui_move_menu_cursor(ui, 1);
        } else if (ui->menu_open && key == SDLK_UP) {
            sound_ui_move_menu_cursor(ui, -1);
        } else if (ui->menu_open &&
            (key == SDLK_RETURN || key == SDLK_KP_ENTER)) {
            sound_ui_commit_menu_item(
                ui,
                current_mode,
                current_frequency_band,
                current_workspace,
                events
            );
        } else if (ui->menu_open) {
            continue;
        } else if (key == SDLK_TAB) {
            events->workspace = sound_workspace_offset(current_workspace, 1);
            events->workspace_changed = true;
        } else if (key == SDLK_R) {
            events->toggle_recording = true;
        } else if (key == SDLK_SPACE || key == SDLK_P) {
            events->toggle_playback = true;
        } else if (key == SDLK_A) {
            events->cycle_audition = true;
        } else if (key == SDLK_F) {
            events->cycle_band_method = true;
        } else if (key == SDLK_H) {
            events->cycle_band_handle = true;
        } else if (workspace_for_key(key, &selected_workspace)) {
            events->workspace = selected_workspace;
            events->workspace_changed = current_workspace != events->workspace;
        } else if (current_workspace == SOUND_WORKSPACE_CLIPS &&
            (key == SDLK_RETURN || key == SDLK_KP_ENTER)) {
            events->select_recording = true;
        } else if (current_workspace == SOUND_WORKSPACE_CLIPS &&
            key == SDLK_N) {
            events->begin_recording_rename = true;
        } else if (current_workspace == SOUND_WORKSPACE_CLIPS &&
            key == SDLK_D) {
            events->delete_recording = true;
        } else if (current_workspace == SOUND_WORKSPACE_CLIPS &&
            key == SDLK_UP) {
            events->recording_delta = -1;
        } else if (current_workspace == SOUND_WORKSPACE_CLIPS &&
            key == SDLK_DOWN) {
            events->recording_delta = 1;
        } else if (current_workspace == SOUND_WORKSPACE_TRIM &&
            key == SDLK_COMMA) {
            events->trim_select_start = true;
        } else if (current_workspace == SOUND_WORKSPACE_TRIM &&
            key == SDLK_PERIOD) {
            events->trim_select_end = true;
        } else if (current_workspace == SOUND_WORKSPACE_TRIM &&
            key == SDLK_LEFT) {
            events->trim_move_delta = -1;
        } else if (current_workspace == SOUND_WORKSPACE_TRIM &&
            key == SDLK_RIGHT) {
            events->trim_move_delta = 1;
        } else if (current_workspace == SOUND_WORKSPACE_TRIM &&
            key == SDLK_DOWN) {
            events->trim_move_delta = -10;
        } else if (current_workspace == SOUND_WORKSPACE_TRIM &&
            key == SDLK_UP) {
            events->trim_move_delta = 10;
        } else if (current_workspace == SOUND_WORKSPACE_TRIM &&
            key == SDLK_SLASH) {
            events->trim_commit = true;
        } else if (current_workspace == SOUND_WORKSPACE_TRIM &&
            (key == SDLK_BACKSPACE || key == SDLK_DELETE)) {
            events->trim_clear = true;
        } else if (current_workspace == SOUND_WORKSPACE_BAND &&
            key == SDLK_UP) {
            events->selected_band_delta = 1;
        } else if (current_workspace == SOUND_WORKSPACE_BAND &&
            key == SDLK_DOWN) {
            events->selected_band_delta = -1;
        } else if (current_workspace == SOUND_WORKSPACE_CLIPS &&
            key == SDLK_LEFTBRACKET) {
            events->recording_delta = -1;
        } else if (current_workspace == SOUND_WORKSPACE_CLIPS &&
            key == SDLK_RIGHTBRACKET) {
            events->recording_delta = 1;
        } else if (current_workspace == SOUND_WORKSPACE_BAND &&
            key == SDLK_LEFTBRACKET) {
            events->lower_band_delta = -1;
        } else if (current_workspace == SOUND_WORKSPACE_BAND &&
            key == SDLK_RIGHTBRACKET) {
            events->lower_band_delta = 1;
        } else if (current_workspace == SOUND_WORKSPACE_BAND &&
            key == SDLK_MINUS) {
            events->upper_band_delta = -1;
        } else if (current_workspace == SOUND_WORKSPACE_BAND &&
            key == SDLK_EQUALS) {
            events->upper_band_delta = 1;
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

    int banner_height = (SOUND_UI_GLYPH_HEIGHT + 10) * text_scale;
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
    defer {
        if (texture) {
            SDL_DestroyTexture(texture);
        }
    }

    (void)SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);

    uint32_t *pixels = malloc(sizeof(uint32_t) * (size_t)width * (size_t)height);
    float *bands = malloc(sizeof(float) * (size_t)spectrogram_height);
    float *spectrogram_db =
        malloc(sizeof(float) * (size_t)(width - spectrogram_left) * (size_t)spectrogram_height);
    uint8_t *grid_flags = malloc((size_t)spectrogram_height);
    uint8_t *spectrogram_filled = malloc((size_t)(width - spectrogram_left));
    defer {
        free(pixels);
    }
    defer {
        free(bands);
    }
    defer {
        free(spectrogram_db);
    }
    defer {
        free(grid_flags);
    }
    defer {
        free(spectrogram_filled);
    }

    if (!pixels || !bands || !spectrogram_db || !grid_flags || !spectrogram_filled) {
        sound_error_set(error, "could not allocate a %dx%d view", width, height);
        return false;
    }

    if (ui->texture) {
        SDL_DestroyTexture(ui->texture);
    }

    free(ui->pixels);
    free(ui->bands);
    free(ui->spectrogram_db);
    free(ui->grid_flags);
    free(ui->spectrogram_filled);

    ui->texture = texture;
    ui->pixels = pixels;
    ui->bands = bands;
    ui->spectrogram_db = spectrogram_db;
    ui->grid_flags = grid_flags;
    ui->spectrogram_filled = spectrogram_filled;
    texture = NULL;
    pixels = NULL;
    bands = NULL;
    spectrogram_db = NULL;
    grid_flags = NULL;
    spectrogram_filled = NULL;
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
    sync_imui_draw(ui);
    return true;
}

uint64_t sound_ui_spectrogram_rows(const SoundUi *ui) {
    return ui->spectrogram_height > 0 ? (uint64_t)ui->spectrogram_height : 0;
}

uint64_t sound_ui_spectrogram_columns(const SoundUi *ui) {
    int columns = ui->width - ui->spectrogram_left;
    return columns > 0 ? (uint64_t)columns : 0;
}

SoundColormap sound_ui_colormap(const SoundUi *ui) {
    return ui->colormap;
}

SoundFrequencyBand sound_ui_frequency_band(const SoundUi *ui) {
    return ui->frequency_band;
}

bool sound_ui_menu_open(const SoundUi *ui) {
    return ui->menu_open;
}

void sound_ui_set_frequency_band(
    SoundUi *ui,
    SoundFrequencyBand band,
    double min_hz,
    double max_hz
) {
    if (!ui || min_hz <= 0.0 || max_hz <= min_hz) {
        return;
    }

    ui->frequency_band = band;
    ui->min_hz = min_hz;
    ui->max_hz = max_hz;
    sound_ui_clear_spectrogram(ui);
}

void sound_ui_set_custom_frequency_range(
    SoundUi *ui,
    double min_hz,
    double max_hz
) {
    if (!ui || min_hz <= 0.0 || max_hz <= min_hz) {
        return;
    }

    ui->custom_min_hz = min_hz;
    ui->custom_max_hz = max_hz;
}

void sound_ui_present(SoundUi *ui) {
    if (ui->dirty &&
        ui->dirty_left < ui->dirty_right &&
        ui->dirty_top < ui->dirty_bottom) {
        SDL_Rect rect = {
            .x = ui->dirty_left,
            .y = ui->dirty_top,
            .w = ui->dirty_right - ui->dirty_left,
            .h = ui->dirty_bottom - ui->dirty_top,
        };
        const uint32_t *pixels = sound_ui_row(ui, rect.y) + rect.x;

        (void)SDL_UpdateTexture(
            ui->texture,
            &rect,
            pixels,
            ui->width * (int)sizeof(uint32_t)
        );
        ui->dirty = false;
    }

    (void)SDL_RenderClear(ui->renderer);
    render_present_texture(ui);
    (void)SDL_RenderPresent(ui->renderer);

    if (!ui->vsync) {
        SDL_Delay(16);
    }
}
