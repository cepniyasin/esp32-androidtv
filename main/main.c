#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
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
}
