#include "webserver.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "logbuf.h"
#include "mdns.h"
#include "remote.h"
#include "samsung_tv.h"

static const char *TAG = "webserver";

// web/index.html embedded via board_build.embed_txtfiles (null-terminated).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
// PWA icons embedded via board_build.embed_files (binary, no terminator).
extern const uint8_t icon192_start[] asm("_binary_icon_192_png_start");
extern const uint8_t icon192_end[] asm("_binary_icon_192_png_end");
extern const uint8_t icon512_start[] asm("_binary_icon_512_png_start");
extern const uint8_t icon512_end[] asm("_binary_icon_512_png_end");
extern const uint8_t apple_icon_start[] asm("_binary_apple_touch_icon_png_start");
extern const uint8_t apple_icon_end[] asm("_binary_apple_touch_icon_png_end");

static const char MANIFEST_JSON[] =
    "{"
    "\"name\":\"TV Remote\","
    "\"short_name\":\"TV Remote\","
    "\"start_url\":\"/\","
    "\"display\":\"standalone\","
    "\"background_color\":\"#0c0f13\","
    "\"theme_color\":\"#101418\","
    "\"icons\":["
    "{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"},"
    "{\"src\":\"/icon-512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\"}"
    "]"
    "}";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    // The page is versioned by firmware, not by URL; stale cached JS after
    // a reflash has already caused confusing bugs.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // -1: skip the null terminator appended by the embedder
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t manifest_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/manifest+json");
    return httpd_resp_send(req, MANIFEST_JSON, sizeof(MANIFEST_JSON) - 1);
}

static esp_err_t png_handler(httpd_req_t *req)
{
    const uint8_t *start = req->user_ctx == (void *)1   ? icon192_start
                           : req->user_ctx == (void *)2 ? icon512_start
                                                        : apple_icon_start;
    const uint8_t *end = req->user_ctx == (void *)1   ? icon192_end
                         : req->user_ctx == (void *)2 ? icon512_end
                                                      : apple_icon_end;
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char body[200];
    int len = snprintf(body, sizeof(body),
                       "{\"paired\":%s,\"connected\":%s,\"state\":\"%s\","
                       "\"volume\":{\"level\":%d,\"max\":%d,\"muted\":%s},"
                       "\"samsung\":{\"connected\":%s}}",
                       g_atv_status.paired ? "true" : "false",
                       g_atv_status.connected ? "true" : "false",
                       atv_state_str(g_atv_status.state),
                       g_atv_status.vol_level, g_atv_status.vol_max,
                       g_atv_status.vol_muted ? "true" : "false",
                       g_samsung_status.connected ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

// Accepts {"code":"2C42AF"} (or any body containing 6 hex chars), hands the
// code to the TV-session task, and waits for the pairing outcome.
static esp_err_t pair_post_handler(httpd_req_t *req)
{
    char body[64] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    char code[PAIR_CODE_LEN + 1] = {0};
    int n = 0;
    for (int i = 0; body[i] != '\0' && n <= PAIR_CODE_LEN; i++) {
        if (isxdigit((unsigned char)body[i])) {
            if (n == PAIR_CODE_LEN) {
                n++;  // too many hex chars in a row -> not a 6-char code
                break;
            }
            code[n++] = (char)toupper((unsigned char)body[i]);
        } else if (n > 0 && n < PAIR_CODE_LEN) {
            n = 0;  // hex run too short; keep scanning
        }
    }
    if (n != PAIR_CODE_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need a 6-hex-digit code");
        return ESP_FAIL;
    }

    if (g_atv_status.state != ATV_STATE_WAIT_CODE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not waiting for a code");
        return ESP_FAIL;
    }

    xEventGroupClearBits(g_pair_events, PAIR_EVENT_OK | PAIR_EVENT_FAIL);
    xQueueOverwrite(g_pair_code_queue, code);
    EventBits_t bits = xEventGroupWaitBits(g_pair_events,
                                           PAIR_EVENT_OK | PAIR_EVENT_FAIL,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));
    httpd_resp_set_type(req, "application/json");
    if (bits & PAIR_EVENT_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"pairing failed - check the code\"}");
}

// Pulls the quoted value of a JSON string field out of body without a
// parser: finds `"field"`, the colon after it, then the next quoted token.
static bool json_get_str(const char *body, const char *field,
                         char *out, size_t out_size)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *p = strstr(body, pattern);
    if (p != NULL) {
        p = strchr(p + strlen(pattern), ':');
    }
    if (p != NULL) {
        p = strchr(p, '"');
    }
    if (p == NULL) {
        return false;
    }
    const char *end = strchr(p + 1, '"');
    if (end == NULL || (size_t)(end - p - 1) >= out_size) {
        return false;
    }
    memcpy(out, p + 1, (size_t)(end - p - 1));
    out[end - p - 1] = '\0';
    return true;
}

// Volume keys the Chromecast can't act on (volume_max == 0, see CLAUDE.md
// "Samsung TV fallback (volume)") get rerouted to a Samsung TV's own local
// WebSocket remote API instead. Returns NULL for any other key name.
static const char *samsung_key_name(const char *atv_key_name)
{
    if (strcmp(atv_key_name, "VOLUME_UP") == 0) return "KEY_VOLUP";
    if (strcmp(atv_key_name, "VOLUME_DOWN") == 0) return "KEY_VOLDOWN";
    if (strcmp(atv_key_name, "VOLUME_MUTE") == 0) return "KEY_MUTE";
    return NULL;
}

// Accepts {"key":"DPAD_UP","direction":"START_LONG"} (any TvKeys.txt name;
// direction optional, default SHORT); queues it for the TV-session task.
static esp_err_t key_post_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    char name[40] = {0};
    key_cmd_t cmd = {.code = -1, .direction = remote_direction_from_name("SHORT")};
    if (json_get_str(body, "key", name, sizeof(name))) {
        cmd.code = remote_key_from_name(name);
    }
    char dir[16] = {0};
    if (json_get_str(body, "direction", dir, sizeof(dir))) {
        cmd.direction = remote_direction_from_name(dir);
    }
    if (cmd.code < 0 || cmd.direction < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown key or direction");
        return ESP_FAIL;
    }

    const char *samsung_name = samsung_key_name(name);
    if (samsung_name != NULL && g_atv_status.vol_max == 0 && g_samsung_status.connected) {
        // Samsung's remote API is click-only: a tap or the start of a hold
        // both become one Click; the matching release is a no-op (no
        // repeat-while-held on this path).
        bool ok = true;
        if (cmd.direction != remote_direction_from_name("END_LONG")) {
            ok = samsung_tv_send_key(samsung_name);
        }
        httpd_resp_set_type(req, "application/json");
        if (!ok) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "samsung send failed");
            return ESP_FAIL;
        }
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    if (!g_atv_status.connected) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not connected to the TV");
        return ESP_FAIL;
    }
    if (xQueueSend(g_key_queue, &cmd, 0) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "key queue full");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Accepts {"link":"netflix://home"}; queues an app launch.
static esp_err_t app_post_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    char link[APP_LINK_MAX] = {0};
    if (!json_get_str(body, "link", link, sizeof(link)) || link[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing link");
        return ESP_FAIL;
    }
    if (!g_atv_status.connected) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not connected to the TV");
        return ESP_FAIL;
    }
    if (xQueueSend(g_app_queue, link, 0) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "queue full");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Returns firmware log lines newer than ?after=<offset> as JSON:
// {"next":<offset to poll from next>,"dropped":<bool, ring wrapped past
// what the client had>,"text":"<escaped log text>"}. Poll with `after`
// omitted/0 on first load (returns whatever's still in the ring), then
// with `after=next` from the previous response.
static esp_err_t logs_get_handler(httpd_req_t *req)
{
    uint32_t after = 0;
    char query[32];
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "after", val, sizeof(val)) == ESP_OK) {
            after = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    // Static: esp_http_server processes one request at a time on its
    // single worker task, so this is not shared across concurrent clients.
    static char raw[1536];
    uint32_t next_pos;
    bool dropped;
    size_t n = logbuf_read(after, raw, sizeof(raw), &next_pos, &dropped);

    httpd_resp_set_type(req, "application/json");
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        "{\"next\":%lu,\"dropped\":%s,\"text\":\"",
                        (unsigned long)next_pos, dropped ? "true" : "false");
    httpd_resp_send_chunk(req, header, hlen);

    char esc[256];
    size_t ei = 0;
    for (size_t i = 0; i < n; i++) {
        char c = raw[i];
        char repbuf[2] = {c, '\0'};
        const char *rep = repbuf;
        if (c == '"') {
            rep = "\\\"";
        } else if (c == '\\') {
            rep = "\\\\";
        } else if (c == '\n') {
            rep = "\\n";
        } else if (c == '\r') {
            rep = "\\r";
        } else if (c == '\t') {
            rep = "\\t";
        } else if ((unsigned char)c < 0x20) {
            continue;  // drop other control chars
        }
        size_t rl = strlen(rep);
        if (ei + rl > sizeof(esc)) {
            httpd_resp_send_chunk(req, esc, ei);
            ei = 0;
        }
        memcpy(esc + ei, rep, rl);
        ei += rl;
    }
    if (ei > 0) {
        httpd_resp_send_chunk(req, esc, ei);
    }
    httpd_resp_send_chunk(req, "\"}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("androidtv-remote"));
    ESP_ERROR_CHECK(mdns_instance_name_set("ESP32 Android TV Remote"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    return ESP_OK;
}

esp_err_t webserver_start(void)
{
    ESP_ERROR_CHECK(mdns_start());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Phones hold several keep-alive connections; without LRU purge the
    // server starts refusing new requests, which looks like dropouts.
    config.lru_purge_enable = true;
    // Default is 8; we register 10 (page, manifest, 3 icons, 5 API).
    config.max_uri_handlers = 12;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_get_handler
    };
    static const httpd_uri_t status_uri = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler
    };
    static const httpd_uri_t pair_uri = {
        .uri = "/api/pair", .method = HTTP_POST, .handler = pair_post_handler
    };
    static const httpd_uri_t key_uri = {
        .uri = "/api/key", .method = HTTP_POST, .handler = key_post_handler
    };
    static const httpd_uri_t app_uri = {
        .uri = "/api/app", .method = HTTP_POST, .handler = app_post_handler
    };
    static const httpd_uri_t logs_uri = {
        .uri = "/api/logs", .method = HTTP_GET, .handler = logs_get_handler
    };
    static const httpd_uri_t manifest_uri = {
        .uri = "/manifest.webmanifest", .method = HTTP_GET, .handler = manifest_get_handler
    };
    static const httpd_uri_t icon192_uri = {
        .uri = "/icon-192.png", .method = HTTP_GET, .handler = png_handler, .user_ctx = (void *)1
    };
    static const httpd_uri_t icon512_uri = {
        .uri = "/icon-512.png", .method = HTTP_GET, .handler = png_handler, .user_ctx = (void *)2
    };
    static const httpd_uri_t apple_icon_uri = {
        .uri = "/apple-touch-icon.png", .method = HTTP_GET, .handler = png_handler, .user_ctx = (void *)3
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &manifest_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &icon192_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &icon512_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &apple_icon_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &pair_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &key_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &app_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &logs_uri));

    ESP_LOGI(TAG, "Web UI at http://androidtv-remote.local/ (port 80)");
    return ESP_OK;
}
