#pragma once

#include <stdbool.h>

#include "esp_err.h"

bool store_get_paired(void);
esp_err_t store_set_paired(bool paired);
