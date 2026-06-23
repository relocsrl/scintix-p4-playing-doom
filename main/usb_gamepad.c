#include "usb_gamepad.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bit_defs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_gamepad";

typedef enum {
    GAMEPAD_EVENT_HID = 0,
} gamepad_event_group_t;

typedef struct {
    gamepad_event_group_t event_group;
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
    void *arg;
} gamepad_event_t;

typedef struct {
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t l2;
    uint8_t r2;
    uint8_t dpad;
    uint16_t buttons;
    bool valid;
} ds4_state_t;

enum {
    DS4_BTN_SQUARE = USB_GAMEPAD_BUTTON_SQUARE,
    DS4_BTN_CROSS = USB_GAMEPAD_BUTTON_CROSS,
    DS4_BTN_CIRCLE = USB_GAMEPAD_BUTTON_CIRCLE,
    DS4_BTN_TRIANGLE = USB_GAMEPAD_BUTTON_TRIANGLE,
    DS4_BTN_L1 = USB_GAMEPAD_BUTTON_L1,
    DS4_BTN_R1 = USB_GAMEPAD_BUTTON_R1,
    DS4_BTN_L2 = USB_GAMEPAD_BUTTON_L2,
    DS4_BTN_R2 = USB_GAMEPAD_BUTTON_R2,
    DS4_BTN_SHARE = USB_GAMEPAD_BUTTON_SHARE,
    DS4_BTN_OPTIONS = USB_GAMEPAD_BUTTON_OPTIONS,
    DS4_BTN_L3 = USB_GAMEPAD_BUTTON_L3,
    DS4_BTN_R3 = USB_GAMEPAD_BUTTON_R3,
    DS4_BTN_PS = USB_GAMEPAD_BUTTON_PS,
    DS4_BTN_TOUCHPAD = USB_GAMEPAD_BUTTON_TOUCHPAD,
};

static QueueHandle_t s_gamepad_event_queue;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static usb_gamepad_state_t s_public_state;
static hid_host_device_handle_t s_active_gamepad_handle;
static uint8_t s_active_gamepad_addr;
static uint8_t s_active_gamepad_iface;

static bool is_ds4_device(uint16_t vid, uint16_t pid)
{
    return vid == 0x054c && (pid == 0x05c4 || pid == 0x09cc);
}

static const char *hid_proto_name(hid_protocol_t proto)
{
    switch (proto) {
    case HID_PROTOCOL_KEYBOARD:
        return "KEYBOARD";
    case HID_PROTOCOL_MOUSE:
        return "MOUSE";
    case HID_PROTOCOL_NONE:
    default:
        return "GENERIC";
    }
}

static const char *dpad_name(uint8_t dpad)
{
    switch (dpad) {
    case 0:
        return "up";
    case 1:
        return "up-right";
    case 2:
        return "right";
    case 3:
        return "down-right";
    case 4:
        return "down";
    case 5:
        return "down-left";
    case 6:
        return "left";
    case 7:
        return "up-left";
    default:
        return "neutral";
    }
}

static void append_button(char *buf, size_t buf_len, const char *name)
{
    if (buf[0] != '\0') {
        strlcat(buf, ",", buf_len);
    }
    strlcat(buf, name, buf_len);
}

static void format_buttons(uint16_t buttons, char *buf, size_t buf_len)
{
    buf[0] = '\0';
    if (buttons & DS4_BTN_SQUARE) {
        append_button(buf, buf_len, "square");
    }
    if (buttons & DS4_BTN_CROSS) {
        append_button(buf, buf_len, "cross");
    }
    if (buttons & DS4_BTN_CIRCLE) {
        append_button(buf, buf_len, "circle");
    }
    if (buttons & DS4_BTN_TRIANGLE) {
        append_button(buf, buf_len, "triangle");
    }
    if (buttons & DS4_BTN_L1) {
        append_button(buf, buf_len, "l1");
    }
    if (buttons & DS4_BTN_R1) {
        append_button(buf, buf_len, "r1");
    }
    if (buttons & DS4_BTN_L2) {
        append_button(buf, buf_len, "l2");
    }
    if (buttons & DS4_BTN_R2) {
        append_button(buf, buf_len, "r2");
    }
    if (buttons & DS4_BTN_SHARE) {
        append_button(buf, buf_len, "share");
    }
    if (buttons & DS4_BTN_OPTIONS) {
        append_button(buf, buf_len, "options");
    }
    if (buttons & DS4_BTN_L3) {
        append_button(buf, buf_len, "l3");
    }
    if (buttons & DS4_BTN_R3) {
        append_button(buf, buf_len, "r3");
    }
    if (buttons & DS4_BTN_PS) {
        append_button(buf, buf_len, "ps");
    }
    if (buttons & DS4_BTN_TOUCHPAD) {
        append_button(buf, buf_len, "touchpad");
    }
    if (buf[0] == '\0') {
        strlcpy(buf, "none", buf_len);
    }
}

static bool ds4_parse_usb_report(const uint8_t *data, size_t len, ds4_state_t *state)
{
    size_t off = 0;

    if (len >= 10 && data[0] == 0x01) {
        off = 1;
    } else if (len < 9) {
        return false;
    }

    state->lx = data[off + 0];
    state->ly = data[off + 1];
    state->rx = data[off + 2];
    state->ry = data[off + 3];
    state->dpad = data[off + 4] & 0x0f;
    state->buttons = 0;
    state->buttons |= (data[off + 4] & BIT4) ? DS4_BTN_SQUARE : 0;
    state->buttons |= (data[off + 4] & BIT5) ? DS4_BTN_CROSS : 0;
    state->buttons |= (data[off + 4] & BIT6) ? DS4_BTN_CIRCLE : 0;
    state->buttons |= (data[off + 4] & BIT7) ? DS4_BTN_TRIANGLE : 0;
    state->buttons |= (data[off + 5] & BIT0) ? DS4_BTN_L1 : 0;
    state->buttons |= (data[off + 5] & BIT1) ? DS4_BTN_R1 : 0;
    state->buttons |= (data[off + 5] & BIT2) ? DS4_BTN_L2 : 0;
    state->buttons |= (data[off + 5] & BIT3) ? DS4_BTN_R2 : 0;
    state->buttons |= (data[off + 5] & BIT4) ? DS4_BTN_SHARE : 0;
    state->buttons |= (data[off + 5] & BIT5) ? DS4_BTN_OPTIONS : 0;
    state->buttons |= (data[off + 5] & BIT6) ? DS4_BTN_L3 : 0;
    state->buttons |= (data[off + 5] & BIT7) ? DS4_BTN_R3 : 0;
    state->buttons |= (data[off + 6] & BIT0) ? DS4_BTN_PS : 0;
    state->buttons |= (data[off + 6] & BIT1) ? DS4_BTN_TOUCHPAD : 0;
    state->l2 = data[off + 7];
    state->r2 = data[off + 8];
    state->valid = true;
    return true;
}

static bool ds4_state_changed_enough(const ds4_state_t *a, const ds4_state_t *b)
{
    const int threshold = 8;

    if (!a->valid || !b->valid) {
        return true;
    }
    if (a->buttons != b->buttons || a->dpad != b->dpad) {
        return true;
    }
    if (abs((int)a->lx - (int)b->lx) >= threshold ||
        abs((int)a->ly - (int)b->ly) >= threshold ||
        abs((int)a->rx - (int)b->rx) >= threshold ||
        abs((int)a->ry - (int)b->ry) >= threshold ||
        abs((int)a->l2 - (int)b->l2) >= threshold ||
        abs((int)a->r2 - (int)b->r2) >= threshold) {
        return true;
    }
    return false;
}

static void log_ds4_state(const ds4_state_t *state)
{
    char buttons[128];
    format_buttons(state->buttons, buttons, sizeof(buttons));
    ESP_LOGI(TAG, "DS4 lx=%3u ly=%3u rx=%3u ry=%3u l2=%3u r2=%3u dpad=%s buttons=%s",
             state->lx, state->ly, state->rx, state->ry, state->l2, state->r2,
             dpad_name(state->dpad), buttons);
}

static void update_public_state(hid_host_device_handle_t handle,
                                const hid_host_dev_params_t *dev_params,
                                const ds4_state_t *state)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_active_gamepad_handle == NULL) {
        s_active_gamepad_handle = handle;
        s_active_gamepad_addr = dev_params->addr;
        s_active_gamepad_iface = dev_params->iface_num;
    }
    s_public_state.connected = true;
    s_public_state.lx = state->lx;
    s_public_state.ly = state->ly;
    s_public_state.rx = state->rx;
    s_public_state.ry = state->ry;
    s_public_state.l2 = state->l2;
    s_public_state.r2 = state->r2;
    s_public_state.dpad = state->dpad;
    s_public_state.buttons = state->buttons;
    s_public_state.sequence++;
    taskEXIT_CRITICAL(&s_state_lock);
}

static bool is_active_gamepad_handle(hid_host_device_handle_t handle)
{
    bool active;

    taskENTER_CRITICAL(&s_state_lock);
    active = s_active_gamepad_handle == handle;
    taskEXIT_CRITICAL(&s_state_lock);
    return active;
}

static void mark_public_state_disconnected(hid_host_device_handle_t handle)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_active_gamepad_handle != handle) {
        taskEXIT_CRITICAL(&s_state_lock);
        return;
    }
    s_active_gamepad_handle = NULL;
    s_active_gamepad_addr = 0;
    s_active_gamepad_iface = 0;
    s_public_state.connected = false;
    s_public_state.buttons = 0;
    s_public_state.sequence++;
    taskEXIT_CRITICAL(&s_state_lock);
}

static void hid_gamepad_report_callback(hid_host_device_handle_t handle,
                                        const hid_host_dev_params_t *dev_params,
                                        const uint8_t *data,
                                        size_t len)
{
    static ds4_state_t prev = {};
    static uint32_t raw_dumped = 0;
    ds4_state_t state = {};

    /* Dump the first handful of raw reports so we can identify the layout of a
     * non-DualShock controller (the DS4 parser below may misread it). */
    if (raw_dumped < 12) {
        char report[3 * 32 + 1] = {};
        size_t bytes = len < 32 ? len : 32;
        for (size_t i = 0; i < bytes; i++) {
            char byte_text[4];
            snprintf(byte_text, sizeof(byte_text), "%02X ", data[i]);
            strlcat(report, byte_text, sizeof(report));
        }
        ESP_LOGI(TAG, "Raw HID report len=%u data=%s", (unsigned)len, report);
        raw_dumped++;
    }

    if (ds4_parse_usb_report(data, len, &state)) {
        bool was_active = is_active_gamepad_handle(handle);
        update_public_state(handle, dev_params, &state);
        if (!was_active) {
            ESP_LOGI(TAG, "Using DS4 report interface addr=%u iface=%u",
                     dev_params->addr, dev_params->iface_num);
        }
        if (ds4_state_changed_enough(&prev, &state)) {
            log_ds4_state(&state);
            prev = state;
        }
        return;
    }

    char report[3 * 32 + 1] = {};
    size_t bytes = len < 32 ? len : 32;
    for (size_t i = 0; i < bytes; i++) {
        char byte_text[4];
        snprintf(byte_text, sizeof(byte_text), "%02X ", data[i]);
        strlcat(report, byte_text, sizeof(report));
    }
    ESP_LOGI(TAG, "Generic HID report len=%u data=%s", (unsigned)len, report);
}

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                        const hid_host_interface_event_t event,
                                        void *arg)
{
    (void)arg;
    uint8_t data[128] = {};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;

    ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(hid_device_handle, data, sizeof(data), &data_length) == ESP_OK) {
            hid_gamepad_report_callback(hid_device_handle, &dev_params, data, data_length);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID device '%s' disconnected addr=%u iface=%u",
                 hid_proto_name(dev_params.proto), dev_params.addr, dev_params.iface_num);
        mark_public_state_disconnected(hid_device_handle);
        ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_close(hid_device_handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID device '%s' transfer error addr=%u iface=%u",
                 hid_proto_name(dev_params.proto), dev_params.addr, dev_params.iface_num);
        break;
    default:
        ESP_LOGW(TAG, "Unhandled HID interface event %d", event);
        break;
    }
}

static void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                                  const hid_host_driver_event_t event,
                                  void *arg)
{
    (void)arg;
    hid_host_dev_params_t dev_params;
    hid_host_dev_info_t dev_info = {};

    ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_get_params(hid_device_handle, &dev_params));

    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_get_device_info(hid_device_handle, &dev_info));
    ESP_LOGI(TAG, "HID device connected: vid=0x%04x pid=0x%04x addr=%u iface=%u proto=%s subclass=%u",
             dev_info.VID, dev_info.PID, dev_params.addr, dev_params.iface_num,
             hid_proto_name(dev_params.proto), dev_params.sub_class);

    const bool is_ds4 = is_ds4_device(dev_info.VID, dev_info.PID);
    /* Genuine DualShock 4: the gamepad is on interface 0 (others are audio/extra),
     * so keep using only iface 0. Any other device: accept generic-protocol HID
     * interfaces (proto == NONE) regardless of VID:PID, so third-party
     * "PS4/PS3/PC" pads are tried too. Keyboards/mice (BOOT protocols) are still
     * skipped. */
    if (is_ds4) {
        if (dev_params.iface_num != 0) {
            ESP_LOGI(TAG, "Ignoring DS4 non-gamepad HID interface %u", dev_params.iface_num);
            return;
        }
    } else if (dev_params.proto != HID_PROTOCOL_NONE) {
        ESP_LOGI(TAG, "Ignoring HID %s device vid=0x%04x pid=0x%04x",
                 hid_proto_name(dev_params.proto), dev_info.VID, dev_info.PID);
        return;
    } else {
        ESP_LOGI(TAG, "Trying generic HID gamepad vid=0x%04x pid=0x%04x iface=%u",
                 dev_info.VID, dev_info.PID, dev_params.iface_num);
    }

    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback,
        .callback_arg = NULL,
    };

    esp_err_t err = hid_host_device_open(hid_device_handle, &dev_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Ignoring HID device that could not be opened: %s", esp_err_to_name(err));
        return;
    }
    if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
        if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(hid_class_request_set_idle(hid_device_handle, 0, 0));
        }
    }
    err = hid_host_device_start(hid_device_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Ignoring HID device that could not be started: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_close(hid_device_handle));
    }
}

static void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                                     const hid_host_driver_event_t event,
                                     void *arg)
{
    const gamepad_event_t evt = {
        .event_group = GAMEPAD_EVENT_HID,
        .handle = hid_device_handle,
        .event = event,
        .arg = arg,
    };

    if (s_gamepad_event_queue != NULL) {
        xQueueSend(s_gamepad_event_queue, &evt, 0);
    }
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags = 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_device_free_all());
        }
    }
}

static void usb_gamepad_task(void *arg)
{
    (void)arg;
    gamepad_event_t evt;

    s_gamepad_event_queue = xQueueCreate(10, sizeof(gamepad_event_t));
    if (s_gamepad_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create gamepad event queue");
        vTaskDelete(NULL);
        return;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                                                      xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB event task");
        vTaskDelete(NULL);
        return;
    }
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_install(&hid_host_driver_config));
    ESP_LOGI(TAG, "USB HID gamepad host ready; plug in a wired controller");

    while (true) {
        if (xQueueReceive(s_gamepad_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.event_group == GAMEPAD_EVENT_HID) {
                hid_host_device_event(evt.handle, evt.event, evt.arg);
            }
        }
    }
}

esp_err_t usb_gamepad_start(void)
{
    BaseType_t created = xTaskCreatePinnedToCore(usb_gamepad_task, "usb_gamepad", 4096, NULL, 4, NULL, 0);
    return created == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

bool usb_gamepad_get_state(usb_gamepad_state_t *state)
{
    if (state == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_state_lock);
    *state = s_public_state;
    taskEXIT_CRITICAL(&s_state_lock);
    return true;
}
