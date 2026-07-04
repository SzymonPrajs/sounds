#include "internal.h"

#include "sounds/colormap.h"
#include "font.h"

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

static void draw_menu_line(
    SoundUi *ui,
    const char *text,
    int left,
    int top,
    int width,
    int scale,
    bool selected
) {
    if (selected) {
        sound_ui_fill_rect(
            ui,
            left - 3 * scale,
            top - 2 * scale,
            width + 6 * scale,
            (SOUND_UI_GLYPH_HEIGHT + 4) * scale,
            SOUND_UI_MENU_SELECTED_COLOR
        );
    }

    sound_ui_draw_text_scaled(
        ui,
        text,
        left,
        top,
        scale,
        selected ? SOUND_UI_MENU_TITLE_COLOR : SOUND_UI_AXIS_TEXT_COLOR
    );
}

static void draw_menu_tabs(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int scale
) {
    int gap = 6 * scale;
    int tab_width = (width - gap) / 2;

    draw_menu_line(
        ui,
        "[1-8] ANALYSIS",
        left,
        top,
        tab_width,
        scale,
        ui->menu_tab == SOUND_UI_MENU_ANALYSIS
    );
    draw_menu_line(
        ui,
        "[TAB] COLORS",
        left + tab_width + gap,
        top,
        tab_width,
        scale,
        ui->menu_tab == SOUND_UI_MENU_SETTINGS
    );
}

static int menu_line_count(const SoundUi *ui) {
    if (ui->menu_tab == SOUND_UI_MENU_ANALYSIS) {
        return sound_app_mode_count() + 10;
    }

    return sound_colormap_count() + 9;
}

static int draw_analysis_menu(
    SoundUi *ui,
    SoundAppMode mode,
    bool sst_enabled,
    int left,
    int top,
    int width,
    int scale,
    int line_height
) {
    int y = top;
    int mode_count = sound_app_mode_count();

    sound_ui_draw_text_scaled(ui, "ANALYSIS MODES", left, y, scale, SOUND_UI_MENU_TITLE_COLOR);
    y += line_height;

    for (int i = 0; i < mode_count; ++i) {
        SoundAppMode item = sound_app_mode_at(i);
        char line[96];

        (void)snprintf(
            line,
            sizeof(line),
            "%d %s",
            i + 1,
            sound_app_mode_title(item, item == SOUND_APP_MODE_TONAL && sst_enabled)
        );
        draw_menu_line(ui, line, left, y, width, scale, item == mode);
        y += line_height;
    }

    y += line_height;
    sound_ui_draw_text_scaled(ui, "[1-8] SWITCH MODE", left, y, scale, SOUND_UI_AXIS_TEXT_COLOR);
    y += line_height;
    sound_ui_draw_text_scaled(ui, "[T] TONAL SST  [TAB] COLORS", left, y, scale, SOUND_UI_AXIS_TEXT_COLOR);
    return y + line_height;
}

static int draw_settings_menu(
    SoundUi *ui,
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

    sound_ui_draw_text_scaled(ui, "COLORS", left, y, scale, SOUND_UI_MENU_TITLE_COLOR);
    y += line_height;

    for (int i = 0; i < colormap_count; ++i) {
        SoundColormap item = sound_colormap_at(i);
        const char *name = sound_colormap_name(item);
        int preview_width = 76 * scale;
        int preview_height = 5 * scale;
        int preview_left = panel_left + panel_width - padding - preview_width;
        int preview_top = y + scale;

        draw_menu_line(ui, name, left, y, width, scale, item == ui->colormap);
        draw_colormap_preview(ui, item, preview_left, preview_top, preview_width, preview_height);
        y += line_height;
    }

    y += line_height;
    sound_ui_draw_text_scaled(ui, "[C]/ARROWS COLOR", left, y, scale, SOUND_UI_AXIS_TEXT_COLOR);
    return y + line_height;
}

static bool workspace_separator_after(SoundWorkspace workspace) {
    return workspace == SOUND_WORKSPACE_LIVE;
}

void sound_ui_draw_workspace_tabs_line(
    SoundUi *ui,
    SoundWorkspace workspace,
    int left,
    int top,
    int scale
) {
    int x = left;
    for (int i = 0; i < sound_workspace_count(); ++i) {
        SoundWorkspace item = sound_workspace_at(i);
        const char *name = sound_workspace_short_name(item);
        char label[32];

        (void)snprintf(
            label,
            sizeof(label),
            item == workspace ? "[%s]" : " %s ",
            name
        );

        sound_ui_draw_text_scaled(
            ui,
            label,
            x,
            top,
            scale,
            item == workspace ? SOUND_UI_TAB_ACTIVE_COLOR : SOUND_UI_TAB_INACTIVE_COLOR
        );
        x += sound_ui_text_width_pixels(label, scale) + 5 * scale;

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
}

void sound_ui_draw_workbench_tabs(SoundUi *ui, const SoundUiWorkbenchState *state) {
    if (!state) {
        return;
    }

    sound_ui_draw_workspace_tabs_line(
        ui,
        state->workspace,
        6 * ui->text_scale,
        3 * ui->text_scale,
        ui->text_scale
    );
}

void sound_ui_draw_menu(
    SoundUi *ui,
    SoundAppMode mode,
    SoundWorkspace workspace,
    bool sst_enabled,
    bool recording_enabled,
    bool playback_enabled
) {
    (void)workspace;
    (void)playback_enabled;

    if (!ui->menu_open) {
        return;
    }

    int line_count = menu_line_count(ui);
    int scale = menu_text_scale(ui, line_count);
    int line_height = (SOUND_UI_GLYPH_HEIGHT + 4) * scale;
    int padding = 14 * scale;
    int panel_width = ui->width - 24 * scale;

    if (panel_width > 430 * scale) {
        panel_width = 430 * scale;
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

    sound_ui_dim_screen(ui);
    sound_ui_fill_rect(ui, panel_left, panel_top, panel_width, panel_height, SOUND_UI_MENU_PANEL_COLOR);
    sound_ui_draw_rect_outline(ui, panel_left, panel_top, panel_width, panel_height, scale, SOUND_UI_MENU_BORDER_COLOR);

    sound_ui_draw_text_scaled(ui, "SOUNDS", text_left, y, scale, SOUND_UI_MENU_TITLE_COLOR);
    y += line_height;
    sound_ui_draw_text_scaled(ui, "[TAB] SWITCH  [M] CLOSE  [Q] QUIT", text_left, y, scale, SOUND_UI_AXIS_TEXT_COLOR);
    y += line_height;

    draw_menu_tabs(ui, text_left, y, text_width, scale);
    y += line_height * 2;

    if (ui->menu_tab == SOUND_UI_MENU_ANALYSIS) {
        y = draw_analysis_menu(
            ui,
            mode,
            sst_enabled,
            text_left,
            y,
            text_width,
            scale,
            line_height
        );
    } else {
        y = draw_settings_menu(
            ui,
            panel_left,
            panel_width,
            text_left,
            y,
            text_width,
            padding,
            scale,
            line_height
        );
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
}

void sound_ui_draw_banner(
    SoundUi *ui,
    SoundAppMode mode,
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
        "MODE [%d] %s  [1-8] SWITCH",
        sound_app_mode_index(mode) + 1,
        mode_text
    );

    sound_ui_draw_workspace_tabs_line(ui, workspace, text_left, tab_top, scale);
    sound_ui_draw_text(ui, mode_status, text_left, text_top, SOUND_UI_AXIS_TEXT_COLOR);

    (void)snprintf(
        controls,
        sizeof(controls),
        "[M] MENU  [P] PLAY %s  [R] REC %s  [C] %s  %s",
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
        "Sounds   %s   RMS %.1f dBFS   peak %.1f dBFS   %.0f Hz   %s   %s   %s   rec %s   play %s   %.1f-%.0f Hz",
        sound_workspace_short_name(title->workspace),
        sound_ui_amplitude_db(title->rms),
        sound_ui_amplitude_db(title->peak),
        title->sample_rate,
        sound_app_mode_title(title->mode, title->sst_enabled),
        sound_colormap_name(title->colormap),
        SOUND_UI_RECORDING_FORMAT_LABEL,
        title->recording_enabled ? "on" : "off",
        title->playback_enabled ? "on" : "off",
        title->min_hz,
        title->max_hz
    );
    (void)SDL_SetWindowTitle(ui->window, text);
}
