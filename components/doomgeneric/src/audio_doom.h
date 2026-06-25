#pragma once

#include <stdint.h>

#include "esp_err.h"

#define DOOM_AUDIO_DEFAULT_VOLUME 85
#define DOOM_AUDIO_MAX_VOLUME 100

uint8_t doom_audio_get_volume(void);
void doom_audio_set_volume(uint8_t volume);
esp_err_t doom_audio_load_settings(void);
esp_err_t doom_audio_save_settings(uint8_t volume);
