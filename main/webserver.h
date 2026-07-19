#pragma once

#include "esp_err.h"

// Starts esp_http_server on :80 and mDNS as http://androidtv-remote.local/.
// Call once WiFi has an IP. Status/pairing state lives in app_state.h.
esp_err_t webserver_start(void);
