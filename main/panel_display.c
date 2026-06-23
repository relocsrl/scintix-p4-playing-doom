#include "panel_display.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#define PANEL_SETTINGS_NAMESPACE "display"
#define PANEL_SETTINGS_KEY_ROTATION "rotation"
#define PANEL_SETTINGS_KEY_SWAP_RB "swap_rb"
#define PANEL_SETTINGS_KEY_INVERT "invert"
#define PANEL_SETTINGS_KEY_SHOW_FPS "show_fps"
#define PANEL_SETTINGS_KEY_RED_GAIN "red_gain"
#define PANEL_SETTINGS_KEY_GREEN_GAIN "green_gain"
#define PANEL_SETTINGS_KEY_BLUE_GAIN "blue_gain"

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_panel_lock;
static portMUX_TYPE s_settings_lock = portMUX_INITIALIZER_UNLOCKED;

/* PPA (Pixel Processing Accelerator) fast path. When the colour settings are
 * neutral and rotation is 0/180, the scale+rotate+framebuffer-write that the
 * CPU otherwise does is offloaded to the PPA SRM engine, which writes straight
 * into a DPI frame buffer. With two frame buffers we render into the back one
 * and page-flip; with one we render into the live buffer. */
#define DPI_MAX_FBS 2
static ppa_client_handle_t s_ppa_client;
static bool s_ppa_init_done;     /* init attempted (success or not) */
static bool s_ppa_available;     /* fast path usable */
static void *s_dpi_fbs[DPI_MAX_FBS];
static int s_dpi_num_fbs;
static int s_dpi_render_idx;     /* frame buffer the next frame renders into */
#define DOOM_LINE_BUFFERS 3
static uint16_t *s_doom_lines[DOOM_LINE_BUFFERS];
static size_t s_doom_lines_pixels;
static size_t s_doom_line_index;

/* Nearest-neighbour scaling maps: s_scale_lx[x] / s_scale_ly[y] give the source
 * (logical) coordinate for each destination column/row, so the hot loop avoids a
 * per-pixel integer divide. Rebuilt only when the geometry changes. */
static uint16_t *s_scale_lx;
static uint16_t *s_scale_ly;
static int s_scale_logical_w;
static int s_scale_logical_h;
static int s_scale_dst_w;
static int s_scale_dst_h;
static panel_display_settings_t s_settings = {
    .rotation = PANEL_ROTATION_0,
    .swap_red_blue = false,
    .invert_colors = false,
    .show_fps = false,
    .red_gain = 100,
    .green_gain = 100,
    .blue_gain = 100,
};
static int64_t s_fps_window_start_us;
static uint32_t s_fps_window_frames;
static uint32_t s_fps_value_x10;

static uint8_t clamp_gain(uint8_t gain)
{
    if (gain > 200) {
        return 200;
    }
    return gain;
}

static void sanitize_settings(panel_display_settings_t *settings)
{
    if (settings->rotation != PANEL_ROTATION_0 && settings->rotation != PANEL_ROTATION_180) {
        settings->rotation = PANEL_ROTATION_0;
    }
    settings->red_gain = clamp_gain(settings->red_gain);
    settings->green_gain = clamp_gain(settings->green_gain);
    settings->blue_gain = clamp_gain(settings->blue_gain);
    if (settings->red_gain == 0) {
        settings->red_gain = 100;
    }
    if (settings->green_gain == 0) {
        settings->green_gain = 100;
    }
    if (settings->blue_gain == 0) {
        settings->blue_gain = 100;
    }
}

static uint8_t apply_gain(uint8_t value, uint8_t gain)
{
    uint16_t adjusted = ((uint16_t)value * gain) / 100;
    return adjusted > 255 ? 255 : (uint8_t)adjusted;
}

static uint16_t apply_color_settings(uint16_t color, const panel_display_settings_t *settings)
{
    if (!settings->swap_red_blue && !settings->invert_colors &&
        settings->red_gain == 100 && settings->green_gain == 100 && settings->blue_gain == 100) {
        return color;
    }

    uint8_t r = (uint8_t)(((color >> 11) & 0x1f) * 255 / 31);
    uint8_t g = (uint8_t)(((color >> 5) & 0x3f) * 255 / 63);
    uint8_t b = (uint8_t)((color & 0x1f) * 255 / 31);

    if (settings->swap_red_blue) {
        uint8_t tmp = r;
        r = b;
        b = tmp;
    }

    r = apply_gain(r, settings->red_gain);
    g = apply_gain(g, settings->green_gain);
    b = apply_gain(b, settings->blue_gain);

    if (settings->invert_colors) {
        r = 255 - r;
        g = 255 - g;
        b = 255 - b;
    }

    return (uint16_t)(((uint16_t)(r & 0xf8) << 8) |
                      ((uint16_t)(g & 0xfc) << 3) |
                      (b >> 3));
}

static bool color_settings_are_neutral(const panel_display_settings_t *settings)
{
    return !settings->swap_red_blue && !settings->invert_colors &&
           settings->red_gain == 100 && settings->green_gain == 100 && settings->blue_gain == 100;
}

static const uint8_t *fps_glyph(char c)
{
    static const uint8_t digits[10][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7},
        {0x2, 0x6, 0x2, 0x2, 0x7},
        {0x7, 0x1, 0x7, 0x4, 0x7},
        {0x7, 0x1, 0x7, 0x1, 0x7},
        {0x5, 0x5, 0x7, 0x1, 0x1},
        {0x7, 0x4, 0x7, 0x1, 0x7},
        {0x7, 0x4, 0x7, 0x5, 0x7},
        {0x7, 0x1, 0x1, 0x2, 0x2},
        {0x7, 0x5, 0x7, 0x5, 0x7},
        {0x7, 0x5, 0x7, 0x1, 0x7},
    };
    static const uint8_t glyph_f[5] = {0x7, 0x4, 0x6, 0x4, 0x4};
    static const uint8_t glyph_p[5] = {0x6, 0x5, 0x6, 0x4, 0x4};
    static const uint8_t glyph_s[5] = {0x7, 0x4, 0x7, 0x1, 0x7};
    static const uint8_t glyph_dash[5] = {0x0, 0x0, 0x7, 0x0, 0x0};
    static const uint8_t glyph_dot[5] = {0x0, 0x0, 0x0, 0x0, 0x2};
    static const uint8_t glyph_blank[5] = {0x0, 0x0, 0x0, 0x0, 0x0};

    if (c >= '0' && c <= '9') {
        return digits[c - '0'];
    }
    switch (c) {
    case 'F':
        return glyph_f;
    case 'P':
        return glyph_p;
    case 'S':
        return glyph_s;
    case '-':
        return glyph_dash;
    case '.':
        return glyph_dot;
    default:
        return glyph_blank;
    }
}

static void draw_fps_rect(uint16_t *lines, int chunk_y0, int chunk_h, int dst_w,
                          int x0, int y0, int w, int h, uint16_t color)
{
    int y_start = y0 > chunk_y0 ? y0 : chunk_y0;
    int y_end = (y0 + h) < (chunk_y0 + chunk_h) ? (y0 + h) : (chunk_y0 + chunk_h);
    if (y_start >= y_end || x0 >= dst_w) {
        return;
    }

    int x_start = x0 < 0 ? 0 : x0;
    int x_end = (x0 + w) < dst_w ? (x0 + w) : dst_w;
    if (x_start >= x_end) {
        return;
    }

    for (int y = y_start; y < y_end; y++) {
        uint16_t *row = lines + (y - chunk_y0) * dst_w;
        for (int x = x_start; x < x_end; x++) {
            row[x] = color;
        }
    }
}

static void draw_fps_char(uint16_t *lines, int chunk_y0, int chunk_h, int dst_w,
                          int x0, int y0, char c, int scale, uint16_t color)
{
    const uint8_t *glyph = fps_glyph(c);

    for (int gy = 0; gy < 5; gy++) {
        for (int gx = 0; gx < 3; gx++) {
            if ((glyph[gy] & (1 << (2 - gx))) == 0) {
                continue;
            }
            draw_fps_rect(lines, chunk_y0, chunk_h, dst_w,
                          x0 + gx * scale, y0 + gy * scale, scale, scale, color);
        }
    }
}

static void draw_fps_overlay(uint16_t *lines, int chunk_y0, int chunk_h, int dst_w, uint32_t fps_x10)
{
    const int scale = 4;
    const int char_w = 3 * scale;
    const int char_h = 5 * scale;
    const int spacing = scale;
    const int pad = 4;
    const int x0 = 8;
    const int y0 = 8;
    const int overlay_y0 = y0 - pad;
    const int overlay_y1 = y0 + char_h + pad;

    if (chunk_y0 >= overlay_y1 || chunk_y0 + chunk_h <= overlay_y0) {
        return;
    }

    char text[16];
    if (fps_x10 == 0) {
        strlcpy(text, "FPS --.-", sizeof(text));
    } else {
        snprintf(text, sizeof(text), "FPS %lu.%lu",
                 (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10));
    }

    const int len = (int)strlen(text);
    const int text_w = len > 0 ? len * char_w + (len - 1) * spacing : 0;

    draw_fps_rect(lines, chunk_y0, chunk_h, dst_w,
                  x0 - pad, y0 - pad, text_w + pad * 2, char_h + pad * 2, 0x0000);
    for (int i = 0; i < len; i++) {
        draw_fps_char(lines, chunk_y0, chunk_h, dst_w,
                      x0 + i * (char_w + spacing), y0, text[i], scale, 0xffff);
    }
}

static void update_fps_counter(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_fps_window_start_us == 0) {
        s_fps_window_start_us = now_us;
    }

    s_fps_window_frames++;
    int64_t elapsed_us = now_us - s_fps_window_start_us;
    if (elapsed_us >= 500000) {
        s_fps_value_x10 = (uint32_t)((s_fps_window_frames * 10000000ULL + (uint64_t)elapsed_us / 2) /
                                     (uint64_t)elapsed_us);
        s_fps_window_start_us = now_us;
        s_fps_window_frames = 0;
    }
}

void panel_display_set_panel(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    if (s_panel_lock == NULL) {
        s_panel_lock = xSemaphoreCreateMutex();
    }
}

void panel_display_get_settings(panel_display_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_settings_lock);
    *settings = s_settings;
    taskEXIT_CRITICAL(&s_settings_lock);
}

esp_err_t panel_display_set_settings(const panel_display_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    panel_display_settings_t sanitized = *settings;
    sanitize_settings(&sanitized);

    taskENTER_CRITICAL(&s_settings_lock);
    s_settings = sanitized;
    taskEXIT_CRITICAL(&s_settings_lock);
    return ESP_OK;
}

esp_err_t panel_display_load_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PANEL_SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    panel_display_settings_t settings = s_settings;
    uint8_t value = 0;
    if (nvs_get_u8(nvs, PANEL_SETTINGS_KEY_ROTATION, &value) == ESP_OK) {
        settings.rotation = (panel_rotation_t)value;
    }
    if (nvs_get_u8(nvs, PANEL_SETTINGS_KEY_SWAP_RB, &value) == ESP_OK) {
        settings.swap_red_blue = value != 0;
    }
    if (nvs_get_u8(nvs, PANEL_SETTINGS_KEY_INVERT, &value) == ESP_OK) {
        settings.invert_colors = value != 0;
    }
    if (nvs_get_u8(nvs, PANEL_SETTINGS_KEY_SHOW_FPS, &value) == ESP_OK) {
        settings.show_fps = value != 0;
    }
    if (nvs_get_u8(nvs, PANEL_SETTINGS_KEY_RED_GAIN, &value) == ESP_OK) {
        settings.red_gain = value;
    }
    if (nvs_get_u8(nvs, PANEL_SETTINGS_KEY_GREEN_GAIN, &value) == ESP_OK) {
        settings.green_gain = value;
    }
    if (nvs_get_u8(nvs, PANEL_SETTINGS_KEY_BLUE_GAIN, &value) == ESP_OK) {
        settings.blue_gain = value;
    }
    nvs_close(nvs);
    return panel_display_set_settings(&settings);
}

esp_err_t panel_display_save_settings(const panel_display_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    panel_display_settings_t sanitized = *settings;
    sanitize_settings(&sanitized);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PANEL_SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = nvs_set_u8(nvs, PANEL_SETTINGS_KEY_ROTATION, (uint8_t)sanitized.rotation)) == ESP_OK &&
        (err = nvs_set_u8(nvs, PANEL_SETTINGS_KEY_SWAP_RB, sanitized.swap_red_blue ? 1 : 0)) == ESP_OK &&
        (err = nvs_set_u8(nvs, PANEL_SETTINGS_KEY_INVERT, sanitized.invert_colors ? 1 : 0)) == ESP_OK &&
        (err = nvs_set_u8(nvs, PANEL_SETTINGS_KEY_SHOW_FPS, sanitized.show_fps ? 1 : 0)) == ESP_OK &&
        (err = nvs_set_u8(nvs, PANEL_SETTINGS_KEY_RED_GAIN, sanitized.red_gain)) == ESP_OK &&
        (err = nvs_set_u8(nvs, PANEL_SETTINGS_KEY_GREEN_GAIN, sanitized.green_gain)) == ESP_OK &&
        (err = nvs_set_u8(nvs, PANEL_SETTINGS_KEY_BLUE_GAIN, sanitized.blue_gain)) == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        err = panel_display_set_settings(&sanitized);
    }
    return err;
}

esp_err_t panel_display_fill(uint16_t rgb565)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int lines_per_chunk = 40;
    uint16_t *lines = heap_caps_malloc(PANEL_DISPLAY_WIDTH * lines_per_chunk * sizeof(uint16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (lines == NULL) {
        lines = heap_caps_malloc(PANEL_DISPLAY_WIDTH * lines_per_chunk * sizeof(uint16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (lines == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < PANEL_DISPLAY_WIDTH * lines_per_chunk; i++) {
        lines[i] = rgb565;
    }

    if (s_panel_lock != NULL) {
        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    }
    for (int y = 0; y < PANEL_DISPLAY_HEIGHT; y += lines_per_chunk) {
        int h = PANEL_DISPLAY_HEIGHT - y;
        if (h > lines_per_chunk) {
            h = lines_per_chunk;
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, PANEL_DISPLAY_WIDTH, y + h, lines);
        if (err != ESP_OK) {
            if (s_panel_lock != NULL) {
                xSemaphoreGive(s_panel_lock);
            }
            free(lines);
            return err;
        }
    }
    if (s_panel_lock != NULL) {
        xSemaphoreGive(s_panel_lock);
    }

    free(lines);
    return ESP_OK;
}

/* (Re)build the nearest-neighbour scaling maps. Returns false on allocation
 * failure, in which case the caller falls back to the per-pixel divide path. */
static bool ensure_scale_maps(int dst_w, int dst_h, int logical_w, int logical_h)
{
    if (s_scale_lx != NULL && s_scale_ly != NULL &&
        s_scale_dst_w == dst_w && s_scale_dst_h == dst_h &&
        s_scale_logical_w == logical_w && s_scale_logical_h == logical_h) {
        return true;
    }

    free(s_scale_lx);
    free(s_scale_ly);
    s_scale_lx = heap_caps_malloc((size_t)dst_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_scale_ly = heap_caps_malloc((size_t)dst_h * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_scale_lx == NULL || s_scale_ly == NULL) {
        free(s_scale_lx);
        free(s_scale_ly);
        s_scale_lx = NULL;
        s_scale_ly = NULL;
        s_scale_dst_w = 0;
        return false;
    }

    for (int x = 0; x < dst_w; x++) {
        s_scale_lx[x] = (uint16_t)(((int)x * logical_w) / dst_w);
    }
    for (int y = 0; y < dst_h; y++) {
        s_scale_ly[y] = (uint16_t)(((int)y * logical_h) / dst_h);
    }
    s_scale_dst_w = dst_w;
    s_scale_dst_h = dst_h;
    s_scale_logical_w = logical_w;
    s_scale_logical_h = logical_h;
    return true;
}

/* Register the PPA SRM client and grab the DPI frame buffer pointer(s). Runs
 * once, lazily, on the first draw. On any failure the fast path stays off and
 * the CPU scaler is used. */
static void ppa_lazy_init(void)
{
    if (s_ppa_init_done) {
        return;
    }
    s_ppa_init_done = true;
    if (s_panel == NULL) {
        return;
    }

    /* The driver allocates CONFIG_BSP_LCD_DPI_BUFFER_NUMS frame buffers; probe
     * downwards from the most we support until get_frame_buffer accepts the
     * count (it rejects counts above num_fbs). */
    for (int n = DPI_MAX_FBS; n >= 1; n--) {
        esp_err_t err;
        if (n == 2) {
            err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &s_dpi_fbs[0], &s_dpi_fbs[1]);
        } else {
            err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &s_dpi_fbs[0]);
        }
        if (err == ESP_OK) {
            s_dpi_num_fbs = n;
            break;
        }
    }
    if (s_dpi_num_fbs == 0) {
        ESP_LOGW("panel_perf", "PPA: no DPI frame buffer, using CPU scaler");
        return;
    }

    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&cfg, &s_ppa_client) != ESP_OK) {
        ESP_LOGW("panel_perf", "PPA: register client failed, using CPU scaler");
        return;
    }

    /* The scale factor quantizes to 3.1875x (1020 px), so PPA never writes the
     * rightmost ~4 columns, nor any letterbox rows. Clear every frame buffer to
     * black once now so those untouched regions don't show stale boot/test-
     * pattern data (which, differing between the two buffers, flickered as we
     * page-flipped). PPA never overwrites them again, so this stays black. */
    const size_t fb_size = (size_t)PANEL_DISPLAY_WIDTH * PANEL_DISPLAY_HEIGHT * sizeof(uint16_t);
    for (int i = 0; i < s_dpi_num_fbs; i++) {
        memset(s_dpi_fbs[i], 0, fb_size);
        esp_cache_msync(s_dpi_fbs[i], fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    /* fb0 is the buffer the driver scans out first, so begin rendering into the
     * other one (if any) to avoid drawing over the live frame. */
    s_dpi_render_idx = (s_dpi_num_fbs > 1) ? 1 : 0;
    s_ppa_available = true;
    ESP_LOGI("panel_perf", "PPA ready: %d DPI frame buffer(s)", s_dpi_num_fbs);
}

/* Hardware scale (and optional 180° rotate) of the Doom frame straight into a
 * DPI frame buffer, then page-flip. Caller must hold s_panel_lock and must have
 * checked s_ppa_available + neutral colours + rotation 0/180. dst_y0 letterbox
 * rows are left untouched (the buffers start black and PPA never writes them).
 * Returns ESP_OK on success; on error the caller falls back to the CPU path. */
static esp_err_t ppa_draw_doom(const uint16_t *src, int src_w, int src_h,
                               const panel_display_settings_t *settings,
                               int dst_w, int dst_h, int dst_y0)
{
    void *fb = s_dpi_fbs[s_dpi_render_idx];
    const size_t fb_size = (size_t)PANEL_DISPLAY_WIDTH * PANEL_DISPLAY_HEIGHT * sizeof(uint16_t);

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src,
            .pic_w = (uint32_t)src_w,
            .pic_h = (uint32_t)src_h,
            .block_w = (uint32_t)src_w,
            .block_h = (uint32_t)src_h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = fb,
            .buffer_size = fb_size,
            .pic_w = PANEL_DISPLAY_WIDTH,
            .pic_h = PANEL_DISPLAY_HEIGHT,
            .block_offset_y = (uint32_t)dst_y0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = (settings->rotation == PANEL_ROTATION_180)
                              ? PPA_SRM_ROTATION_ANGLE_180
                              : PPA_SRM_ROTATION_ANGLE_0,
        /* Scale factor is quantized to 1/16 in hardware, so e.g. 1024/320=3.2
         * actually renders at 3.1875 (1020 px wide); the few unwritten edge
         * columns stay black. */
        .scale_x = (float)dst_w / (float)src_w,
        .scale_y = (float)dst_h / (float)src_h,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_client, &op);
    if (err != ESP_OK) {
        return err;
    }

    if (settings->show_fps) {
        /* Draw the overlay straight into the frame buffer (full-frame addressing
         * via chunk_y0=0). The draw_bitmap below writes it back from cache. */
        draw_fps_overlay((uint16_t *)fb, 0, PANEL_DISPLAY_HEIGHT, PANEL_DISPLAY_WIDTH, s_fps_value_x10);
    }

    /* Hand the buffer back to the panel. Because it lies inside one of the
     * driver's own frame buffers, draw_bitmap does no copy: it just writes back
     * the (overlay) cache lines and switches the scanout to this buffer at the
     * next frame boundary. */
    err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_DISPLAY_WIDTH, PANEL_DISPLAY_HEIGHT, fb);
    if (err != ESP_OK) {
        return err;
    }

    if (s_dpi_num_fbs > 1) {
        s_dpi_render_idx = (s_dpi_render_idx + 1) % s_dpi_num_fbs;
    }
    return ESP_OK;
}

esp_err_t panel_display_draw_doom_rgb565(const uint16_t *src, int src_w, int src_h)
{
    if (s_panel == NULL || src == NULL || src_w <= 0 || src_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int dst_w = PANEL_DISPLAY_WIDTH;
    const int dst_screen_h = PANEL_DISPLAY_HEIGHT;
    panel_display_settings_t settings;
    panel_display_get_settings(&settings);
    if (settings.show_fps) {
        update_fps_counter();
    } else {
        s_fps_window_start_us = 0;
        s_fps_window_frames = 0;
        s_fps_value_x10 = 0;
    }

    const bool rotated = settings.rotation == PANEL_ROTATION_90_CW || settings.rotation == PANEL_ROTATION_270_CW;
    const int logical_w = rotated ? src_h : src_w;
    const int logical_h = rotated ? src_w : src_h;
    int dst_h = (PANEL_DISPLAY_WIDTH * logical_h) / logical_w;
    if (dst_h > PANEL_DISPLAY_HEIGHT) {
        dst_h = PANEL_DISPLAY_HEIGHT;
    }
    const int dst_y0 = (dst_screen_h - dst_h) / 2;

    /* PPA hardware fast path: neutral colours and rotation 0/180 only. Anything
     * else (colour adjust, 90/270) falls through to the CPU scaler below. */
    ppa_lazy_init();
    if (s_ppa_available && color_settings_are_neutral(&settings) &&
        (settings.rotation == PANEL_ROTATION_0 || settings.rotation == PANEL_ROTATION_180)) {
        const int64_t ppa_t0 = esp_timer_get_time(); /* PERF */
        esp_err_t perr;
        if (s_panel_lock != NULL) {
            xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        }
        perr = ppa_draw_doom(src, src_w, src_h, &settings, dst_w, dst_h, dst_y0);
        if (s_panel_lock != NULL) {
            xSemaphoreGive(s_panel_lock);
        }
        if (perr == ESP_OK) {
            /* PERF: average PPA scale+flip time, compared against on-screen FPS. */
            static int64_t s_ppa_us_accum;
            static uint32_t s_ppa_us_frames;
            s_ppa_us_accum += esp_timer_get_time() - ppa_t0;
            if (++s_ppa_us_frames >= 64) {
                ESP_LOGI("panel_perf", "ppa scale+flip %lld us/frame",
                         (long long)(s_ppa_us_accum / s_ppa_us_frames));
                s_ppa_us_accum = 0;
                s_ppa_us_frames = 0;
            }
            return ESP_OK;
        }
        ESP_LOGW("panel_perf", "PPA draw failed (%s), CPU fallback", esp_err_to_name(perr));
    }

    const int lines_per_chunk = 20;
    const bool use_maps = ensure_scale_maps(dst_w, dst_h, logical_w, logical_h);

    const size_t line_pixels = (size_t)dst_w * lines_per_chunk;
    if (s_doom_lines_pixels < line_pixels) {
        for (int i = 0; i < DOOM_LINE_BUFFERS; i++) {
            free(s_doom_lines[i]);
            s_doom_lines[i] = NULL;
        }
        s_doom_lines_pixels = 0;
    }
    if (s_doom_lines_pixels == 0) {
        for (int i = 0; i < DOOM_LINE_BUFFERS; i++) {
            s_doom_lines[i] = heap_caps_malloc(line_pixels * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (s_doom_lines[i] == NULL) {
                s_doom_lines[i] = heap_caps_malloc(line_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (s_doom_lines[i] == NULL) {
                for (int j = 0; j < DOOM_LINE_BUFFERS; j++) {
                    free(s_doom_lines[j]);
                    s_doom_lines[j] = NULL;
                }
                return ESP_ERR_NO_MEM;
            }
        }
        s_doom_lines_pixels = line_pixels;
        ESP_LOGI("panel_perf", "line buffers (%u KB) in %s RAM",
                 (unsigned)(DOOM_LINE_BUFFERS * line_pixels * sizeof(uint16_t) / 1024),
                 esp_ptr_internal(s_doom_lines[0]) ? "INTERNAL" : "PSRAM");
    }

    if (s_panel_lock != NULL) {
        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    }
    const bool adjust_color = !color_settings_are_neutral(&settings);
    const panel_rotation_t rot = settings.rotation;
    const bool swap_axes = (rot == PANEL_ROTATION_90_CW || rot == PANEL_ROTATION_270_CW);
    const bool flip180 = (rot == PANEL_ROTATION_180);
    const int64_t scale_t0 = esp_timer_get_time(); /* PERF: scale+blit timing */
    int64_t blit_us = 0;                            /* PERF: draw_bitmap-only time */
    for (int screen_y0 = 0; screen_y0 < dst_screen_h; screen_y0 += lines_per_chunk) {
        uint16_t *lines = s_doom_lines[s_doom_line_index++ % DOOM_LINE_BUFFERS];
        int h = dst_screen_h - screen_y0;
        if (h > lines_per_chunk) {
            h = lines_per_chunk;
        }

        int prev_ly = -1; /* last gathered source row, for vertical row dedup */
        for (int y = 0; y < h; y++) {
            int screen_y = screen_y0 + y;
            int doom_y = screen_y - dst_y0;
            uint16_t *dline = lines + y * dst_w;
            if (doom_y < 0 || doom_y >= dst_h) {
                memset(dline, 0, dst_w * sizeof(uint16_t));
                prev_ly = -1;
                continue;
            }

            if (!use_maps) {
                /* Fallback path: per-pixel divide. Only taken if the (tiny) scale
                 * LUTs failed to allocate. */
                prev_ly = -1;
                for (int x = 0; x < dst_w; x++) {
                    int logical_x = (x * logical_w) / dst_w;
                    int logical_y = (doom_y * logical_h) / dst_h;
                    int src_x = logical_x;
                    int src_y = logical_y;
                    switch (rot) {
                    case PANEL_ROTATION_90_CW:  src_x = logical_y; src_y = src_h - 1 - logical_x; break;
                    case PANEL_ROTATION_270_CW: src_x = src_w - 1 - logical_y; src_y = logical_x; break;
                    case PANEL_ROTATION_180:    src_x = src_w - 1 - logical_x; src_y = src_h - 1 - logical_y; break;
                    default: break;
                    }
                    uint16_t color = src[src_y * src_w + src_x];
                    dline[x] = adjust_color ? apply_color_settings(color, &settings) : color;
                }
                continue;
            }

            const int ly_val = s_scale_ly[doom_y];
            if (ly_val == prev_ly) {
                /* Same source row as the line just above: nearest-neighbour gives an
                 * identical output row, so copy it instead of re-gathering. */
                memcpy(dline, dline - dst_w, dst_w * sizeof(uint16_t));
                continue;
            }
            prev_ly = ly_val;
            if (!swap_axes) {
                const int src_y = flip180 ? (src_h - 1 - ly_val) : ly_val;
                const uint16_t *srow = src + (size_t)src_y * src_w;
                if (!adjust_color && !flip180) {
                    /* Hot path (rotation 0, neutral colours): a plain gather, no
                     * divide and no branch. */
                    for (int x = 0; x < dst_w; x++) {
                        dline[x] = srow[s_scale_lx[x]];
                    }
                } else {
                    for (int x = 0; x < dst_w; x++) {
                        int src_x = flip180 ? (src_w - 1 - s_scale_lx[x]) : s_scale_lx[x];
                        uint16_t color = srow[src_x];
                        dline[x] = adjust_color ? apply_color_settings(color, &settings) : color;
                    }
                }
            } else {
                const int sxc = (rot == PANEL_ROTATION_90_CW) ? ly_val : (src_w - 1 - ly_val);
                for (int x = 0; x < dst_w; x++) {
                    int lxv = s_scale_lx[x];
                    int src_y = (rot == PANEL_ROTATION_90_CW) ? (src_h - 1 - lxv) : lxv;
                    uint16_t color = src[(size_t)src_y * src_w + sxc];
                    dline[x] = adjust_color ? apply_color_settings(color, &settings) : color;
                }
            }
        }

        if (settings.show_fps) {
            draw_fps_overlay(lines, screen_y0, h, dst_w, s_fps_value_x10);
        }

        const int64_t blit_t0 = esp_timer_get_time();
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, screen_y0, dst_w, screen_y0 + h, lines);
        blit_us += esp_timer_get_time() - blit_t0;
        if (err != ESP_OK) {
            if (s_panel_lock != NULL) {
                xSemaphoreGive(s_panel_lock);
            }
            return err;
        }
    }
    if (s_panel_lock != NULL) {
        xSemaphoreGive(s_panel_lock);
    }

    /* PERF: average scale+blit time. Compare against the on-screen FPS (total
     * frame time = 1000/FPS ms) to see how much is Doom's own rendering. */
    static int64_t s_scale_us_accum;
    static int64_t s_blit_us_accum;
    static uint32_t s_scale_us_frames;
    s_scale_us_accum += esp_timer_get_time() - scale_t0;
    s_blit_us_accum += blit_us;
    if (++s_scale_us_frames >= 64) {
        int64_t total = s_scale_us_accum / s_scale_us_frames;
        int64_t blit = s_blit_us_accum / s_scale_us_frames;
        ESP_LOGI("panel_perf", "total %lld us/frame = scale %lld + blit %lld",
                 (long long)total, (long long)(total - blit), (long long)blit);
        s_scale_us_accum = 0;
        s_blit_us_accum = 0;
        s_scale_us_frames = 0;
    }

    return ESP_OK;
}
