#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Captures everything written through ESP_LOG* into a small in-RAM ring
// buffer, so the web UI can show live firmware logs without a serial
// monitor. Installed as an esp_log vprintf hook; still forwards to the
// original hook (UART) so `pio run -t monitor` keeps working unchanged.

// Call once, as early as possible in app_main (before nvs_flash_init) so
// boot-time logs are captured too.
void logbuf_init(void);

// Current write position (byte offset into the infinite log stream). Pass
// as `after` on the first poll to only receive logs from "now" onward.
uint32_t logbuf_head(void);

// Copies log bytes newer than `after` into out (NUL-terminated if room
// allows within out_size). Returns the number of bytes copied (excluding
// NUL). *next_pos is the offset to pass as `after` next time. *dropped is
// set true if the ring wrapped past `after` before this read, i.e. some
// log lines were lost.
size_t logbuf_read(uint32_t after, char *out, size_t out_size,
                    uint32_t *next_pos, bool *dropped);
