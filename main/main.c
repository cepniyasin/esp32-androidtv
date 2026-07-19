#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_tls.h"
#include "nvs_flash.h"
#include "webserver.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "polo.pb.h"
#include "remotemessage.pb.h"
#include "wifi.h"

static const char *TAG = "main";

// Boot-time sanity check that the generated nanopb code round-trips a
// pairing_request identical to the one we will send in Phase 4.
static void proto_selftest(void)
{
    polo_wire_protobuf_OuterMessage msg = polo_wire_protobuf_OuterMessage_init_zero;
    msg.protocol_version = 2;
    msg.status = polo_wire_protobuf_OuterMessage_Status_STATUS_OK;
    msg.has_pairing_request = true;
    strcpy(msg.pairing_request.service_name, "atvremote");
    msg.pairing_request.has_client_name = true;
    strcpy(msg.pairing_request.client_name, "esp32-androidtv");

    uint8_t buf[128];
    pb_ostream_t ostream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&ostream, polo_wire_protobuf_OuterMessage_fields, &msg)) {
        ESP_LOGE(TAG, "proto selftest: encode failed: %s", PB_GET_ERROR(&ostream));
        return;
    }

    polo_wire_protobuf_OuterMessage decoded = polo_wire_protobuf_OuterMessage_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(buf, ostream.bytes_written);
    if (!pb_decode(&istream, polo_wire_protobuf_OuterMessage_fields, &decoded)) {
        ESP_LOGE(TAG, "proto selftest: decode failed: %s", PB_GET_ERROR(&istream));
        return;
    }

    if (decoded.protocol_version != 2 || !decoded.has_pairing_request ||
        strcmp(decoded.pairing_request.service_name, "atvremote") != 0) {
        ESP_LOGE(TAG, "proto selftest: round-trip mismatch");
        return;
    }
    ESP_LOGI(TAG, "proto selftest OK (%u bytes, KEYCODE_DPAD_UP=%d)",
             (unsigned)ostream.bytes_written, (int)remote_RemoteKeyCode_KEYCODE_DPAD_UP);
}

// Phase 1 acceptance: mutual-TLS handshake to the TV's pairing port and read
// the peer certificate from the live session. Runs in its own task — the TLS
// handshake needs more stack than app_main's.
static void tv_session_task(void *arg)
{
    atv_tls_t tls;
    while (true) {
        if (atv_tls_connect(&tls, CONFIG_ATV_TV_IP, 6467) == ESP_OK) {
            const mbedtls_x509_crt *peer = atv_tls_peer_cert(&tls);
            if (peer != NULL) {
                char subject[128];
                mbedtls_x509_dn_gets(subject, sizeof(subject), &peer->subject);
                ESP_LOGI(TAG, "Phase 1 complete: TV peer cert subject: %s", subject);
            } else {
                ESP_LOGE(TAG, "Handshake OK but no peer certificate available");
            }
            atv_tls_close(&tls);
            break;
        }
        ESP_LOGW(TAG, "TLS connect failed, retrying in 5 s (is the TV on?)");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    proto_selftest();

    if (wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not configured; halting here.");
        return;
    }

    ESP_LOGI(TAG, "Phase 0 complete: WiFi up, nanopb working.");

    ESP_ERROR_CHECK(webserver_start());

    xTaskCreate(tv_session_task, "tv_session", 10240, NULL, 5, NULL);
}
