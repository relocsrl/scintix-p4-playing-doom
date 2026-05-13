/*
 * Doom for the JC4880P443 ESP32-P4 + ESP32-C6 panel.
 *
 * mazur888 2026
 *
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "dns_server.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_private/wifi.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "audio_doom.h"
#include "doom_port.h"
#include "panel_display.h"
#include "usb_gamepad.h"

static const char *TAG = "jc4880p443";
static const char *NET_TAG = "wifi_portal";

#define LCD_H_RES 480
#define LCD_V_RES 800

#define LCD_HSYNC 12
#define LCD_HBP 42
#define LCD_HFP 42
#define LCD_VSYNC 2
#define LCD_VBP 8
#define LCD_VFP 166

#define LCD_DPI_CLK_MHZ 34
#define LCD_DSI_LANE_NUM 2
#define LCD_DSI_LANE_BITRATE_MBPS 500

#define LCD_RST_GPIO GPIO_NUM_5
#define LCD_BACKLIGHT_GPIO GPIO_NUM_23

#define MIPI_DSI_PHY_LDO_CHAN 3
#define MIPI_DSI_PHY_LDO_MV 2500

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_STA_RETRY 5
#define WIFI_PORTAL_MAX_CLIENTS 4
#define WIFI_PORTAL_RESTART_DELAY_MS 1200
#define WIFI_NVS_NAMESPACE "wifi_portal"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"

static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_portal_server;
static dns_server_handle_t s_dns_server;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static int s_wifi_retry_count;
static bool s_portal_started;
static bool s_wifi_started;
static bool s_sta_netif_started;
static bool s_ap_netif_started;

typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t data_len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

#define CMD(_cmd, ...)                                      \
    {                                                       \
        .cmd = (_cmd),                                      \
        .data = (const uint8_t[]){__VA_ARGS__},             \
        .data_len = sizeof((const uint8_t[]){__VA_ARGS__}), \
        .delay_ms = 0,                                      \
    }

#define CMD_DELAY(_cmd, _delay_ms, ...)                     \
    {                                                       \
        .cmd = (_cmd),                                      \
        .data = (const uint8_t[]){__VA_ARGS__},             \
        .data_len = sizeof((const uint8_t[]){__VA_ARGS__}), \
        .delay_ms = (_delay_ms),                            \
    }

static const lcd_init_cmd_t jc4880p443_init_cmds[] = {
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x13),
    CMD(0xEF, 0x08),
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10),
    CMD(0xC0, 0x63, 0x00),
    CMD(0xC1, 0x0D, 0x02),
    CMD(0xC2, 0x10, 0x08),
    CMD(0xCC, 0x10),
    CMD(0xB0, 0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71),
    CMD(0xB1, 0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D),
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x11),
    CMD(0xB0, 0x5D),
    CMD(0xB1, 0x58),
    CMD(0xB2, 0x87),
    CMD(0xB3, 0x80),
    CMD(0xB5, 0x4E),
    CMD(0xB7, 0x85),
    CMD(0xB8, 0x21),
    CMD(0xB9, 0x10, 0x1F),
    CMD(0xBB, 0x03),
    CMD(0xBC, 0x00),
    CMD(0xC1, 0x78),
    CMD(0xC2, 0x78),
    CMD(0xD0, 0x88),
    CMD(0xE0, 0x00, 0x3A, 0x02),
    CMD(0xE1, 0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40),
    CMD(0xE2, 0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00),
    CMD(0xE3, 0x00, 0x00, 0x33, 0x33),
    CMD(0xE4, 0x44, 0x44),
    CMD(0xE5, 0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0),
    CMD(0xE6, 0x00, 0x00, 0x33, 0x33),
    CMD(0xE7, 0x44, 0x44),
    CMD(0xE8, 0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0),
    CMD(0xEB, 0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00),
    CMD(0xEC, 0x08, 0x01),
    CMD(0xED, 0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B),
    CMD(0xEF, 0x08, 0x08, 0x08, 0x45, 0x3F, 0x54),
    CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x00),
    CMD(0x3A, 0x55),       // RGB565 pixel format.
    CMD(0x36, 0x00),       // RGB order, no mirroring.
    CMD(0x20),             // Inversion off.
    CMD_DELAY(0x11, 120),  // Sleep out.
    CMD_DELAY(0x29, 20),   // Display on.
};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

static void reset_lcd(void)
{
    gpio_config_t reset_config = {
        .pin_bit_mask = BIT64(LCD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&reset_config));

    gpio_set_level(LCD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(LCD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(LCD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void enable_backlight(void)
{
    gpio_config_t backlight_config = {
        .pin_bit_mask = BIT64(LCD_BACKLIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    gpio_set_level(LCD_BACKLIGHT_GPIO, 1);
    ESP_LOGI(TAG, "Backlight enabled on GPIO%d", LCD_BACKLIGHT_GPIO);
}

static void enable_mipi_dphy_power(void)
{
    esp_ldo_channel_config_t ldo_config = {
        .chan_id = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_MV,
    };
    esp_ldo_channel_handle_t ldo = NULL;
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &ldo));
    ESP_LOGI(TAG, "MIPI D-PHY LDO%d enabled at %dmV", MIPI_DSI_PHY_LDO_CHAN, MIPI_DSI_PHY_LDO_MV);
}

static void send_panel_init_sequence(esp_lcd_panel_io_handle_t io)
{
    for (size_t i = 0; i < sizeof(jc4880p443_init_cmds) / sizeof(jc4880p443_init_cmds[0]); i++) {
        const lcd_init_cmd_t *cmd = &jc4880p443_init_cmds[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, cmd->cmd, cmd->data, cmd->data_len));
        if (cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }
}

static void draw_test_pattern(esp_lcd_panel_handle_t panel)
{
    const int stripe_lines = 40;
    uint16_t *stripe = heap_caps_malloc(LCD_H_RES * stripe_lines * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stripe == NULL) {
        stripe = heap_caps_malloc(LCD_H_RES * stripe_lines * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    ESP_ERROR_CHECK(stripe == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    for (int y0 = 0; y0 < LCD_V_RES; y0 += stripe_lines) {
        int lines = (LCD_V_RES - y0) < stripe_lines ? (LCD_V_RES - y0) : stripe_lines;
        for (int y = 0; y < lines; y++) {
            int screen_y = y0 + y;
            for (int x = 0; x < LCD_H_RES; x++) {
                uint8_t r = (uint8_t)((x * 255) / (LCD_H_RES - 1));
                uint8_t g = (uint8_t)((screen_y * 255) / (LCD_V_RES - 1));
                uint8_t b = (uint8_t)(((x / 40) ^ (screen_y / 40)) & 1 ? 255 : 32);

                if (screen_y < 80) {
                    r = 255;
                    g = (x < LCD_H_RES / 2) ? 255 : 80;
                    b = 0;
                }

                stripe[y * LCD_H_RES + x] = rgb565(r, g, b);
            }
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y0, LCD_H_RES, y0 + lines, stripe));
    }

    free(stripe);
    ESP_LOGI(TAG, "Test pattern drawn");
}

static void url_decode(char *text)
{
    char *read = text;
    char *write = text;

    while (*read != '\0') {
        if (*read == '+') {
            *write++ = ' ';
            read++;
        } else if (read[0] == '%' && read[1] != '\0' && read[2] != '\0') {
            char hex[3] = {read[1], read[2], 0};
            *write++ = (char)strtol(hex, NULL, 16);
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static void trim_ascii_whitespace(char *text)
{
    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0) {
        char c = text[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        text[--len] = '\0';
    }
}

static bool load_saved_wifi_config(wifi_config_t *wifi_config)
{
    nvs_handle_t nvs;
    char ssid[33] = {};
    char password[65] = {};
    size_t ssid_len = sizeof(ssid);
    size_t password_len = sizeof(password);

    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_get_str(nvs, WIFI_NVS_SSID_KEY, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_NVS_PASSWORD_KEY, password, &password_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
            err = ESP_OK;
        }
    }
    nvs_close(nvs);

    if (err != ESP_OK || ssid[0] == '\0') {
        return false;
    }
    trim_ascii_whitespace(ssid);
    if (ssid[0] == '\0') {
        return false;
    }

    if (wifi_config != NULL) {
        memset(wifi_config, 0, sizeof(*wifi_config));
        strlcpy((char *)wifi_config->sta.ssid, ssid, sizeof(wifi_config->sta.ssid));
        strlcpy((char *)wifi_config->sta.password, password, sizeof(wifi_config->sta.password));
        wifi_config->sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    return true;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, WIFI_NVS_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_PASSWORD_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t clear_wifi_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_erase_key(nvs, WIFI_NVS_SSID_KEY));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_erase_key(nvs, WIFI_NVS_PASSWORD_KEY));
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static bool wifi_has_saved_credentials(void)
{
    return load_saved_wifi_config(NULL);
}

static void get_portal_ssid(char *ssid, size_t ssid_len)
{
    uint8_t mac[6] = {};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BASE));
    snprintf(ssid, ssid_len, "JC4880P443-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void portal_send_page(httpd_req_t *req, const char *status)
{
    const char *status_text = status ? status : "Choose your Wi-Fi network and save it.";
    wifi_config_t saved_config = {};
    const char *saved_ssid = load_saved_wifi_config(&saved_config) ? (const char *)saved_config.sta.ssid : "";
    char page[1500];
    int len = snprintf(page, sizeof(page),
                       "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<title>JC4880P443 Wi-Fi</title>"
                       "<style>body{font-family:sans-serif;margin:2rem;max-width:34rem}"
                       "input,button{box-sizing:border-box;font-size:1rem;margin:.35rem 0;padding:.7rem;width:100%%}"
                       "button{font-weight:700}</style></head><body>"
                       "<h2>JC4880P443 Wi-Fi</h2><p>%s</p>"
                       "<form action=\"/save\" method=\"post\">"
                       "<label>SSID</label><input name=\"ssid\" maxlength=\"32\" autocomplete=\"off\" value=\"%s\" autofocus>"
                       "<label>Password</label><input name=\"password\" maxlength=\"64\" type=\"password\">"
                       "<button type=\"submit\">Connect</button></form>"
                       "<p><a href=\"/reset\">Forget saved Wi-Fi</a></p>"
                       "</body></html>",
                       status_text, saved_ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, len);
}

static esp_err_t portal_root_get_handler(httpd_req_t *req)
{
    if (wifi_has_saved_credentials()) {
        httpd_resp_set_status(req, "302 Temporary Redirect");
        httpd_resp_set_hdr(req, "Location", "/settings");
        httpd_resp_sendstr(req, "Redirecting to settings");
    } else {
        portal_send_page(req, NULL);
    }
    return ESP_OK;
}

static esp_err_t portal_wifi_get_handler(httpd_req_t *req)
{
    portal_send_page(req, NULL);
    return ESP_OK;
}

static void delayed_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_PORTAL_RESTART_DELAY_MS));
    esp_restart();
}

static esp_err_t start_wifi_netif(esp_netif_t *netif, esp_event_base_t event_base, int32_t event_id, void *event_data);
static esp_err_t connect_wifi_netif(esp_netif_t *netif, esp_event_base_t event_base, int32_t event_id, void *event_data);

static esp_err_t portal_reset_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<h2>Wi-Fi credentials cleared. Restarting...</h2>");
    ESP_ERROR_CHECK_WITHOUT_ABORT(clear_wifi_credentials());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_restore());
    xTaskCreate(delayed_restart_task, "wifi_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t portal_save_post_handler(httpd_req_t *req)
{
    char body[384] = {};
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        portal_send_page(req, "Request was too large or empty.");
        return ESP_OK;
    }

    int offset = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + offset, remaining);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        offset += received;
        remaining -= received;
    }
    body[offset] = '\0';

    char ssid[33] = {};
    char password[65] = {};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        portal_send_page(req, "SSID is required.");
        return ESP_OK;
    }
    (void)httpd_query_key_value(body, "password", password, sizeof(password));
    url_decode(ssid);
    url_decode(password);
    trim_ascii_whitespace(ssid);

    if (strlen(ssid) == 0) {
        portal_send_page(req, "SSID is required.");
        return ESP_OK;
    }

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_LOGI(NET_TAG, "Saving Wi-Fi credentials for SSID '%s'", ssid);
    esp_err_t err = save_wifi_credentials(ssid, password);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK) {
        s_wifi_retry_count = 0;
        err = esp_wifi_connect();
    }

    if (err == ESP_OK) {
        portal_send_page(req, "Credentials saved. Connecting now; the setup network can be closed after the device joins.");
    } else {
        ESP_LOGE(NET_TAG, "Failed to save Wi-Fi credentials: %s", esp_err_to_name(err));
        portal_send_page(req, "Failed to save credentials. Check serial logs.");
    }
    return ESP_OK;
}

static const char *rotation_name(panel_rotation_t rotation)
{
    switch (rotation) {
    case PANEL_ROTATION_90_CW:
        return "Default";
    case PANEL_ROTATION_270_CW:
        return "Flipped";
    case PANEL_ROTATION_0:
    case PANEL_ROTATION_180:
    default:
        return "Default";
    }
}

static void append_controller_button(char *buf, size_t buf_len, const char *name)
{
    if (buf[0] != '\0') {
        strlcat(buf, ",", buf_len);
    }
    strlcat(buf, name, buf_len);
}

static void controller_buttons_to_text(uint16_t buttons, char *buf, size_t buf_len)
{
    buf[0] = '\0';
    if (buttons & USB_GAMEPAD_BUTTON_SQUARE) append_controller_button(buf, buf_len, "square");
    if (buttons & USB_GAMEPAD_BUTTON_CROSS) append_controller_button(buf, buf_len, "cross");
    if (buttons & USB_GAMEPAD_BUTTON_CIRCLE) append_controller_button(buf, buf_len, "circle");
    if (buttons & USB_GAMEPAD_BUTTON_TRIANGLE) append_controller_button(buf, buf_len, "triangle");
    if (buttons & USB_GAMEPAD_BUTTON_L1) append_controller_button(buf, buf_len, "l1");
    if (buttons & USB_GAMEPAD_BUTTON_R1) append_controller_button(buf, buf_len, "r1");
    if (buttons & USB_GAMEPAD_BUTTON_L2) append_controller_button(buf, buf_len, "l2");
    if (buttons & USB_GAMEPAD_BUTTON_R2) append_controller_button(buf, buf_len, "r2");
    if (buttons & USB_GAMEPAD_BUTTON_SHARE) append_controller_button(buf, buf_len, "share");
    if (buttons & USB_GAMEPAD_BUTTON_OPTIONS) append_controller_button(buf, buf_len, "options");
    if (buttons & USB_GAMEPAD_BUTTON_L3) append_controller_button(buf, buf_len, "l3");
    if (buttons & USB_GAMEPAD_BUTTON_R3) append_controller_button(buf, buf_len, "r3");
    if (buttons & USB_GAMEPAD_BUTTON_PS) append_controller_button(buf, buf_len, "ps");
    if (buttons & USB_GAMEPAD_BUTTON_TOUCHPAD) append_controller_button(buf, buf_len, "touchpad");
    if (buf[0] == '\0') {
        strlcpy(buf, "none", buf_len);
    }
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    panel_display_settings_t settings;
    panel_display_get_settings(&settings);
    uint8_t audio_volume = doom_audio_get_volume();

    const size_t page_size = 6200;
    char *page = heap_caps_malloc(page_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (page == NULL) {
        page = heap_caps_malloc(page_size, MALLOC_CAP_8BIT);
    }
    if (page == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory for settings page");
        return ESP_OK;
    }
    int len = snprintf(page, page_size,
                       "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<title>Doom P4 Settings . 2026</title>"
                       "<style>body{font-family:sans-serif;margin:1.25rem;max-width:42rem;background:#101418;color:#edf2f7}"
                       "a{color:#8cc7ff}.row{display:grid;grid-template-columns:9rem 1fr;gap:.8rem;align-items:center;margin:.7rem 0}"
                       "select,input,button{font-size:1rem;padding:.55rem}button{font-weight:700;width:100%%}"
                       ".panel{border:1px solid #344150;border-radius:8px;padding:1rem;margin:1rem 0;background:#161d24}"
                       ".sw{display:flex;gap:.5rem}.c{height:2.2rem;flex:1;border-radius:4px}.muted{color:#aab6c2}"
                       "pre{white-space:pre-wrap;background:#0b0f13;padding:.8rem;border-radius:6px}</style></head><body>"
                       "<h2>Doom P4 Settings&nbsp;&nbsp;.&nbsp;&nbsp;2026</h2>"
                       "<div class=\"panel\"><form action=\"/settings/save\" method=\"post\">"
                       "<div class=\"row\"><label>Rotation</label><select name=\"rotation\">"
                       "<option value=\"1\"%s>Default</option>"
                       "<option value=\"3\"%s>Flipped</option></select></div>"
                       "<div class=\"row\"><label>Swap red/blue</label><input type=\"checkbox\" name=\"swap_rb\" value=\"1\" %s></div>"
                       "<div class=\"row\"><label>Invert colors</label><input type=\"checkbox\" name=\"invert\" value=\"1\" %s></div>"
                       "<div class=\"row\"><label>Show FPS</label><input type=\"checkbox\" name=\"show_fps\" value=\"1\" %s></div>"
                       "<div class=\"row\"><label>Red gain</label><input type=\"range\" name=\"red_gain\" min=\"50\" max=\"150\" value=\"%u\" oninput=\"rg.value=this.value\"><output id=\"rg\">%u</output></div>"
                       "<div class=\"row\"><label>Green gain</label><input type=\"range\" name=\"green_gain\" min=\"50\" max=\"150\" value=\"%u\" oninput=\"gg.value=this.value\"><output id=\"gg\">%u</output></div>"
                       "<div class=\"row\"><label>Blue gain</label><input type=\"range\" name=\"blue_gain\" min=\"50\" max=\"150\" value=\"%u\" oninput=\"bg.value=this.value\"><output id=\"bg\">%u</output></div>"
                       "<div class=\"row\"><label>Audio volume</label><input type=\"range\" name=\"audio_volume\" min=\"0\" max=\"100\" value=\"%u\" oninput=\"av.value=this.value\"><output id=\"av\">%u</output></div>"
                       "<div class=\"sw\"><div class=\"c\" style=\"background:#f00\"></div><div class=\"c\" style=\"background:#0f0\"></div><div class=\"c\" style=\"background:#00f\"></div></div>"
                       "<p><button type=\"submit\">Save settings</button></p></form></div>"
                       "<div class=\"panel\"><h3>Controller Test</h3><p class=\"muted\">Live DS4 state from USB host.</p><pre id=\"pad\">Waiting...</pre></div>"
                       "<p><a href=\"/wifi\">Wi-Fi setup</a></p>"
                       "<script>async function tick(){try{let r=await fetch('/api/controller');let j=await r.json();"
                       "pad.textContent='connected: '+j.connected+'\\nbuttons: '+j.buttons_text+'\\n'"
                       "+'dpad: '+j.dpad+'\\nlx/ly: '+j.lx+'/'+j.ly+'\\nrx/ry: '+j.rx+'/'+j.ry+'\\nl2/r2: '+j.l2+'/'+j.r2;}"
                       "catch(e){pad.textContent='controller API unavailable';}}setInterval(tick,1000);tick();</script>"
                       "</body></html>",
                       settings.rotation == PANEL_ROTATION_90_CW ? " selected" : "",
                       settings.rotation == PANEL_ROTATION_270_CW ? " selected" : "",
                       settings.swap_red_blue ? "checked" : "",
                       settings.invert_colors ? "checked" : "",
                       settings.show_fps ? "checked" : "",
                       settings.red_gain, settings.red_gain,
                       settings.green_gain, settings.green_gain,
                       settings.blue_gain, settings.blue_gain,
                       audio_volume, audio_volume);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, len);
    free(page);
    return ESP_OK;
}

static bool form_has_key(const char *body, const char *key)
{
    const char *p = body;
    size_t key_len = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        bool start_ok = p == body || p[-1] == '&';
        bool end_ok = p[key_len] == '=' || p[key_len] == '&' || p[key_len] == '\0';
        if (start_ok && end_ok) {
            return true;
        }
        p += key_len;
    }
    return false;
}

static uint8_t form_get_u8(const char *body, const char *key, uint8_t fallback)
{
    char value[8] = {};
    if (httpd_query_key_value(body, key, value, sizeof(value)) != ESP_OK) {
        return fallback;
    }
    int parsed = atoi(value);
    if (parsed < 0) {
        parsed = 0;
    }
    if (parsed > 255) {
        parsed = 255;
    }
    return (uint8_t)parsed;
}

static bool display_settings_equal(const panel_display_settings_t *a, const panel_display_settings_t *b)
{
    return a->rotation == b->rotation &&
           a->swap_red_blue == b->swap_red_blue &&
           a->invert_colors == b->invert_colors &&
           a->show_fps == b->show_fps &&
           a->red_gain == b->red_gain &&
           a->green_gain == b->green_gain &&
           a->blue_gain == b->blue_gain;
}

static esp_err_t settings_save_post_handler(httpd_req_t *req)
{
    char body[384] = {};
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad settings request");
        return ESP_OK;
    }

    int offset = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + offset, remaining);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        offset += received;
        remaining -= received;
    }
    body[offset] = '\0';

    panel_display_settings_t settings;
    panel_display_get_settings(&settings);
    panel_display_settings_t old_settings = settings;
    uint8_t old_audio_volume = doom_audio_get_volume();
    settings.rotation = (panel_rotation_t)form_get_u8(body, "rotation", (uint8_t)settings.rotation);
    if (settings.rotation != PANEL_ROTATION_270_CW) {
        settings.rotation = PANEL_ROTATION_90_CW;
    }
    settings.swap_red_blue = form_has_key(body, "swap_rb");
    settings.invert_colors = form_has_key(body, "invert");
    settings.show_fps = form_has_key(body, "show_fps");
    settings.red_gain = form_get_u8(body, "red_gain", settings.red_gain);
    settings.green_gain = form_get_u8(body, "green_gain", settings.green_gain);
    settings.blue_gain = form_get_u8(body, "blue_gain", settings.blue_gain);
    uint8_t audio_volume = form_get_u8(body, "audio_volume", old_audio_volume);

    esp_err_t err = ESP_OK;
    if (!display_settings_equal(&old_settings, &settings)) {
        err = panel_display_save_settings(&settings);
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "Failed to save display settings: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
            return ESP_OK;
        }
    }

    if (audio_volume != old_audio_volume) {
        err = doom_audio_save_settings(audio_volume);
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "Failed to save audio settings: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save audio settings");
            return ESP_OK;
        }
    }

    ESP_LOGI(NET_TAG, "Settings saved: rotation=%s swap_rb=%d invert=%d show_fps=%d gain=%u/%u/%u volume=%u",
             rotation_name(settings.rotation), settings.swap_red_blue, settings.invert_colors,
             settings.show_fps, settings.red_gain, settings.green_gain, settings.blue_gain, audio_volume);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings");
    httpd_resp_sendstr(req, "Saved");
    return ESP_OK;
}

static esp_err_t api_settings_get_handler(httpd_req_t *req)
{
    panel_display_settings_t settings;
    panel_display_get_settings(&settings);
    uint8_t audio_volume = doom_audio_get_volume();
    char json[320];
    int len = snprintf(json, sizeof(json),
                       "{\"rotation\":%u,\"rotation_text\":\"%s\",\"swap_red_blue\":%s,"
                       "\"invert_colors\":%s,\"show_fps\":%s,\"red_gain\":%u,\"green_gain\":%u,\"blue_gain\":%u,"
                       "\"audio_volume\":%u}",
                       (unsigned)settings.rotation, rotation_name(settings.rotation),
                       settings.swap_red_blue ? "true" : "false",
                       settings.invert_colors ? "true" : "false",
                       settings.show_fps ? "true" : "false",
                       settings.red_gain, settings.green_gain, settings.blue_gain,
                       audio_volume);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t api_controller_get_handler(httpd_req_t *req)
{
    usb_gamepad_state_t state = {};
    usb_gamepad_get_state(&state);
    char buttons[128];
    controller_buttons_to_text(state.buttons, buttons, sizeof(buttons));

    char json[384];
    int len = snprintf(json, sizeof(json),
                       "{\"connected\":%s,\"lx\":%u,\"ly\":%u,\"rx\":%u,\"ry\":%u,"
                       "\"l2\":%u,\"r2\":%u,\"dpad\":%u,\"buttons\":%u,\"buttons_text\":\"%s\",\"sequence\":%lu}",
                       state.connected ? "true" : "false",
                       state.lx, state.ly, state.rx, state.ry, state.l2, state.r2,
                       state.dpad, state.buttons, buttons, (unsigned long)state.sequence);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t portal_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Redirecting to setup portal");
    return ESP_OK;
}

static esp_err_t start_portal_servers(void)
{
    if (s_portal_started) {
        return ESP_OK;
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.max_open_sockets = 7;
    http_config.max_uri_handlers = 12;
    http_config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_portal_server, &http_config);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "HTTP portal start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_root_get_handler,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = portal_save_post_handler,
    };
    const httpd_uri_t wifi = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = portal_wifi_get_handler,
    };
    const httpd_uri_t reset = {
        .uri = "/reset",
        .method = HTTP_GET,
        .handler = portal_reset_get_handler,
    };
    const httpd_uri_t settings = {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = settings_get_handler,
    };
    const httpd_uri_t settings_save = {
        .uri = "/settings/save",
        .method = HTTP_POST,
        .handler = settings_save_post_handler,
    };
    const httpd_uri_t api_settings = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = api_settings_get_handler,
    };
    const httpd_uri_t api_controller = {
        .uri = "/api/controller",
        .method = HTTP_GET,
        .handler = api_controller_get_handler,
    };
    if ((err = httpd_register_uri_handler(s_portal_server, &root)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_portal_server, &save)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_portal_server, &wifi)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_portal_server, &reset)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_portal_server, &settings)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_portal_server, &settings_save)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_portal_server, &api_settings)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_portal_server, &api_controller)) != ESP_OK ||
        (err = httpd_register_err_handler(s_portal_server, HTTPD_404_NOT_FOUND, portal_404_handler)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "HTTP portal handler registration failed: %s", esp_err_to_name(err));
        httpd_stop(s_portal_server);
        s_portal_server = NULL;
        return err;
    }

    s_portal_started = true;
    return ESP_OK;
}

static esp_err_t start_wifi_portal(void)
{
    char ssid[32] = {};
    get_portal_ssid(ssid, sizeof(ssid));

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(ssid),
            .channel = 1,
            .max_connection = WIFI_PORTAL_MAX_CLIENTS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK || mode != WIFI_MODE_APSTA) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "Failed to set APSTA mode: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Failed to configure setup AP: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "Failed to start Wi-Fi for portal: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
    }

#ifdef CONFIG_ESP_ENABLE_DHCP_CAPTIVEPORTAL
    {
        esp_netif_ip_info_t ip_info;
        esp_err_t err = esp_netif_get_ip_info(s_ap_netif, &ip_info);
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "Failed to read AP IP: %s", esp_err_to_name(err));
            return err;
        }
        char ip_addr[16] = {};
        inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));
        char captive_url[32] = {};
        snprintf(captive_url, sizeof(captive_url), "http://%s", ip_addr);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap_netif));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                                             captive_url, strlen(captive_url)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ap_netif));
    }
#endif

    err = start_portal_servers();
    if (err != ESP_OK) {
        return err;
    }
    if (s_dns_server == NULL) {
        dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns_server = start_dns_server(&dns_config);
        if (s_dns_server == NULL) {
            ESP_LOGW(NET_TAG, "DNS captive redirect did not start; portal remains available by IP");
        }
    }

    esp_netif_ip_info_t ip_info;
    err = esp_netif_get_ip_info(s_ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Failed to read AP IP: %s", esp_err_to_name(err));
        return err;
    }
    char ip_addr[16] = {};
    inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));
    ESP_LOGI(NET_TAG, "Captive portal active: SSID '%s', URL http://%s", ssid, ip_addr);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_sta_netif_started) {
            if (start_wifi_netif(s_sta_netif, event_base, event_id, event_data) == ESP_OK) {
                s_sta_netif_started = true;
            }
        }
        if (wifi_has_saved_credentials()) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        if (s_sta_netif_started) {
            esp_netif_action_stop(s_sta_netif, event_base, event_id, event_data);
            s_sta_netif_started = false;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(connect_wifi_netif(s_sta_netif, event_base, event_id, event_data));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_netif_action_disconnected(s_sta_netif, event_base, event_id, event_data);
        if (wifi_has_saved_credentials() && s_wifi_retry_count < WIFI_MAX_STA_RETRY) {
            s_wifi_retry_count++;
            ESP_LOGW(NET_TAG, "STA disconnected, retry %d/%d", s_wifi_retry_count, WIFI_MAX_STA_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(NET_TAG, "No working Wi-Fi connection; starting captive portal");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_ERROR_CHECK_WITHOUT_ABORT(start_wifi_portal());
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        if (!s_ap_netif_started) {
            if (start_wifi_netif(s_ap_netif, event_base, event_id, event_data) == ESP_OK) {
                s_ap_netif_started = true;
            }
        } else {
            ESP_LOGW(NET_TAG, "Ignoring duplicate AP start event");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        if (s_ap_netif_started) {
            esp_netif_action_stop(s_ap_netif, event_base, event_id, event_data);
            s_ap_netif_started = false;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(NET_TAG, "Portal client joined: " MACSTR, MAC2STR(event->mac));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_action_got_ip(s_sta_netif, event_base, event_id, event_data);
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(NET_TAG, "Connected to Wi-Fi with IP " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_ERROR_CHECK_WITHOUT_ABORT(start_portal_servers());
    }
}

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

static esp_err_t start_wifi_netif(esp_netif_t *netif, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    wifi_netif_driver_t driver = esp_netif_get_io_driver(netif);
    uint8_t mac[6] = {};

    esp_err_t err = esp_wifi_get_if_mac(driver, mac);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Failed to read Wi-Fi interface MAC: %s", esp_err_to_name(err));
        return err;
    }

    if (esp_wifi_is_if_ready_when_started(driver)) {
        err = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "Failed to register AP receive callback: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Failed to register Wi-Fi netstack buffer callbacks: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, event_base, event_id, event_data);
    return ESP_OK;
}

static esp_err_t connect_wifi_netif(esp_netif_t *netif, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    wifi_netif_driver_t driver = esp_netif_get_io_driver(netif);

    if (!esp_wifi_is_if_ready_when_started(driver)) {
        esp_err_t err = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "Failed to register STA receive callback: %s", esp_err_to_name(err));
            return err;
        }
    }

    esp_netif_action_connected(netif, event_base, event_id, event_data);
    return ESP_OK;
}

static void init_wifi_with_portal(void)
{
    init_nvs();
    ESP_ERROR_CHECK_WITHOUT_ABORT(panel_display_load_settings());
    ESP_ERROR_CHECK_WITHOUT_ABORT(doom_audio_load_settings());
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(NET_TAG, "event loop init failed: %s", esp_err_to_name(err));
        return;
    }
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(NET_TAG, "Wi-Fi event group allocation failed");
        return;
    }

    esp_netif_inherent_config_t sta_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    esp_netif_inherent_config_t ap_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    s_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &sta_netif_config);
    s_ap_netif = esp_netif_create_wifi(WIFI_IF_AP, &ap_netif_config);
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        ESP_LOGE(NET_TAG, "Wi-Fi netif creation failed");
        return;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Wi-Fi event handler registration failed: %s", esp_err_to_name(err));
        return;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_config);
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
        return;
    }

    wifi_config_t saved_wifi_config = {};
    if (load_saved_wifi_config(&saved_wifi_config)) {
        ESP_LOGI(NET_TAG, "Saved Wi-Fi credentials found for SSID '%s'; starting STA", saved_wifi_config.sta.ssid);
        err = esp_wifi_set_config(WIFI_IF_STA, &saved_wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(NET_TAG, "Failed to apply saved Wi-Fi credentials: %s", esp_err_to_name(err));
            return;
        }
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            err = esp_wifi_start();
            if (err == ESP_OK) {
                s_wifi_started = true;
            }
        }
    } else {
        ESP_LOGI(NET_TAG, "No saved Wi-Fi credentials; starting captive portal");
        err = start_wifi_portal();
    }

    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Wi-Fi startup failed: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Doom P4 on JC4880P443");
    enable_mipi_dphy_power();

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = LCD_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCD_DSI_LANE_BITRATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    esp_lcd_panel_handle_t dpi_panel = NULL;
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_pulse_width = LCD_HSYNC,
            .hsync_back_porch = LCD_HBP,
            .hsync_front_porch = LCD_HFP,
            .vsync_pulse_width = LCD_VSYNC,
            .vsync_back_porch = LCD_VBP,
            .vsync_front_porch = LCD_VFP,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(dsi_bus, &dpi_config, &dpi_panel));
    panel_display_set_panel(dpi_panel);

    reset_lcd();
    ESP_ERROR_CHECK(esp_lcd_panel_init(dpi_panel));
    send_panel_init_sequence(dbi_io);
    draw_test_pattern(dpi_panel);
    enable_backlight();

    ESP_LOGI(TAG, "Doom P4 bringup complete");
    init_wifi_with_portal();
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_gamepad_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(doom_port_start());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
