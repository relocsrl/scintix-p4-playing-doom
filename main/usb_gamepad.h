#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

enum {
    USB_GAMEPAD_BUTTON_SQUARE = 1U << 0,
    USB_GAMEPAD_BUTTON_CROSS = 1U << 1,
    USB_GAMEPAD_BUTTON_CIRCLE = 1U << 2,
    USB_GAMEPAD_BUTTON_TRIANGLE = 1U << 3,
    USB_GAMEPAD_BUTTON_L1 = 1U << 4,
    USB_GAMEPAD_BUTTON_R1 = 1U << 5,
    USB_GAMEPAD_BUTTON_L2 = 1U << 6,
    USB_GAMEPAD_BUTTON_R2 = 1U << 7,
    USB_GAMEPAD_BUTTON_SHARE = 1U << 8,
    USB_GAMEPAD_BUTTON_OPTIONS = 1U << 9,
    USB_GAMEPAD_BUTTON_L3 = 1U << 10,
    USB_GAMEPAD_BUTTON_R3 = 1U << 11,
    USB_GAMEPAD_BUTTON_PS = 1U << 12,
    USB_GAMEPAD_BUTTON_TOUCHPAD = 1U << 13,
};

typedef struct {
    bool connected;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t l2;
    uint8_t r2;
    uint8_t dpad;
    uint16_t buttons;
    uint32_t sequence;
} usb_gamepad_state_t;

/* Boot-protocol keyboard snapshot: modifier byte + up to 6 simultaneously
 * pressed HID usage codes (standard 8-byte boot report layout). */
typedef struct {
    bool connected;
    uint8_t modifiers;
    uint8_t keys[6];
    uint32_t sequence;
} usb_keyboard_state_t;

esp_err_t usb_gamepad_start(void);
bool usb_gamepad_get_state(usb_gamepad_state_t *state);
bool usb_keyboard_get_state(usb_keyboard_state_t *state);
