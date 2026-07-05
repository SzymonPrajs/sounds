#ifndef SOUNDS_UI_INTERNAL_H
#define SOUNDS_UI_INTERNAL_H

#include "sounds/colormap.h"
#include "sounds/ui.h"

#include "imui.h"
#include "imui_sdl.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>

enum {
    SOUND_UI_BACKGROUND_COLOR = 0x05070C,
    SOUND_UI_SEPARATOR_COLOR = 0x10141C,
    SOUND_UI_MIDLINE_COLOR = 0x1B2433,
    SOUND_UI_WAVEFORM_COLOR = 0x95DDFF,
    SOUND_UI_AXIS_BACKGROUND_COLOR = 0x0B0F16,
    SOUND_UI_AXIS_TEXT_COLOR = 0x8FA3BF,
    SOUND_UI_AXIS_TICK_COLOR = 0x415068,
    SOUND_UI_GRIDLINE_COLOR = 0x3E4A66,
    SOUND_UI_MENU_PANEL_COLOR = 0x0A1018,
    SOUND_UI_MENU_BORDER_COLOR = 0x5F7394,
    SOUND_UI_MENU_SELECTED_COLOR = 0x17243A,
    SOUND_UI_MENU_TITLE_COLOR = 0xD7E8FF,
    SOUND_UI_MENU_RECORDING_COLOR = 0xFF8F70,
    SOUND_UI_TAB_ACTIVE_COLOR = 0xD7E8FF,
    SOUND_UI_TAB_INACTIVE_COLOR = 0x6E819C,
    SOUND_UI_MARKER_COLOR = 0xF4F8FF,
    SOUND_UI_MARKER_DIM_COLOR = 0x8497B2,
    SOUND_UI_PLAYHEAD_COLOR = 0xFFF2A8,
    SOUND_UI_RENDER_GREEN_COLOR = 0x97E6B0,
};

static const float sound_ui_floor_db = -95.0F;
static const float sound_ui_ceiling_db = -15.0F;
static const int sound_ui_separator_height = 2;

typedef enum SoundUiMenuTab {
    SOUND_UI_MENU_ANALYSIS,
    SOUND_UI_MENU_BANDS,
    SOUND_UI_MENU_COLORS,
    SOUND_UI_MENU_COUNT,
} SoundUiMenuTab;

#define SOUND_UI_CUSTOM_RANGE_ID_SCOPE "bands"
#define SOUND_UI_CUSTOM_LOW_FIELD_ID "custom-low-field"
#define SOUND_UI_CUSTOM_HIGH_FIELD_ID "custom-high-field"

struct SoundUi {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t *pixels;
    float *bands;
    float *spectrogram_db;
    uint8_t *grid_flags;
    uint8_t *spectrogram_filled;
    SoundColormap colormap;
    SoundImui imui;
    SoundImuiInput imui_input;
    SoundImuiSdlDraw imui_adapter;
    SoundUiEvents pending_ui_events;
    char recording_rename_buffer[SOUND_UI_RECORDING_LABEL_CAPACITY];
    SoundFrequencyBand frequency_band;
    SoundUiMenuTab menu_tab;
    int menu_cursors[SOUND_UI_MENU_COUNT];
    char custom_low_hz_text[32];
    char custom_high_hz_text[32];
    double full_min_hz;
    double full_max_hz;
    double min_hz;
    double max_hz;
    double custom_min_hz;
    double custom_max_hz;
    int width;
    int height;
    int banner_height;
    int waveform_height;
    int spectrogram_top;
    int spectrogram_height;
    int spectrogram_left;
    int spectrogram_origin;
    int text_scale;
    int dirty_left;
    int dirty_top;
    int dirty_right;
    int dirty_bottom;
    uint64_t recording_rename_index;
    bool menu_open;
    bool menu_opened_from_toolbar;
    bool recording_rename_inline_active;
    bool recording_rename_focus_pending;
    bool trim_drag_handle_end;
    bool dirty;
    bool spectrogram_wrap_enabled;
    bool sdl_ready;
    bool vsync;
};

uint32_t *sound_ui_row(SoundUi *ui, int y);
void sound_ui_mark_dirty_rect(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height
);
void sound_ui_mark_dirty_all(SoundUi *ui);
uint32_t sound_ui_pack_color(SoundColor color);
uint32_t sound_ui_color_for_db(const SoundUi *ui, float db);
double sound_ui_amplitude_db(double value);
uint32_t sound_ui_blend_color(uint32_t base, uint32_t over, float amount);
void sound_ui_fill_rows(SoundUi *ui, int from, int rows, uint32_t color);
void sound_ui_draw_text_scaled(
    SoundUi *ui,
    const char *text,
    int x,
    int y,
    int scale,
    uint32_t color
);
void sound_ui_draw_text(SoundUi *ui, const char *text, int x, int y, uint32_t color);
void sound_ui_fill_rect(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height,
    uint32_t color
);
void sound_ui_draw_rect_outline(
    SoundUi *ui,
    int left,
    int top,
    int width,
    int height,
    int thickness,
    uint32_t color
);
void sound_ui_dim_screen(SoundUi *ui);
int sound_ui_spectrogram_row_for_frequency(const SoundUi *ui, double hz);
int sound_ui_spectrogram_row_for_height(
    const SoundUi *ui,
    double hz,
    int height
);
double sound_ui_spectrogram_frequency_for_row_in_height(
    const SoundUi *ui,
    int row,
    int height
);
bool sound_ui_spectrogram_gridline_for_row(
    const SoundUi *ui,
    int row,
    int height
);
void sound_ui_draw_axis(SoundUi *ui);
void sound_ui_draw_axis_in_rect(SoundUi *ui, int top, int height);
int sound_ui_waveform_y(double value, double gain, int rows);
void sound_ui_clear_analysis_pane(SoundUi *ui);
int sound_ui_plot_width(const SoundUi *ui);
void sound_ui_draw_panel_text(
    SoundUi *ui,
    const char *text,
    int line,
    uint32_t color
);
void sound_ui_draw_waveform_in_rect(
    SoundUi *ui,
    const float *samples,
    uint64_t sample_count,
    int left,
    int top,
    int width,
    int height,
    uint32_t color
);
void sound_ui_prepare_resized_buffer(SoundUi *ui);
void sound_ui_open_menu(
    SoundUi *ui,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band
);
void sound_ui_close_menu(SoundUi *ui);
void sound_ui_select_menu_tab(
    SoundUi *ui,
    SoundUiMenuTab tab,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band
);
void sound_ui_switch_menu_tab(
    SoundUi *ui,
    int offset,
    SoundAppMode mode,
    SoundFrequencyBand frequency_band
);
void sound_ui_move_menu_cursor(SoundUi *ui, int offset);
void sound_ui_reset_custom_range_text(SoundUi *ui, bool high);
void sound_ui_focus_custom_range_text_field(SoundUi *ui, bool high);
bool sound_ui_custom_range_text_field_focused(SoundUi *ui);
bool sound_ui_commit_custom_range_text(
    SoundUi *ui,
    bool high,
    const char *text,
    SoundFrequencyBand current_frequency_band,
    SoundUiEvents *events
);
void sound_ui_commit_menu_item(
    SoundUi *ui,
    SoundAppMode current_mode,
    SoundFrequencyBand current_frequency_band,
    SoundWorkspace current_workspace,
    SoundUiEvents *events
);

#endif
