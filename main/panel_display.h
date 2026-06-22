#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/display.h"

/* Match the active BSP panel (EK79007 = 1024x600 landscape on the SCINTIX P4). */
#define PANEL_DISPLAY_WIDTH BSP_LCD_H_RES
#define PANEL_DISPLAY_HEIGHT BSP_LCD_V_RES

typedef enum {
    PANEL_ROTATION_0 = 0,
    PANEL_ROTATION_90_CW = 1,
    PANEL_ROTATION_180 = 2,
    PANEL_ROTATION_270_CW = 3,
} panel_rotation_t;

typedef struct {
    panel_rotation_t rotation;
    bool swap_red_blue;
    bool invert_colors;
    bool show_fps;
    uint8_t red_gain;
    uint8_t green_gain;
    uint8_t blue_gain;
} panel_display_settings_t;

void panel_display_set_panel(esp_lcd_panel_handle_t panel);
void panel_display_get_settings(panel_display_settings_t *settings);
esp_err_t panel_display_set_settings(const panel_display_settings_t *settings);
esp_err_t panel_display_load_settings(void);
esp_err_t panel_display_save_settings(const panel_display_settings_t *settings);
esp_err_t panel_display_fill(uint16_t rgb565);
esp_err_t panel_display_draw_doom_rgb565(const uint16_t *src, int src_w, int src_h);
