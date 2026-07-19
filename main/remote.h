#pragma once

#include "esp_err.h"
#include "net_tls.h"

// Runs one control-channel session (port 6466) on an established TLS
// connection: RemoteConfigure/SetActive exchange, ping/pong keepalive,
// and key commands from g_key_queue. Returns when the connection drops
// or goes idle too long (caller reconnects).
esp_err_t remote_session(atv_tls_t *tls);

// Maps a key name from TvKeys.txt (e.g. "DPAD_UP", with or without the
// KEYCODE_ prefix) to its RemoteKeyCode value, or -1 if unknown.
int remote_key_from_name(const char *name);

// Maps "SHORT"/"START_LONG"/"END_LONG" to RemoteDirection, or -1.
int remote_direction_from_name(const char *name);
