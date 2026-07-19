#pragma once

#include "esp_err.h"

// Connects to the WiFi network configured via menuconfig (blocking).
// Returns ESP_OK once an IP address has been acquired.
esp_err_t wifi_connect(void);
