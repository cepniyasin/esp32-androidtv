#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Shared state reflected by GET /api/status. Written by the TV-session
// task, read by HTTP handlers (single-word writes, no locking needed).
typedef struct {
    volatile bool paired;
    volatile bool connected;
} atv_status_t;

extern atv_status_t g_atv_status;

// Starts esp_http_server on :80 and mDNS as http://androidtv-remote.local/.
// Call once WiFi has an IP.
esp_err_t webserver_start(void);
