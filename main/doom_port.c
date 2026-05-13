#include "doom_port.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_display.h"
#include "usb_gamepad.h"

#define DOOM_WAD_PATH "/spiffs/doom1.wad"
#define DOOM_EVENT_QUEUE_LEN 32

static const char *TAG = "doom_port";

typedef struct {
    int pressed;
    unsigned char key;
} doom_key_event_t;

static doom_key_event_t s_key_events[DOOM_EVENT_QUEUE_LEN];
static uint8_t s_key_head;
static uint8_t s_key_tail;
static uint32_t s_last_key_mask;
static uint32_t s_last_gamepad_sequence;
static bool s_spiffs_mounted;

enum {
    DOOM_KEY_UP = 1U << 0,
    DOOM_KEY_DOWN = 1U << 1,
    DOOM_KEY_LEFT = 1U << 2,
    DOOM_KEY_RIGHT = 1U << 3,
    DOOM_KEY_FIRE = 1U << 4,
    DOOM_KEY_USE = 1U << 5,
    DOOM_KEY_STRAFE_LEFT = 1U << 6,
    DOOM_KEY_STRAFE_RIGHT = 1U << 7,
    DOOM_KEY_MENU = 1U << 8,
    DOOM_KEY_ENTER = 1U << 9,
    DOOM_KEY_MAP = 1U << 10,
};

static void queue_key_event(int pressed, unsigned char key)
{
    uint8_t next = (uint8_t)((s_key_head + 1) % DOOM_EVENT_QUEUE_LEN);
    if (next == s_key_tail) {
        return;
    }

    s_key_events[s_key_head].pressed = pressed;
    s_key_events[s_key_head].key = key;
    s_key_head = next;
}

static bool pop_key_event(doom_key_event_t *event)
{
    if (s_key_tail == s_key_head) {
        return false;
    }

    *event = s_key_events[s_key_tail];
    s_key_tail = (uint8_t)((s_key_tail + 1) % DOOM_EVENT_QUEUE_LEN);
    return true;
}

static unsigned char doom_key_for_mask(uint32_t mask)
{
    switch (mask) {
    case DOOM_KEY_UP:
        return KEY_UPARROW;
    case DOOM_KEY_DOWN:
        return KEY_DOWNARROW;
    case DOOM_KEY_LEFT:
        return KEY_LEFTARROW;
    case DOOM_KEY_RIGHT:
        return KEY_RIGHTARROW;
    case DOOM_KEY_FIRE:
        return KEY_FIRE;
    case DOOM_KEY_USE:
        return KEY_USE;
    case DOOM_KEY_STRAFE_LEFT:
        return KEY_STRAFE_L;
    case DOOM_KEY_STRAFE_RIGHT:
        return KEY_STRAFE_R;
    case DOOM_KEY_MENU:
        return KEY_ESCAPE;
    case DOOM_KEY_ENTER:
        return KEY_ENTER;
    case DOOM_KEY_MAP:
        return KEY_TAB;
    default:
        return 0;
    }
}

static uint32_t gamepad_to_doom_keys(const usb_gamepad_state_t *state)
{
    if (!state->connected) {
        return 0;
    }

    uint32_t keys = 0;
    const uint8_t low = 80;
    const uint8_t high = 176;

    if (state->dpad == 0 || state->dpad == 1 || state->dpad == 7 || state->ly < low) {
        keys |= DOOM_KEY_UP;
    }
    if ((state->dpad >= 3 && state->dpad <= 5) || state->ly > high) {
        keys |= DOOM_KEY_DOWN;
    }
    if ((state->dpad >= 5 && state->dpad <= 7) || state->lx < low) {
        keys |= DOOM_KEY_LEFT;
    }
    if ((state->dpad >= 1 && state->dpad <= 3) || state->lx > high) {
        keys |= DOOM_KEY_RIGHT;
    }
    if ((state->buttons & USB_GAMEPAD_BUTTON_CROSS) || (state->buttons & USB_GAMEPAD_BUTTON_R2) || state->r2 > 64) {
        keys |= DOOM_KEY_FIRE;
    }
    if ((state->buttons & USB_GAMEPAD_BUTTON_CIRCLE) || (state->buttons & USB_GAMEPAD_BUTTON_L2) || state->l2 > 64) {
        keys |= DOOM_KEY_USE;
    }
    if (state->buttons & USB_GAMEPAD_BUTTON_L1) {
        keys |= DOOM_KEY_STRAFE_LEFT;
    }
    if (state->buttons & USB_GAMEPAD_BUTTON_R1) {
        keys |= DOOM_KEY_STRAFE_RIGHT;
    }
    if (state->buttons & USB_GAMEPAD_BUTTON_OPTIONS) {
        keys |= DOOM_KEY_MENU;
    }
    if (state->buttons & USB_GAMEPAD_BUTTON_TRIANGLE) {
        keys |= DOOM_KEY_ENTER;
    }
    if (state->buttons & USB_GAMEPAD_BUTTON_SHARE) {
        keys |= DOOM_KEY_MAP;
    }

    return keys;
}

static void poll_gamepad_keys(void)
{
    usb_gamepad_state_t state;
    if (!usb_gamepad_get_state(&state)) {
        return;
    }
    if (state.sequence == s_last_gamepad_sequence) {
        return;
    }
    s_last_gamepad_sequence = state.sequence;

    uint32_t keys = gamepad_to_doom_keys(&state);
    uint32_t changed = keys ^ s_last_key_mask;
    s_last_key_mask = keys;

    for (uint32_t bit = 1; bit != 0 && bit <= DOOM_KEY_MAP; bit <<= 1) {
        if (changed & bit) {
            unsigned char key = doom_key_for_mask(bit);
            if (key != 0) {
                queue_key_event((keys & bit) != 0, key);
            }
        }
    }
}

static esp_err_t mount_spiffs(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 6,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info("storage", &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    }
    s_spiffs_mounted = true;
    return ESP_OK;
}

static bool wad_exists(void)
{
    struct stat st;
    if (stat(DOOM_WAD_PATH, &st) != 0) {
        return false;
    }
    ESP_LOGI(TAG, "Found WAD: %s (%u bytes)", DOOM_WAD_PATH, (unsigned)st.st_size);
    return true;
}

static void doom_task(void *arg)
{
    (void)arg;

    if (mount_spiffs() != ESP_OK || !wad_exists()) {
        ESP_LOGW(TAG, "Doom waiting for %s. Put doom1.wad in the SPIFFS image and flash the storage partition.", DOOM_WAD_PATH);
        ESP_ERROR_CHECK_WITHOUT_ABORT(panel_display_fill(0x001f));
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(panel_display_fill(0x0000));

    char *argv[] = {
        "doomgeneric",
        "-iwad", DOOM_WAD_PATH,
        "-config", "/spiffs/doom.cfg",
        "-extraconfig", "/spiffs/doom-extra.cfg",
        "-mb", "8",
        "-gfxmode", "rgb565",
        "-nomusic",
    };
    int argc = sizeof(argv) / sizeof(argv[0]);

    ESP_LOGI(TAG, "Starting Doom");
    doomgeneric_Create(argc, argv);

    while (true) {
        doomgeneric_Tick();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t doom_port_start(void)
{
    BaseType_t created = xTaskCreatePinnedToCore(doom_task, "doom", 16384, NULL, tskIDLE_PRIORITY, NULL, 1);
    return created == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void DG_Init(void)
{
}

void DG_DrawFrame(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(panel_display_draw_doom_rgb565((const uint16_t *)DG_ScreenBuffer, 320, 200));
}

void DG_SleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    doom_key_event_t event;

    poll_gamepad_keys();
    if (!pop_key_event(&event)) {
        return 0;
    }

    *pressed = event.pressed;
    *key = event.key;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    ESP_LOGI(TAG, "Doom title: %s", title);
}
