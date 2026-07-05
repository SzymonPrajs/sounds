#include "../internal.h"

#include "sounds/colormap.h"
#include "../font.h"

#include <stdio.h>

static int menu_text_scale(const SoundUi *ui, int line_count) {
    int scale = ui->text_scale;
    int margin = 16 * scale;

    while (scale > 1 &&
        line_count * (SOUND_UI_GLYPH_HEIGHT + 4) * scale + margin * 2 > ui->height) {
        --scale;
        margin = 16 * scale;
    }

    return scale;
}

static void draw_colormap_preview(
    SoundUi *ui,
    SoundColormap colormap,
    int left,
    int top,
    int width,
    int height
) {
    if (width <= 0 || height <= 0) {
        return;
    }

    sound_ui_mark_dirty_rect(ui, left, top, width, height);

    for (int x = 0; x < width; ++x) {
        float unit = width == 1 ? 0.0F : (float)x / (float)(width - 1);
        uint32_t color = sound_ui_pack_color(sound_colormap_sample(colormap, unit));

        for (int y = 0; y < height; ++y) {
            int py = top + y;
            int px = left + x;

            if (px >= 0 && px < ui->width && py >= 0 && py < ui->height) {
                sound_ui_row(ui, py)[px] = color;
            }
        }
    }
}

static void format_frequency_value(double hz, char *text, size_t capacity) {
    if (hz >= 1000.0) {
        (void)snprintf(text, capacity, "%.2g kHz", hz / 1000.0);
    } else if (hz >= 100.0) {
        (void)snprintf(text, capacity, "%.0f Hz", hz);
    } else {
        (void)snprintf(text, capacity, "%.1f Hz", hz);
    }
}

static void format_frequency_range(
    double low_hz,
    double high_hz,
    char *text,
    size_t capacity
) {
    char low[24];
    char high[24];

    format_frequency_value(low_hz, low, sizeof(low));
    format_frequency_value(high_hz, high, sizeof(high));
    (void)snprintf(text, capacity, "%s-%s", low, high);
}

static bool imui_rect_contains(SoundImuiRect rect, int x, int y) {
    return rect.width > 0 &&
        rect.height > 0 &&
        x >= rect.x &&
        y >= rect.y &&
        x < rect.x + rect.width &&
        y < rect.y + rect.height;
}

static SoundImuiRect menu_row_rect(
    int left,
    int top,
    int width,
    int scale,
    int line_height
) {
    return sound_imui_rect(
        left - 3 * scale,
        top - 2 * scale,
        width + 6 * scale,
        line_height
    );
}

static bool menu_released_outside_panel(SoundUi *ui, SoundImuiRect panel) {
    return ui->imui_input.mouse_left_released &&
        !imui_rect_contains(panel, ui->imui_input.mouse_x, ui->imui_input.mouse_y);
}

static bool draw_menu_row(
    SoundUi *ui,
    const char *name,
    const char *text,
    int left,
    int top,
    int width,
    int scale,
    int line_height,
    bool cursor,
    bool active
) {
    SoundImuiRect row = menu_row_rect(left, top, width, scale, line_height);
    bool fired = sound_imui_list_row_id(&ui->imui, name, text, cursor, row);

    if (active && !cursor) {
        sound_ui_draw_text_scaled(ui, text, left, top, scale, SOUND_UI_MENU_TITLE_COLOR);
    }

    return fired;
}

static void draw_menu_tabs(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    int left,
    int top,
    int width,
    int scale,
    int line_height
) {
    int gap = 6 * scale;
    int tab_width = (width - gap * 2) / 3;
    const char *labels[] = {"ANALYSIS", "BANDS", "COLORS"};

    sound_imui_push_id(&ui->imui, "tabs");
    for (int index = 0; index < SOUND_UI_MENU_COUNT; ++index) {
        char name[24];
        int tab_left = left + (tab_width + gap) * index;
        SoundUiMenuTab tab = (SoundUiMenuTab)index;

        (void)snprintf(name, sizeof(name), "tab-%d", index);
        if (draw_menu_row(
                ui,
                name,
                labels[index],
                tab_left,
                top,
                tab_width,
                scale,
                line_height,
                ui->menu_tab == tab,
                ui->menu_tab == tab
            )) {
            sound_ui_select_menu_tab(ui, tab, mode, frequency_band);
        }
    }
    sound_imui_pop_id(&ui->imui);
}

static int menu_line_count(const SoundUi *ui) {
    switch (ui->menu_tab) {
        case SOUND_UI_MENU_ANALYSIS:
            return sound_app_mode_count() + 11;
        case SOUND_UI_MENU_BANDS:
            return sound_frequency_band_count() + 12;
        case SOUND_UI_MENU_COLORS:
            return sound_colormap_count() + 10;
        case SOUND_UI_MENU_COUNT:
            break;
    }

    return 10;
}

static int draw_analysis_menu(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    SoundWorkspace workspace,
    bool sst_enabled,
    int left,
    int top,
    int width,
    int scale,
    int line_height
) {
    int y = top;
    int mode_count = sound_app_mode_count();
    int cursor = ui->menu_cursors[SOUND_UI_MENU_ANALYSIS];

    sound_ui_draw_text_scaled(ui, "ANALYSIS MODES", left, y, scale, SOUND_UI_MENU_TITLE_COLOR);
    y += line_height;

    sound_imui_push_id(&ui->imui, "analysis");
    for (int i = 0; i < mode_count; ++i) {
        SoundAppMode item = sound_app_mode_at(i);
        char line[96];

        (void)snprintf(
            line,
            sizeof(line),
            "%s %d  %s",
            item == mode ? "SET" : "   ",
            i + 1,
            sound_app_mode_title(item, item == SOUND_APP_MODE_TONAL && sst_enabled)
        );
        char name[32];
        (void)snprintf(name, sizeof(name), "analysis-%d", i);
        if (draw_menu_row(
                ui,
                name,
                line,
                left,
                y,
                width,
                scale,
                line_height,
                cursor == i,
                item == mode
            )) {
            ui->menu_cursors[SOUND_UI_MENU_ANALYSIS] = i;
            sound_ui_commit_menu_item(
                ui,
                mode,
                frequency_band,
                workspace,
                &ui->pending_ui_events
            );
        }
        y += line_height;
    }

    y += line_height;
    char tonal_line[96];
    (void)snprintf(
        tonal_line,
        sizeof(tonal_line),
        "SET TONAL VIEW  %s",
        sst_enabled ? "SST" : "RAW CWT"
    );
    if (draw_menu_row(
            ui,
            "analysis-tonal-view",
            tonal_line,
            left,
            y,
            width,
            scale,
            line_height,
            cursor == mode_count,
            true
        )) {
        ui->menu_cursors[SOUND_UI_MENU_ANALYSIS] = mode_count;
        sound_ui_commit_menu_item(
            ui,
            mode,
            frequency_band,
            workspace,
            &ui->pending_ui_events
        );
    }
    sound_imui_pop_id(&ui->imui);
    return y + line_height;
}

static int draw_bands_menu(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    SoundWorkspace workspace,
    int left,
    int top,
    int width,
    int scale,
    int line_height
) {
    int y = top;
    int band_count = sound_frequency_band_count();
    int cursor = ui->menu_cursors[SOUND_UI_MENU_BANDS];

    sound_ui_draw_text_scaled(ui, "FREQUENCY BAND", left, y, scale, SOUND_UI_MENU_TITLE_COLOR);
    y += line_height;

    sound_imui_push_id(&ui->imui, "bands");
    for (int i = 0; i < band_count; ++i) {
        SoundFrequencyBand item = sound_frequency_band_at(i);
        char line[128];
        char custom_range[64];
        const char *range_label = sound_frequency_band_range_label(item);

        if (item == SOUND_FREQUENCY_BAND_CUSTOM) {
            format_frequency_range(
                ui->custom_min_hz,
                ui->custom_max_hz,
                custom_range,
                sizeof(custom_range)
            );
            range_label = custom_range;
        }

        (void)snprintf(
            line,
            sizeof(line),
            "%s %-18s %s",
            item == frequency_band ? "SET" : "   ",
            sound_frequency_band_title(item),
            range_label
        );
        char name[32];
        (void)snprintf(name, sizeof(name), "bands-%d", i);
        if (draw_menu_row(
                ui,
                name,
                line,
                left,
                y,
                width,
                scale,
                line_height,
                cursor == i,
                item == frequency_band
            )) {
            ui->menu_cursors[SOUND_UI_MENU_BANDS] = i;
            sound_ui_commit_menu_item(
                ui,
                mode,
                frequency_band,
                workspace,
                &ui->pending_ui_events
            );
        }
        y += line_height;
    }

    y += line_height;

    char low_value[32];
    char high_value[32];
    format_frequency_value(ui->custom_min_hz, low_value, sizeof(low_value));
    format_frequency_value(ui->custom_max_hz, high_value, sizeof(high_value));

    if (ui->custom_range_editing && !ui->custom_range_edit_high) {
        (void)snprintf(low_value, sizeof(low_value), "%s Hz", ui->custom_range_text);
    }

    if (ui->custom_range_editing && ui->custom_range_edit_high) {
        (void)snprintf(high_value, sizeof(high_value), "%s Hz", ui->custom_range_text);
    }

    char low_line[96];
    char high_line[96];
    (void)snprintf(low_line, sizeof(low_line), "    custom low        %s", low_value);
    (void)snprintf(high_line, sizeof(high_line), "    custom high       %s", high_value);

    if (draw_menu_row(
            ui,
            "bands-custom-low",
            low_line,
            left,
            y,
            width,
            scale,
            line_height,
            cursor == band_count,
            false
        )) {
        ui->menu_cursors[SOUND_UI_MENU_BANDS] = band_count;
        sound_ui_commit_menu_item(
            ui,
            mode,
            frequency_band,
            workspace,
            &ui->pending_ui_events
        );
    }
    y += line_height;

    if (draw_menu_row(
            ui,
            "bands-custom-high",
            high_line,
            left,
            y,
            width,
            scale,
            line_height,
            cursor == band_count + 1,
            false
        )) {
        ui->menu_cursors[SOUND_UI_MENU_BANDS] = band_count + 1;
        sound_ui_commit_menu_item(
            ui,
            mode,
            frequency_band,
            workspace,
            &ui->pending_ui_events
        );
    }
    y += line_height;

    sound_imui_pop_id(&ui->imui);
    return y;
}

static int draw_colors_menu(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    SoundWorkspace workspace,
    int panel_left,
    int panel_width,
    int left,
    int top,
    int width,
    int padding,
    int scale,
    int line_height
) {
    int y = top;
    int colormap_count = sound_colormap_count();
    int cursor = ui->menu_cursors[SOUND_UI_MENU_COLORS];

    sound_ui_draw_text_scaled(ui, "COLORS", left, y, scale, SOUND_UI_MENU_TITLE_COLOR);
    y += line_height;

    sound_imui_push_id(&ui->imui, "colors");
    for (int i = 0; i < colormap_count; ++i) {
        SoundColormap item = sound_colormap_at(i);
        const char *name = sound_colormap_name(item);
        char line[64];
        char row_name[32];
        int preview_width = 76 * scale;
        int preview_height = 5 * scale;
        int preview_left = panel_left + panel_width - padding - preview_width;
        int preview_top = y + scale;

        (void)snprintf(
            line,
            sizeof(line),
            "%s %s",
            item == ui->colormap ? "SET" : "   ",
            name
        );
        (void)snprintf(row_name, sizeof(row_name), "colors-%d", i);
        if (draw_menu_row(
                ui,
                row_name,
                line,
                left,
                y,
                width,
                scale,
                line_height,
                cursor == i,
                item == ui->colormap
            )) {
            ui->menu_cursors[SOUND_UI_MENU_COLORS] = i;
            sound_ui_commit_menu_item(
                ui,
                mode,
                frequency_band,
                workspace,
                &ui->pending_ui_events
            );
        }
        draw_colormap_preview(ui, item, preview_left, preview_top, preview_width, preview_height);
        y += line_height;
    }

    sound_imui_pop_id(&ui->imui);
    return y;
}

static bool workspace_separator_after(SoundWorkspace workspace) {
    return workspace == SOUND_WORKSPACE_LIVE;
}

static void begin_non_menu_imui_frame(SoundUi *ui, int scale) {
    sound_imui_set_draw(
        &ui->imui,
        sound_imui_sdl_draw(&ui->imui_adapter, ui)
    );
    sound_imui_set_text_scale(&ui->imui, scale);
    sound_imui_begin(&ui->imui, ui->menu_open ? NULL : &ui->imui_input);
}

void sound_ui_draw_workspace_tabs_line(
    SoundUi *ui,
    SoundWorkspace workspace,
    int left,
    int top,
    int scale
) {
    int x = left;

    sound_imui_push_id(&ui->imui, "workspace-tabs");
    for (int i = 0; i < sound_workspace_count(); ++i) {
        SoundWorkspace item = sound_workspace_at(i);
        const char *name = sound_workspace_short_name(item);
        char label[32];
        char id[32];
        bool hovered = false;

        (void)snprintf(
            label,
            sizeof(label),
            item == workspace ? "[%s]" : " %s ",
            name
        );

        int label_width = sound_ui_text_width_pixels(label, scale);
        SoundImuiRect rect = sound_imui_rect(
            x,
            top,
            label_width,
            SOUND_UI_GLYPH_HEIGHT * scale
        );

        (void)snprintf(id, sizeof(id), "workspace-%d", i);
        if (sound_imui_hit_rect(&ui->imui, id, rect, &hovered, NULL)) {
            ui->pending_ui_events.workspace = item;
            ui->pending_ui_events.workspace_changed = item != workspace;
        }

        sound_ui_draw_text_scaled(
            ui,
            label,
            x,
            top,
            scale,
            (item == workspace || hovered) ?
                SOUND_UI_TAB_ACTIVE_COLOR :
                SOUND_UI_TAB_INACTIVE_COLOR
        );
        x += label_width + 5 * scale;

        if (workspace_separator_after(item)) {
            sound_ui_fill_rect(
                ui,
                x,
                top,
                scale,
                SOUND_UI_GLYPH_HEIGHT * scale,
                SOUND_UI_SEPARATOR_COLOR
            );
            x += 7 * scale;
        }
    }
    sound_imui_pop_id(&ui->imui);
}

void sound_ui_draw_workbench_tabs(SoundUi *ui, const SoundUiWorkbenchState *state) {
    if (!state) {
        return;
    }

    begin_non_menu_imui_frame(ui, ui->text_scale);
    sound_ui_draw_workspace_tabs_line(
        ui,
        state->workspace,
        6 * ui->text_scale,
        3 * ui->text_scale,
        ui->text_scale
    );
    sound_imui_end(&ui->imui);
}

void sound_ui_draw_menu(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    SoundWorkspace workspace,
    bool sst_enabled,
    bool recording_enabled,
    bool playback_enabled
) {
    (void)playback_enabled;

    if (!ui->menu_open) {
        return;
    }

    int line_count = menu_line_count(ui);
    int scale = menu_text_scale(ui, line_count);
    int line_height = (SOUND_UI_GLYPH_HEIGHT + 4) * scale;
    int padding = 14 * scale;
    int panel_width = ui->width - 24 * scale;

    if (panel_width > 540 * scale) {
        panel_width = 540 * scale;
    }

    int panel_height = line_count * line_height + padding * 2;
    if (panel_height > ui->height - 12 * scale) {
        panel_height = ui->height - 12 * scale;
    }

    int panel_left = (ui->width - panel_width) / 2;
    int panel_top = (ui->height - panel_height) / 2;
    int text_left = panel_left + padding;
    int text_top = panel_top + padding;
    int text_width = panel_width - padding * 2;
    int y = text_top;
    SoundImuiRect panel = sound_imui_rect(
        panel_left,
        panel_top,
        panel_width,
        panel_height
    );

    sound_imui_set_draw(
        &ui->imui,
        sound_imui_sdl_draw(&ui->imui_adapter, ui)
    );
    sound_imui_set_text_scale(&ui->imui, scale);
    sound_imui_begin(&ui->imui, &ui->imui_input);

    if (menu_released_outside_panel(ui, panel)) {
        sound_ui_close_menu(ui);
        sound_imui_end(&ui->imui);
        return;
    }

    sound_ui_dim_screen(ui);
    sound_ui_fill_rect(ui, panel_left, panel_top, panel_width, panel_height, SOUND_UI_MENU_PANEL_COLOR);
    sound_ui_draw_rect_outline(ui, panel_left, panel_top, panel_width, panel_height, scale, SOUND_UI_MENU_BORDER_COLOR);

    sound_ui_draw_text_scaled(ui, "SOUNDS", text_left, y, scale, SOUND_UI_MENU_TITLE_COLOR);
    y += line_height;
    sound_ui_draw_text_scaled(
        ui,
        "ARROWS MOVE  ENTER APPLY  CLICK  M CLOSE",
        text_left,
        y,
        scale,
        SOUND_UI_AXIS_TEXT_COLOR
    );
    y += line_height;

    draw_menu_tabs(ui, mode, frequency_band, text_left, y, text_width, scale, line_height);
    y += line_height * 2;

    switch (ui->menu_tab) {
        case SOUND_UI_MENU_ANALYSIS:
            y = draw_analysis_menu(
                ui,
                mode,
                frequency_band,
                workspace,
                sst_enabled,
                text_left,
                y,
                text_width,
                scale,
                line_height
            );
            break;
        case SOUND_UI_MENU_BANDS:
            y = draw_bands_menu(
                ui,
                mode,
                frequency_band,
                workspace,
                text_left,
                y,
                text_width,
                scale,
                line_height
            );
            break;
        case SOUND_UI_MENU_COLORS:
            y = draw_colors_menu(
                ui,
                mode,
                frequency_band,
                workspace,
                panel_left,
                panel_width,
                text_left,
                y,
                text_width,
                padding,
                scale,
                line_height
            );
            break;
        case SOUND_UI_MENU_COUNT:
            break;
    }

    y += line_height;
    sound_ui_draw_text_scaled(
        ui,
        recording_enabled ? "[R] RECORDING ON" : "[R] RECORDING OFF",
        text_left,
        y,
        scale,
        recording_enabled ? SOUND_UI_MENU_RECORDING_COLOR : SOUND_UI_AXIS_TEXT_COLOR
    );

    sound_imui_end(&ui->imui);
}

void sound_ui_draw_banner(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    SoundWorkspace workspace,
    bool sst_enabled,
    bool recording_enabled,
    double recording_seconds,
    bool playback_enabled
) {
    const char *text = sound_app_mode_banner(mode, sst_enabled);
    const char *mode_text = text;
    char mode_status[96];
    char controls[128];
    char timer[32];
    int scale = ui->text_scale;
    int tab_top = 3 * scale;
    int text_top = tab_top + (SOUND_UI_GLYPH_HEIGHT + 4) * scale;
    int text_left = 6 * scale;

    if (mode_text[0] >= '0' && mode_text[0] <= '9' && mode_text[1] == ' ') {
        mode_text += 2;
    }

    sound_ui_mark_dirty_rect(ui, 0, 0, ui->width, ui->banner_height);

    for (int y = 0; y < ui->banner_height; ++y) {
        uint32_t *row = sound_ui_row(ui, y);

        for (int x = 0; x < ui->width; ++x) {
            row[x] = SOUND_UI_AXIS_BACKGROUND_COLOR;
        }
    }

    (void)snprintf(
        mode_status,
        sizeof(mode_status),
        "MODE [%d] %s  BAND %s",
        sound_app_mode_index(mode) + 1,
        mode_text,
        sound_frequency_band_name(frequency_band)
    );

    begin_non_menu_imui_frame(ui, scale);
    sound_ui_draw_workspace_tabs_line(ui, workspace, text_left, tab_top, scale);
    sound_imui_end(&ui->imui);
    sound_ui_draw_text(ui, mode_status, text_left, text_top, SOUND_UI_AXIS_TEXT_COLOR);

    (void)snprintf(
        controls,
        sizeof(controls),
        "[M] MENU  [P] PLAY %s  [R] REC %s  %s  %s",
        playback_enabled ? "ON" : "OFF",
        recording_enabled ? "ON" : "OFF",
        sound_colormap_name(ui->colormap),
        SOUND_UI_RECORDING_FORMAT_LABEL
    );

    int status_width = sound_ui_text_width_pixels(controls, scale);
    int status_left = ui->width - status_width - 6 * scale;
    int mode_width = sound_ui_text_width_pixels(mode_status, scale);

    if (recording_enabled) {
        uint64_t total_seconds = recording_seconds > 0.0 ?
            (uint64_t)recording_seconds :
            0;
        uint64_t minutes = total_seconds / 60U;
        uint64_t seconds = total_seconds % 60U;

        (void)snprintf(
            timer,
            sizeof(timer),
            "REC %02llu:%02llu",
            (unsigned long long)minutes,
            (unsigned long long)seconds
        );

        int timer_width = sound_ui_text_width_pixels(timer, scale);
        int timer_left = (ui->width - timer_width) / 2;

        if (timer_left > text_left + mode_width + 8 * scale &&
            timer_left + timer_width < status_left - 8 * scale) {
            sound_ui_draw_text(
                ui,
                timer,
                timer_left,
                text_top,
                SOUND_UI_MENU_RECORDING_COLOR
            );
        }
    }

    if (status_left > text_left + mode_width + 8 * scale) {
        sound_ui_draw_text(
            ui,
            controls,
            status_left,
            text_top,
            recording_enabled ? SOUND_UI_MENU_RECORDING_COLOR : SOUND_UI_AXIS_TEXT_COLOR
        );
    }
}

void sound_ui_set_title(SoundUi *ui, const SoundUiTitle *title) {
    char text[192];

    (void)snprintf(
        text,
        sizeof(text),
        "Sounds   %s   RMS %.1f dBFS   peak %.1f dBFS   %.0f Hz   %s   band %s   %s   %s   rec %s   play %s   %.1f-%.0f Hz",
        sound_workspace_short_name(title->workspace),
        sound_ui_amplitude_db(title->rms),
        sound_ui_amplitude_db(title->peak),
        title->sample_rate,
        sound_app_mode_title(title->mode, title->sst_enabled),
        sound_frequency_band_name(title->frequency_band),
        sound_colormap_name(title->colormap),
        SOUND_UI_RECORDING_FORMAT_LABEL,
        title->recording_enabled ? "on" : "off",
        title->playback_enabled ? "on" : "off",
        title->min_hz,
        title->max_hz
    );
    (void)SDL_SetWindowTitle(ui->window, text);
}
