#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

bool store_get_paired(void);
esp_err_t store_set_paired(bool paired);

// Samsung TV WebSocket pairing token (see samsung_tv.c). Empty string if
// never paired. out must be at least SAMSUNG_TOKEN_MAX bytes.
#define SAMSUNG_TOKEN_MAX 64
esp_err_t store_get_samsung_token(char *out, size_t out_size);
esp_err_t store_set_samsung_token(const char *token);
