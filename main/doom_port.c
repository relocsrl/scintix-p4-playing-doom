#include "doom_port.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "panel_display.h"
#include "usb_gamepad.h"
#include "doom_agent.h"

/* Doom engine internals, for the agent's state capture. The doomgeneric
 * component exports its src/ directory as a public include dir. */
#include "doomstat.h"
#include "d_items.h"
#include "p_local.h"
#include "r_main.h"

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
static uint32_t s_last_keyboard_sequence;
static usb_keyboard_state_t s_kbd_prev;
static bool s_spiffs_mounted;

/* Agent (lockstep) bridge. The WebSocket task hands an action to the Doom task
 * via s_agent_step_req, waits on s_agent_step_done, then reads s_agent_obs. */
static SemaphoreHandle_t s_agent_step_req;
static SemaphoreHandle_t s_agent_step_done;
static doom_agent_action_t s_agent_action;
static doom_agent_obs_t s_agent_obs;
static volatile bool s_agent_active;
static volatile bool s_doom_ready;
static int s_agent_idle_ms;

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

/* Map a HID keyboard usage code to a Doom key. Modifier keys (Ctrl/Shift/Alt)
 * are handled separately from the report's modifier byte. */
static unsigned char kbd_usage_to_doom(uint8_t usage)
{
    switch (usage) {
    case 0x52: return KEY_UPARROW;     /* Up    -> forward */
    case 0x51: return KEY_DOWNARROW;   /* Down  -> back */
    case 0x50: return KEY_LEFTARROW;   /* Left  -> turn left */
    case 0x4F: return KEY_RIGHTARROW;  /* Right -> turn right */
    case 0x2C: return KEY_USE;         /* Space -> use/open */
    case 0x28: return KEY_ENTER;       /* Enter */
    case 0x58: return KEY_ENTER;       /* Keypad Enter */
    case 0x29: return KEY_ESCAPE;      /* Esc -> menu */
    case 0x2B: return KEY_TAB;         /* Tab -> automap */
    case 0x2A: return KEY_BACKSPACE;   /* Backspace */
    case 0x36: return KEY_STRAFE_L;    /* , -> strafe left */
    case 0x37: return KEY_STRAFE_R;    /* . -> strafe right */
    default: break;
    }
    if (usage >= 0x1E && usage <= 0x26) {  /* 1..9 -> weapon select / menu */
        return (unsigned char)('1' + (usage - 0x1E));
    }
    if (usage == 0x27) {                   /* 0 */
        return '0';
    }
    if (usage >= 0x04 && usage <= 0x1D) {  /* a..z (y/n prompts, etc.) */
        return (unsigned char)('a' + (usage - 0x04));
    }
    return 0;
}

/* Translate a USB keyboard into Doom key events: Ctrl=fire, Shift=run, Alt=strafe,
 * arrows=move/turn, Space=use, ,/.=strafe, 1-7=weapons, Esc/Enter/Tab for menus. */
static void poll_keyboard_keys(void)
{
    usb_keyboard_state_t state;
    if (!usb_keyboard_get_state(&state)) {
        return;
    }
    if (state.sequence == s_last_keyboard_sequence) {
        return;
    }
    s_last_keyboard_sequence = state.sequence;

    const uint8_t cur_mod = state.connected ? state.modifiers : 0;
    static const struct {
        uint8_t mask;
        unsigned char key;
    } mod_map[] = {
        {0x11, KEY_FIRE},    /* Left/Right Ctrl  -> fire */
        {0x22, KEY_RSHIFT},  /* Left/Right Shift -> run */
        {0x44, KEY_RALT},    /* Left/Right Alt   -> strafe */
    };
    for (size_t i = 0; i < sizeof(mod_map) / sizeof(mod_map[0]); i++) {
        bool now = (cur_mod & mod_map[i].mask) != 0;
        bool was = (s_kbd_prev.modifiers & mod_map[i].mask) != 0;
        if (now != was) {
            queue_key_event(now, mod_map[i].key);
        }
    }

    /* Press events for keys newly present, release for keys newly absent. */
    for (int i = 0; i < 6; i++) {
        uint8_t k = state.connected ? state.keys[i] : 0;
        if (k < 0x04) {
            continue;
        }
        bool was = false;
        for (int j = 0; j < 6; j++) {
            if (s_kbd_prev.keys[j] == k) {
                was = true;
                break;
            }
        }
        if (!was) {
            unsigned char dk = kbd_usage_to_doom(k);
            if (dk != 0) {
                queue_key_event(1, dk);
            }
        }
    }
    for (int j = 0; j < 6; j++) {
        uint8_t k = s_kbd_prev.keys[j];
        if (k < 0x04) {
            continue;
        }
        bool still = false;
        for (int i = 0; i < 6; i++) {
            if ((state.connected ? state.keys[i] : 0) == k) {
                still = true;
                break;
            }
        }
        if (!still) {
            unsigned char dk = kbd_usage_to_doom(k);
            if (dk != 0) {
                queue_key_event(0, dk);
            }
        }
    }

    s_kbd_prev.modifiers = cur_mod;
    for (int i = 0; i < 6; i++) {
        s_kbd_prev.keys[i] = state.connected ? state.keys[i] : 0;
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

/* Printed raw (not through ESP_LOG) so the figlet "Doom" font stays aligned in
 * the serial console, with no per-line timestamp/tag prefix mangling it. The
 * diagonal cuts on the D/M edges give it the angular DOOM-logo feel. */
static void print_doom_banner(void)
{
    printf("\n"
           "    ______    _____    _____   ___  ___\n"
           "    |  _  \\  |  _  |  |  _  |  |  \\/  |\n"
           "    | | | |  | | | |  | | | |  | .  . |\n"
           "    | | | |  | | | |  | | | |  | |\\/| |\n"
           "    | |/ /   \\ \\_/ /  \\ \\_/ /  | |  | |\n"
           "    |___/     \\___/    \\___/   \\_|  |_/\n"
           "         SCINTIX P4 — rip and tear!\n\n");
}

/* Inject the agent's action as held keys, advance the requested tics, then
 * release. Runs on the Doom task. A final tick is always run so the key
 * releases are processed and the captured frame is current. */
static void agent_apply_and_tick(const doom_agent_action_t *a)
{
    int tics = a->tics;
    if (tics < 0) {
        tics = 0;
    }
    if (tics > 60) {
        tics = 60;
    }

    unsigned char held[6];
    int n = 0;
    if (a->move > 0) {
        held[n++] = KEY_UPARROW;
    } else if (a->move < 0) {
        held[n++] = KEY_DOWNARROW;
    }
    if (a->turn < 0) {
        held[n++] = KEY_LEFTARROW;
    } else if (a->turn > 0) {
        held[n++] = KEY_RIGHTARROW;
    }
    if (a->strafe < 0) {
        held[n++] = KEY_STRAFE_L;
    } else if (a->strafe > 0) {
        held[n++] = KEY_STRAFE_R;
    }
    if (a->fire) {
        held[n++] = KEY_FIRE;
    }
    if (a->use) {
        held[n++] = KEY_USE;
    }
    unsigned char weapon_key = (a->weapon >= 1 && a->weapon <= 7) ? (unsigned char)('0' + a->weapon) : 0;

    for (int i = 0; i < n; i++) {
        queue_key_event(1, held[i]);
    }
    if (weapon_key) {
        queue_key_event(1, weapon_key);
    }
    for (int i = 0; i < tics; i++) {
        doomgeneric_Tick();
    }
    for (int i = 0; i < n; i++) {
        queue_key_event(0, held[i]);
    }
    if (weapon_key) {
        queue_key_event(0, weapon_key);
    }
    doomgeneric_Tick(); /* flush the key releases, refresh the frame */
}

/* Capture the current game state into an observation. Runs on the Doom task. */
static void agent_capture_obs(doom_agent_obs_t *o)
{
    memset(o, 0, sizeof(*o));
    player_t *pl = &players[consoleplayer];
    mobj_t *pm = pl->mo;
    if (pm == NULL) {
        o->valid = false;
        return;
    }
    o->valid = true;
    o->x = pm->x >> FRACBITS;
    o->y = pm->y >> FRACBITS;
    o->z = pm->z >> FRACBITS;
    o->angle_deg = (int)(((uint64_t)pm->angle * 360u) >> 32);
    o->health = pl->health;
    o->armor = pl->armorpoints;
    o->weapon = pl->readyweapon;
    int ammotype = weaponinfo[pl->readyweapon].ammo;
    o->ammo = (ammotype >= 0 && ammotype < NUMAMMO) ? pl->ammo[ammotype] : -1;
    o->episode = gameepisode;
    o->map = gamemap;

    /* What's directly in front of the player (autoaim along the facing). */
    P_AimLineAttack(pm, pm->angle, MISSILERANGE);
    if (linetarget != NULL) {
        o->front_type = linetarget->type;
        o->front_dist = P_AproxDistance(linetarget->x - pm->x, linetarget->y - pm->y) >> FRACBITS;
    } else {
        o->front_type = -1;
        o->front_dist = -1;
    }

    /* Nearby shootable things (monsters), with distance, bearing and line-of-sight. */
    int n = 0;
    for (thinker_t *th = thinkercap.next; th != &thinkercap && n < DOOM_AGENT_MAX_ENEMIES; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) {
            continue;
        }
        mobj_t *mo = (mobj_t *)th;
        if (mo == pm || mo->health <= 0 || !(mo->flags & MF_SHOOTABLE)) {
            continue;
        }
        angle_t rel = R_PointToAngle2(pm->x, pm->y, mo->x, mo->y) - pm->angle;
        int bearing = (int)(((uint64_t)rel * 360u) >> 32);
        if (bearing > 180) {
            bearing -= 360;
        }
        o->enemies[n].type = mo->type;
        o->enemies[n].dist = P_AproxDistance(mo->x - pm->x, mo->y - pm->y) >> FRACBITS;
        o->enemies[n].bearing_deg = bearing;
        o->enemies[n].in_sight = P_CheckSight(pm, mo);
        n++;
    }
    o->num_enemies = n;
}

bool doom_agent_step(const doom_agent_action_t *action, doom_agent_obs_t *obs)
{
    if (!s_doom_ready || s_agent_step_req == NULL) {
        memset(obs, 0, sizeof(*obs));
        return false;
    }
    s_agent_active = true;
    s_agent_action = *action;
    xSemaphoreGive(s_agent_step_req);
    if (xSemaphoreTake(s_agent_step_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
        memset(obs, 0, sizeof(*obs));
        return false;
    }
    *obs = s_agent_obs;
    return true;
}

void doom_agent_set_active(bool active)
{
    s_agent_active = active;
    s_agent_idle_ms = 0;
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

    print_doom_banner();
    ESP_LOGI(TAG, "Starting Doom");
    doomgeneric_Create(argc, argv);
    s_doom_ready = true;

    while (true) {
        if (s_agent_active) {
            /* Lockstep: pause until the agent submits an action, advance, reply. */
            if (xSemaphoreTake(s_agent_step_req, pdMS_TO_TICKS(200)) == pdTRUE) {
                s_agent_idle_ms = 0;
                agent_apply_and_tick(&s_agent_action);
                agent_capture_obs(&s_agent_obs);
                xSemaphoreGive(s_agent_step_done);
            } else {
                s_agent_idle_ms += 200;
                if (s_agent_idle_ms >= 5000) {
                    /* Controller vanished without closing — resume real-time play. */
                    s_agent_active = false;
                    ESP_LOGW(TAG, "agent idle, resuming real-time play");
                }
            }
        } else {
            doomgeneric_Tick();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

esp_err_t doom_port_start(void)
{
    s_agent_step_req = xSemaphoreCreateBinary();
    s_agent_step_done = xSemaphoreCreateBinary();
    if (s_agent_step_req == NULL || s_agent_step_done == NULL) {
        ESP_LOGE(TAG, "Failed to create agent semaphores");
        return ESP_ERR_NO_MEM;
    }

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
    poll_keyboard_keys();
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
