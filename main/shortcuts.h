#pragma once

#include "esp_err.h"
#include "appshortcuts.pb.h"

// App shortcut row config: a data-driven list of {label, app_id} entries
// the web UI renders as launch buttons. app_id is the deep-link / intent
// handed to RemoteAppLinkLaunchRequest (e.g. "netflix://home"). The list is
// serialized with nanopb and persisted as a single NVS blob (see store.c),
// so it survives reboots and OTA updates.

// Cap on the number of shortcuts, matching the nanopb max_count in
// appshortcuts.options. Label/app_id length limits are the array sizes in
// the generated AppShortcut struct (sizeof(items[i].label / .app_id)).
#define SHORTCUTS_MAX 10

// Load the persisted shortcut list into *out. If nothing has been saved yet
// (fresh device / erased NVS), or the stored blob can't be decoded, *out is
// filled with the built-in defaults so the row still renders out of the box.
// An explicitly-saved empty list is preserved (not replaced with defaults).
void shortcuts_load(AppShortcuts *out);

// Validate and persist a shortcut list as a single NVS blob (one flash
// write). Returns ESP_ERR_INVALID_ARG if the list violates the caps (too
// many entries, or an entry with an empty/overlong label or app_id).
esp_err_t shortcuts_save(const AppShortcuts *in);
