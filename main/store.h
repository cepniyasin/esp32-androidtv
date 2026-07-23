#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

bool store_get_paired(void);
esp_err_t store_set_paired(bool paired);

// Samsung TV WebSocket pairing token (see samsung_tv.c). Empty string if
// never paired. out must be at least SAMSUNG_TOKEN_MAX bytes.
#define SAMSUNG_TOKEN_MAX 64
esp_err_t store_get_samsung_token(char *out, size_t out_size);
esp_err_t store_set_samsung_token(const char *token);

// App shortcut config, persisted as a single serialized blob under one NVS
// key (see main/shortcuts.c). The serialized AppShortcuts message tops out
// around 1660 bytes; this leaves room plus the 1-byte format marker, and
// stays far under the ~4000-byte NVS blob-entry cap.
#define SHORTCUTS_BLOB_MAX 1792
// Returns ESP_ERR_NVS_NOT_FOUND if nothing has been saved yet (so callers
// can fall back to built-in defaults); *out_len is the byte count on ESP_OK.
esp_err_t store_get_shortcuts_blob(uint8_t *buf, size_t buf_size, size_t *out_len);
esp_err_t store_set_shortcuts_blob(const uint8_t *buf, size_t len);
