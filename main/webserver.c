#include "webserver.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"
#include "remote.h"

static const char *TAG = "webserver";

// web/index.html embedded via board_build.embed_txtfiles (null-terminated).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

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

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char body[96];
    int len = snprintf(body, sizeof(body),
                       "{\"paired\":%s,\"connected\":%s,\"state\":\"%s\"}",
                       g_atv_status.paired ? "true" : "false",
                       g_atv_status.connected ? "true" : "false",
                       atv_state_str(g_atv_status.state));
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &pair_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &key_uri));

    ESP_LOGI(TAG, "Web UI at http://androidtv-remote.local/ (port 80)");
    return ESP_OK;
}
