#include "remote.h"

#include <string.h>

#include "app_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pb_decode.h"
#include "proto_frame.h"
#include "remotemessage.pb.h"

static const char *TAG = "remote";

// Feature bits from remote.py (Feature enum); we advertise everything we
// support and AND it with what the device reports in remote_configure.
#define FEAT_PING 1
#define FEAT_KEY 2
#define FEAT_POWER 32
#define FEAT_VOLUME 64
#define FEAT_APP_LINK 512
#define FEATURES_DEFAULT (FEAT_PING | FEAT_KEY | FEAT_POWER | FEAT_VOLUME | FEAT_APP_LINK)

// Server pings every ~5 s; reference treats 16 s of silence as dead.
#define IDLE_DISCONNECT_MS 16000
// Poll granularity for the key queue while waiting for TV messages.
#define POLL_TIMEOUT_MS 200

static const struct {
    const char *name;
    remote_RemoteKeyCode code;
} key_map[] = {
    {"DPAD_UP", remote_RemoteKeyCode_KEYCODE_DPAD_UP},
    {"DPAD_DOWN", remote_RemoteKeyCode_KEYCODE_DPAD_DOWN},
    {"DPAD_LEFT", remote_RemoteKeyCode_KEYCODE_DPAD_LEFT},
    {"DPAD_RIGHT", remote_RemoteKeyCode_KEYCODE_DPAD_RIGHT},
    {"DPAD_CENTER", remote_RemoteKeyCode_KEYCODE_DPAD_CENTER},
    {"BACK", remote_RemoteKeyCode_KEYCODE_BACK},
    {"HOME", remote_RemoteKeyCode_KEYCODE_HOME},
    {"POWER", remote_RemoteKeyCode_KEYCODE_POWER},
    {"VOLUME_UP", remote_RemoteKeyCode_KEYCODE_VOLUME_UP},
    {"VOLUME_DOWN", remote_RemoteKeyCode_KEYCODE_VOLUME_DOWN},
    {"VOLUME_MUTE", remote_RemoteKeyCode_KEYCODE_VOLUME_MUTE},
    {"MEDIA_PLAY_PAUSE", remote_RemoteKeyCode_KEYCODE_MEDIA_PLAY_PAUSE},
    {"SEARCH", remote_RemoteKeyCode_KEYCODE_SEARCH},
};

int remote_key_from_name(const char *name)
{
    if (strncmp(name, "KEYCODE_", 8) == 0) {
        name += 8;
    }
    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
        if (strcmp(name, key_map[i].name) == 0) {
            return (int)key_map[i].code;
        }
    }
    return -1;
}

int remote_direction_from_name(const char *name)
{
    if (strcmp(name, "SHORT") == 0) return remote_RemoteDirection_SHORT;
    if (strcmp(name, "START_LONG") == 0) return remote_RemoteDirection_START_LONG;
    if (strcmp(name, "END_LONG") == 0) return remote_RemoteDirection_END_LONG;
    return -1;
}

// Reads for frame_recv once a frame has started: generous timeout, and
// hands back the first byte grabbed by the poll loop.
typedef struct {
    atv_tls_t *tls;
    bool have_first;
    uint8_t first;
} rx_ctx_t;

static int rx_read(void *ctx, uint8_t *buf, size_t len)
{
    rx_ctx_t *rx = ctx;
    if (rx->have_first) {
        rx->have_first = false;
        buf[0] = rx->first;
        return 1;
    }
    (void)len;
    return atv_tls_read(rx->tls, buf, len, 5000);
}

static int tls_write_adapter(void *ctx, const uint8_t *buf, size_t len)
{
    return atv_tls_write(ctx, buf, len);
}

static esp_err_t send_msg(atv_tls_t *tls, const remote_RemoteMessage *m)
{
    int r = frame_send(remote_RemoteMessage_fields, m, tls_write_adapter, tls);
    if (r != 0) {
        ESP_LOGE(TAG, "send failed (%d)", r);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_key(atv_tls_t *tls, const key_cmd_t *cmd)
{
    remote_RemoteMessage m = remote_RemoteMessage_init_zero;
    m.has_remote_key_inject = true;
    m.remote_key_inject.key_code = (remote_RemoteKeyCode)cmd->code;
    m.remote_key_inject.direction = (remote_RemoteDirection)cmd->direction;
    return send_msg(tls, &m);
}

// Handles one message from the TV; fills reply and returns true if a reply
// should be sent. active_features is ANDed with the device's on configure.
static bool handle_msg(const remote_RemoteMessage *in, remote_RemoteMessage *out,
                       int *active_features)
{
    if (in->has_remote_configure) {
        *active_features &= in->remote_configure.code1 ? in->remote_configure.code1
                                                       : FEATURES_DEFAULT;
        ESP_LOGI(TAG, "remote_configure from %s %s (features 0x%x)",
                 in->remote_configure.device_info.vendor,
                 in->remote_configure.device_info.model, (unsigned)*active_features);
        out->has_remote_configure = true;
        out->remote_configure.code1 = *active_features;
        out->remote_configure.has_device_info = true;
        out->remote_configure.device_info.unknown1 = 1;
        strcpy(out->remote_configure.device_info.unknown2, "1");
        strcpy(out->remote_configure.device_info.package_name, "atvremote");
        strcpy(out->remote_configure.device_info.app_version, "1.0.0");
        return true;
    }
    if (in->has_remote_set_active) {
        out->has_remote_set_active = true;
        out->remote_set_active.active = *active_features;
        return true;
    }
    if (in->has_remote_ping_request) {
        out->has_remote_ping_response = true;
        out->remote_ping_response.val1 = in->remote_ping_request.val1;
        return true;
    }
    if (in->has_remote_start) {
        ESP_LOGI(TAG, "remote_start (device is %s) — control channel ready",
                 in->remote_start.started ? "on" : "off");
        g_atv_status.connected = true;
        g_atv_status.state = ATV_STATE_CONNECTED;
        return false;
    }
    if (in->has_remote_set_volume_level) {
        ESP_LOGI(TAG, "volume %d/%d%s", (int)in->remote_set_volume_level.volume_level,
                 (int)in->remote_set_volume_level.volume_max,
                 in->remote_set_volume_level.volume_muted ? " (muted)" : "");
        return false;
    }
    return false;
}

esp_err_t remote_session(atv_tls_t *tls)
{
    int active_features = FEATURES_DEFAULT;
    int64_t last_rx_us = esp_timer_get_time();
    uint8_t buf[FRAME_MAX_MSG];

    while (true) {
        // Poll for the first byte of a frame so the key queue stays serviced.
        uint8_t first;
        int r = atv_tls_read(tls, &first, 1, POLL_TIMEOUT_MS);
        if (r == MBEDTLS_ERR_SSL_TIMEOUT) {
            key_cmd_t cmd;
            while (g_key_queue && xQueueReceive(g_key_queue, &cmd, 0) == pdTRUE) {
                if (send_key(tls, &cmd) != ESP_OK) {
                    return ESP_FAIL;
                }
            }
            if ((esp_timer_get_time() - last_rx_us) / 1000 > IDLE_DISCONNECT_MS) {
                ESP_LOGW(TAG, "No traffic for %d ms; reconnecting", IDLE_DISCONNECT_MS);
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (r <= 0) {
            ESP_LOGW(TAG, "Connection closed/error (%d)", r);
            return ESP_FAIL;
        }

        rx_ctx_t rx = {.tls = tls, .have_first = true, .first = first};
        int len = frame_recv(buf, sizeof(buf), rx_read, &rx);
        if (len < 0) {
            ESP_LOGW(TAG, "frame_recv failed (%d)", len);
            return ESP_FAIL;
        }
        last_rx_us = esp_timer_get_time();

        remote_RemoteMessage in = remote_RemoteMessage_init_zero;
        pb_istream_t is = pb_istream_from_buffer(buf, (size_t)len);
        if (!pb_decode(&is, remote_RemoteMessage_fields, &in)) {
            ESP_LOGW(TAG, "decode failed: %s (len %d) — skipping", PB_GET_ERROR(&is), len);
            continue;
        }
        remote_RemoteMessage out = remote_RemoteMessage_init_zero;
        if (handle_msg(&in, &out, &active_features)) {
            if (send_msg(tls, &out) != ESP_OK) {
                return ESP_FAIL;
            }
        }
    }
}
