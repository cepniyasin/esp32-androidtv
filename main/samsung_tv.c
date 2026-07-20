#include "samsung_tv.h"

#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "store.h"

static const char *TAG = "samsung_tv";

// base64("ESP32 Remote") — the name shown on the TV's pairing prompt.
// Fixed and precomputed rather than encoded at runtime: no other value is
// ever needed, so pulling in a base64 encoder for one constant isn't
// worth it.
#define APP_NAME_B64 "RVNQMzIgUmVtb3Rl"

static esp_websocket_client_handle_t s_client;
static char s_uri[192];

static void handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        return;
    }
    cJSON *event = cJSON_GetObjectItem(root, "event");
    if (cJSON_IsString(event) && strcmp(event->valuestring, "ms.channel.unauthorized") == 0) {
        ESP_LOGW(TAG, "Samsung TV: pairing not authorized (rejected or timed out on the TV)");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "ms.channel.connect") == 0) {
        cJSON *data_obj = cJSON_GetObjectItem(root, "data");
        cJSON *token = cJSON_GetObjectItem(data_obj, "token");
        if (cJSON_IsString(token) && token->valuestring[0] != '\0') {
            store_set_samsung_token(token->valuestring);
            ESP_LOGI(TAG, "Samsung TV: paired, token saved");
        } else {
            ESP_LOGI(TAG, "Samsung TV: channel connected (no new token)");
        }
        g_samsung_status.connected = true;
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Samsung TV: WebSocket connected; waiting for channel handshake "
                      "(approve the prompt on the TV if this is the first pairing)");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Samsung TV: WebSocket disconnected");
        g_samsung_status.connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        // op_code 1 = text frame; ping/pong/close frames carry no JSON.
        if (data->op_code == 1 && data->data_len > 0) {
            handle_message(data->data_ptr, data->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Samsung TV: WebSocket error event");
        break;
    default:
        break;
    }
}

void samsung_tv_init(void)
{
    if (CONFIG_ATV_SAMSUNG_TV_IP[0] == '\0') {
        ESP_LOGI(TAG, "Samsung TV IP not configured; volume fallback disabled");
        return;
    }

    char token[SAMSUNG_TOKEN_MAX] = {0};
    store_get_samsung_token(token, sizeof(token));

    // Plain ws://:8001 gets an instant "ms.channel.unauthorized" with no
    // on-screen prompt on this TV (2020 Tizen firmware) — confirmed on
    // hardware. wss://:8002 is what actually triggers the pairing prompt;
    // the TV's cert is self-signed, so verification is skipped the same
    // way net_tls.c skips it for the Chromecast's cert.
    int n = snprintf(s_uri, sizeof(s_uri),
                     "wss://%s:8002/api/v2/channels/samsung.remote.control?name=" APP_NAME_B64,
                     CONFIG_ATV_SAMSUNG_TV_IP);
    if (token[0] != '\0' && n > 0 && (size_t)n < sizeof(s_uri)) {
        snprintf(s_uri + n, sizeof(s_uri) - (size_t)n, "&token=%s", token);
    }

    esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_CONNECTED, ws_event_handler, NULL);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_DISCONNECTED, ws_event_handler, NULL);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_DATA, ws_event_handler, NULL);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ERROR, ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Samsung TV: connecting to %s", s_uri);
}

bool samsung_tv_send_key(const char *key_name)
{
    if (s_client == NULL || !g_samsung_status.connected) {
        return false;
    }
    char msg[192];
    int n = snprintf(msg, sizeof(msg),
                     "{\"method\":\"ms.remote.control\",\"params\":{\"Cmd\":\"Click\","
                     "\"DataOfCmd\":\"%s\",\"Option\":\"false\",\"TypeOfRemote\":\"SendRemoteKey\"}}",
                     key_name);
    if (n <= 0 || (size_t)n >= sizeof(msg)) {
        return false;
    }
    return esp_websocket_client_send_text(s_client, msg, n, pdMS_TO_TICKS(1000)) >= 0;
}
