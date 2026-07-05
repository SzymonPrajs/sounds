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

static const char *custom_range_field_name(bool high) {
    return high ? SOUND_UI_CUSTOM_HIGH_FIELD_ID : SOUND_UI_CUSTOM_LOW_FIELD_ID;
}

static char *custom_range_text_buffer(SoundUi *ui, bool high) {
    return high ? ui->custom_high_hz_text : ui->custom_low_hz_text;
}

static void draw_help_line(
    SoundUi *ui,
    const char *text,
    int left,
    int top,
    int scale
) {
    sound_ui_draw_text_scaled(ui, text, left, top, scale, SOUND_UI_AXIS_TEXT_COLOR);
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
            return sound_app_mode_count() + 10;
        case SOUND_UI_MENU_BANDS:
            return sound_frequency_band_count() + 7;
        case SOUND_UI_MENU_COLORS:
            return sound_colormap_count() + 4;
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
    y += line_height;

    y += line_height;
    draw_help_line(
        ui,
        "HELP  M MENU  ESC CLOSE  Q QUIT  SPACE/P PLAY  R REC  TAB/L/V/T/O/B WORKSPACE",
        left,
        y,
        scale
    );
    y += line_height;
    draw_help_line(
        ui,
        "MENU/CLIPS  ARROWS MOVE  ENTER APPLY/LOAD  N RENAME  D DELETE",
        left,
        y,
        scale
    );
    y += line_height;
    draw_help_line(
        ui,
        "TRIM  , . ARROWS / DEL   BAND  [ ] - = EDGES  F METHOD  A HEAR  H HANDLE",
        left,
        y,
        scale
    );
    y += line_height;

    sound_imui_pop_id(&ui->imui);
    return y;
}

static void focus_custom_range_field_in_menu(SoundUi *ui, bool high) {
    sound_ui_reset_custom_range_text(ui, high);
    sound_imui_focus_text_field(
        &ui->imui,
        custom_range_field_name(high),
        custom_range_text_buffer(ui, high)
    );
}

static void reset_custom_range_fields(SoundUi *ui) {
    sound_ui_reset_custom_range_text(ui, false);
    sound_ui_reset_custom_range_text(ui, true);
}

static void draw_custom_range_row(
    SoundUi *ui,
    SoundFrequencyBand frequency_band,
    const char *name,
    const char *label,
    bool high,
    int left,
    int top,
    int width,
    int scale,
    int line_height,
    int cursor_index,
    bool cursor
) {
    const char *unit = " HZ";
    int unit_width = sound_ui_text_width_pixels(unit, scale);
    int field_width = 88 * scale;
    int field_gap = 4 * scale;
    int field_right = left + width - unit_width;
    SoundImuiRect row = menu_row_rect(left, top, width, scale, line_height);
    SoundImuiRect field = sound_imui_rect(
        field_right - field_width,
        top - 2 * scale,
        field_width,
        line_height
    );
    SoundImuiRect label_hit = row;
    char *buffer = custom_range_text_buffer(ui, high);
    uint32_t field_id = sound_imui_id(&ui->imui, custom_range_field_name(high));
    bool field_focused = sound_imui_focus_id(&ui->imui) == field_id;
    bool hovered = false;
    bool active = false;

    label_hit.width = field.x - row.x - field_gap;
    if (label_hit.width < 0) {
        label_hit.width = 0;
    }

    bool fired = sound_imui_hit_rect(&ui->imui, name, label_hit, &hovered, &active);
    if (fired) {
        ui->menu_cursors[SOUND_UI_MENU_BANDS] = cursor_index;
        focus_custom_range_field_in_menu(ui, high);
        field_focused = true;
    } else if (!field_focused) {
        sound_ui_reset_custom_range_text(ui, high);
    }

    if (cursor || hovered || active) {
        sound_ui_fill_rect(
            ui,
            row.x,
            row.y,
            row.width,
            row.height,
            SOUND_UI_MENU_SELECTED_COLOR
        );
    }

    sound_ui_draw_text_scaled(
        ui,
        label,
        left,
        top,
        scale,
        (cursor || field_focused) ? SOUND_UI_MENU_TITLE_COLOR : SOUND_UI_AXIS_TEXT_COLOR
    );

    bool committed = sound_imui_text_field_rect(
        &ui->imui,
        custom_range_field_name(high),
        buffer,
        high ? sizeof(ui->custom_high_hz_text) : sizeof(ui->custom_low_hz_text),
        field
    );

    if (sound_imui_focus_id(&ui->imui) == field_id) {
        ui->menu_cursors[SOUND_UI_MENU_BANDS] = cursor_index;
    }

    if (committed) {
        (void)sound_ui_commit_custom_range_text(
            ui,
            high,
            buffer,
            frequency_band,
            &ui->pending_ui_events
        );
        ui->imui.focus_id = 0U;
        reset_custom_range_fields(ui);
    }

    sound_ui_draw_text_scaled(
        ui,
        unit,
        field.x + field.width,
        top,
        scale,
        SOUND_UI_AXIS_TEXT_COLOR
    );
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

    sound_imui_push_id(&ui->imui, SOUND_UI_CUSTOM_RANGE_ID_SCOPE);
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

    draw_custom_range_row(
        ui,
        frequency_band,
        "bands-custom-low",
        "    custom low",
        false,
        left,
        y,
        width,
        scale,
        line_height,
        band_count,
        cursor == band_count
    );
    y += line_height;

    draw_custom_range_row(
        ui,
        frequency_band,
        "bands-custom-high",
        "    custom high",
        true,
        left,
        y,
        width,
        scale,
        line_height,
        band_count + 1,
        cursor == band_count + 1
    );
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

static int draw_workspace_tabs_line(
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
    return x;
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
    (void)recording_enabled;
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

    bool skip_outside_close = ui->menu_opened_from_toolbar;
    ui->menu_opened_from_toolbar = false;
    if (!skip_outside_close && menu_released_outside_panel(ui, panel)) {
        sound_ui_close_menu(ui);
        sound_imui_end(&ui->imui);
        return;
    }

    sound_ui_dim_screen(ui);
    sound_ui_fill_rect(ui, panel_left, panel_top, panel_width, panel_height, SOUND_UI_MENU_PANEL_COLOR);
    sound_ui_draw_rect_outline(ui, panel_left, panel_top, panel_width, panel_height, scale, SOUND_UI_MENU_BORDER_COLOR);

    sound_ui_draw_text_scaled(ui, "SOUNDS", text_left, y, scale, SOUND_UI_MENU_TITLE_COLOR);
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
    (void)y;

    sound_imui_end(&ui->imui);
}

static int toolbar_button_width(const char *label, int scale) {
    return sound_ui_text_width_pixels(label, scale) + 12 * scale;
}

static SoundImuiRect toolbar_control_rect(
    int left,
    int toolbar_height,
    int width,
    int scale
) {
    int height = (SOUND_UI_GLYPH_HEIGHT + 6) * scale;
    return sound_imui_rect(left, (toolbar_height - height) / 2, width, height);
}

static int toolbar_text_top(SoundImuiRect rect, int scale) {
    return rect.y + (rect.height - SOUND_UI_GLYPH_HEIGHT * scale) / 2;
}

static void draw_centered_toolbar_text(
    SoundUi *ui,
    const char *label,
    SoundImuiRect rect,
    int scale,
    uint32_t color
) {
    int width = sound_ui_text_width_pixels(label, scale);
    int x = rect.x + (rect.width - width) / 2;
    int padding = 6 * scale;

    if (x < rect.x + padding) {
        x = rect.x + padding;
    }

    sound_ui_draw_text_scaled(
        ui,
        label,
        x,
        toolbar_text_top(rect, scale),
        scale,
        color
    );
}

static bool draw_toolbar_button(
    SoundUi *ui,
    const char *name,
    const char *label,
    SoundImuiRect rect,
    int scale,
    bool recording_style
) {
    bool fired = sound_imui_button_rect_id(&ui->imui, name, label, rect);

    if (recording_style) {
        sound_ui_draw_rect_outline(
            ui,
            rect.x,
            rect.y,
            rect.width,
            rect.height,
            1,
            SOUND_UI_MENU_RECORDING_COLOR
        );
        draw_centered_toolbar_text(
            ui,
            label,
            rect,
            scale,
            SOUND_UI_MENU_RECORDING_COLOR
        );
    }

    return fired;
}

static const char *toolbar_mode_name(SoundAppMode mode, bool sst_enabled) {
    const char *text = sound_app_mode_banner(mode, sst_enabled);

    if (text[0] >= '0' && text[0] <= '9' && text[1] == ' ') {
        text += 2;
    }

    return text;
}

static void format_recording_label(
    bool recording_enabled,
    double recording_seconds,
    char *label,
    size_t label_size
) {
    if (!recording_enabled) {
        (void)snprintf(label, label_size, "REC");
        return;
    }

    uint64_t total_seconds = recording_seconds > 0.0 ?
        (uint64_t)recording_seconds :
        0U;
    uint64_t minutes = total_seconds / 60U;
    uint64_t seconds = total_seconds % 60U;

    (void)snprintf(
        label,
        label_size,
        "REC %02llu:%02llu",
        (unsigned long long)minutes,
        (unsigned long long)seconds
    );
}

static void format_toolbar_status(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    bool sst_enabled,
    bool include_band,
    bool include_colormap,
    char *label,
    size_t label_size
) {
    const char *mode_name = toolbar_mode_name(mode, sst_enabled);

    if (include_band && include_colormap) {
        (void)snprintf(
            label,
            label_size,
            "%s  %s  %s",
            mode_name,
            sound_frequency_band_name(frequency_band),
            sound_colormap_name(ui->colormap)
        );
    } else if (include_band) {
        (void)snprintf(
            label,
            label_size,
            "%s  %s",
            mode_name,
            sound_frequency_band_name(frequency_band)
        );
    } else {
        (void)snprintf(label, label_size, "%s", mode_name);
    }
}

static int toolbar_chip_width(const char *label, int scale) {
    return sound_ui_text_width_pixels(label, scale) + 12 * scale;
}

static bool toolbar_status_fits(const char *label, int scale, int width) {
    return toolbar_chip_width(label, scale) <= width;
}

static void draw_toolbar_status_chip(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    bool sst_enabled,
    int left_limit,
    int right,
    SoundImuiRect template_rect,
    int scale
) {
    int available_width = right - left_limit;
    char label[128];

    if (available_width <= 0) {
        return;
    }

    format_toolbar_status(
        ui,
        mode,
        frequency_band,
        sst_enabled,
        true,
        true,
        label,
        sizeof(label)
    );

    if (!toolbar_status_fits(label, scale, available_width)) {
        format_toolbar_status(
            ui,
            mode,
            frequency_band,
            sst_enabled,
            true,
            false,
            label,
            sizeof(label)
        );
    }

    if (!toolbar_status_fits(label, scale, available_width)) {
        format_toolbar_status(
            ui,
            mode,
            frequency_band,
            sst_enabled,
            false,
            false,
            label,
            sizeof(label)
        );
    }

    int width = toolbar_chip_width(label, scale);
    if (width > available_width) {
        return;
    }

    SoundImuiRect rect = template_rect;
    rect.x = right - width;
    rect.width = width;

    bool hovered = false;
    bool active = false;
    if (sound_imui_hit_rect(&ui->imui, "status-chip", rect, &hovered, &active)) {
        sound_ui_open_menu(ui, mode, frequency_band);
        ui->menu_opened_from_toolbar = true;
    }

    sound_ui_draw_text_scaled(
        ui,
        label,
        rect.x + 6 * scale,
        toolbar_text_top(rect, scale),
        scale,
        (hovered || active) ? SOUND_UI_MENU_TITLE_COLOR : SOUND_UI_AXIS_TEXT_COLOR
    );
}

void sound_ui_draw_toolbar(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band,
    SoundWorkspace workspace,
    bool sst_enabled,
    bool recording_enabled,
    double recording_seconds,
    bool playback_enabled
) {
    int scale = ui->text_scale;
    int margin = 6 * scale;
    int gap = 8 * scale;
    int toolbar_height = ui->banner_height;
    int tab_top = (toolbar_height - SOUND_UI_GLYPH_HEIGHT * scale) / 2;
    SoundImuiRect button_rect = toolbar_control_rect(0, toolbar_height, 0, scale);
    char record_label[32];
    const char *play_label = playback_enabled ? "STOP" : "PLAY";

    sound_ui_fill_rect(
        ui,
        0,
        0,
        ui->width,
        toolbar_height,
        SOUND_UI_AXIS_BACKGROUND_COLOR
    );

    begin_non_menu_imui_frame(ui, scale);

    int x = draw_workspace_tabs_line(ui, workspace, margin, tab_top, scale) + gap;

    format_recording_label(
        recording_enabled,
        recording_seconds,
        record_label,
        sizeof(record_label)
    );
    button_rect.x = x;
    button_rect.width = toolbar_button_width(record_label, scale);
    if (draw_toolbar_button(
            ui,
            "record",
            record_label,
            button_rect,
            scale,
            recording_enabled
        )) {
        ui->pending_ui_events.toggle_recording = true;
    }
    x += button_rect.width + gap;

    button_rect.x = x;
    button_rect.width = toolbar_button_width(play_label, scale);
    if (draw_toolbar_button(
            ui,
            "playback",
            play_label,
            button_rect,
            scale,
            false
        )) {
        ui->pending_ui_events.toggle_playback = true;
    }
    x += button_rect.width + gap;

    draw_toolbar_status_chip(
        ui,
        mode,
        frequency_band,
        sst_enabled,
        x,
        ui->width - margin,
        button_rect,
        scale
    );

    sound_imui_end(&ui->imui);
}

void sound_ui_set_title(SoundUi *ui, const SoundUiTitle *title) {
    char text[192];

    (void)snprintf(
        text,
        sizeof(text),
        "Sounds   %s   RMS %.1f dBFS   peak %.1f dBFS   %.0f Hz   %s   band %s   %s   rec %s   play %s   %.1f-%.0f Hz",
        sound_workspace_short_name(title->workspace),
        sound_ui_amplitude_db(title->rms),
        sound_ui_amplitude_db(title->peak),
        title->sample_rate,
        sound_app_mode_title(title->mode, title->sst_enabled),
        sound_frequency_band_name(title->frequency_band),
        sound_colormap_name(title->colormap),
        title->recording_enabled ? "on" : "off",
        title->playback_enabled ? "on" : "off",
        title->min_hz,
        title->max_hz
    );
    (void)SDL_SetWindowTitle(ui->window, text);
}
