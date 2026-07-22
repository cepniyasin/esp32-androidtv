#include "shortcuts.h"

#include <string.h>

#include "esp_log.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "store.h"

static const char *TAG = "shortcuts";

// The stored blob is a 1-byte format marker followed by the serialized
// AppShortcuts message. The marker distinguishes an explicitly-saved empty
// list (marker present, zero-length payload) from "never saved" (key
// absent), and leaves room to change the on-flash format later.
#define SHORTCUTS_FORMAT_V1 1

// Built-in defaults, used until the user saves their own list. Preserves
// the row that used to be hardcoded in the web UI (Netflix + YouTube).
static void load_defaults(AppShortcuts *out)
{
    *out = (AppShortcuts)AppShortcuts_init_zero;
    out->items_count = 2;
    strlcpy(out->items[0].label, "Netflix", sizeof(out->items[0].label));
    strlcpy(out->items[0].app_id, "netflix://home", sizeof(out->items[0].app_id));
    strlcpy(out->items[1].label, "YouTube", sizeof(out->items[1].label));
    strlcpy(out->items[1].app_id, "https://www.youtube.com/tv", sizeof(out->items[1].app_id));
}

void shortcuts_load(AppShortcuts *out)
{
    *out = (AppShortcuts)AppShortcuts_init_zero;

    uint8_t blob[SHORTCUTS_BLOB_MAX];
    size_t len = 0;
    esp_err_t err = store_get_shortcuts_blob(blob, sizeof(blob), &len);
    if (err != ESP_OK || len < 1 || blob[0] != SHORTCUTS_FORMAT_V1) {
        load_defaults(out);  // nothing saved (or unknown format) -> built-in row
        return;
    }

    pb_istream_t is = pb_istream_from_buffer(blob + 1, len - 1);
    if (!pb_decode(&is, AppShortcuts_fields, out)) {
        ESP_LOGW(TAG, "decode failed (%s); using defaults", PB_GET_ERROR(&is));
        load_defaults(out);
    }
}

// Rejects a list that would exceed the caps or that carries an empty or
// non-NUL-terminated (i.e. overlong) label/app_id — the same guards the web
// handler applies, enforced here too so no bad list can reach flash.
static bool valid(const AppShortcuts *in)
{
    if (in->items_count > SHORTCUTS_MAX) {
        return false;
    }
    for (pb_size_t i = 0; i < in->items_count; i++) {
        size_t label_len = strnlen(in->items[i].label, sizeof(in->items[i].label));
        size_t id_len = strnlen(in->items[i].app_id, sizeof(in->items[i].app_id));
        if (label_len == 0 || label_len >= sizeof(in->items[i].label)) {
            return false;
        }
        if (id_len == 0 || id_len >= sizeof(in->items[i].app_id)) {
            return false;
        }
    }
    return true;
}

esp_err_t shortcuts_save(const AppShortcuts *in)
{
    if (!valid(in)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t blob[SHORTCUTS_BLOB_MAX];
    blob[0] = SHORTCUTS_FORMAT_V1;
    pb_ostream_t os = pb_ostream_from_buffer(blob + 1, sizeof(blob) - 1);
    if (!pb_encode(&os, AppShortcuts_fields, in)) {
        ESP_LOGE(TAG, "encode failed: %s", PB_GET_ERROR(&os));
        return ESP_FAIL;
    }
    return store_set_shortcuts_blob(blob, os.bytes_written + 1);
}
