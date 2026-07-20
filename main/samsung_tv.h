#pragma once

#include <stdbool.h>

// Fallback volume control that talks directly to a Samsung Tizen TV over
// its own local WebSocket remote API, used only because the Chromecast
// reports it cannot control volume on this hardware (see CLAUDE.md,
// "Samsung TV fallback (volume)"). No-ops entirely if
// CONFIG_ATV_SAMSUNG_TV_IP is empty.

// Starts the (auto-reconnecting) WebSocket connection. Call once, after
// wifi_connect() has brought up the default event loop.
void samsung_tv_init(void);

// Sends one Click of key_name (e.g. "KEY_VOLUP", "KEY_VOLDOWN",
// "KEY_MUTE"). Returns false if not currently connected.
bool samsung_tv_send_key(const char *key_name);
