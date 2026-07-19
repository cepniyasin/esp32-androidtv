#include "webserver.h"

#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "webserver";

atv_status_t g_atv_status;

// web/index.html embedded via board_build.embed_txtfiles (null-terminated).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    // -1: skip the null terminator appended by the embedder
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char body[64];
    int len = snprintf(body, sizeof(body), "{\"paired\":%s,\"connected\":%s}",
                       g_atv_status.paired ? "true" : "false",
                       g_atv_status.connected ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));

    ESP_LOGI(TAG, "Web UI at http://androidtv-remote.local/ (port 80)");
    return ESP_OK;
}
