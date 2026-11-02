#pragma once

#include "esp_err.h"

/* Read-only copy of the WAD in PSRAM, served as WAD_MEM_BASE "/<name>".
 * Doom reads hundreds of scattered lumps at startup and SPIFFS seeks inside a
 * 4 MB file are slow (~100 KB/s, ~38 s of black screen); one sequential copy
 * plus RAM reads is much faster. */
#define WAD_MEM_BASE "/wadmem"

esp_err_t wad_mem_load(const char *src_path, const char *name);
