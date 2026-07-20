#include <string.h>

#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "net_tls.h"
#include "nvs_flash.h"
#include "pairing.h"
#include "remote.h"
#include "store.h"
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

// Blocks until the web form delivers a code (or 180 s pass). The TV keeps
// showing the code the whole time.
static bool code_from_web(char code[7], void *ctx)
{
    (void)ctx;
    return xQueueReceive(g_pair_code_queue, code, pdMS_TO_TICKS(180000)) == pdTRUE;
}

// Owns all TV-facing I/O (PLAN.md §4.5): pairs if needed, then runs the
// control channel with auto-reconnect. Separate task: TLS needs the stack.
static void tv_session_task(void *arg)
{
    (void)arg;
    while (!g_atv_status.paired) {
        atv_tls_t tls;
        if (atv_tls_connect(&tls, CONFIG_ATV_TV_IP, 6467) != ESP_OK) {
            ESP_LOGW(TAG, "Pairing connect failed, retrying in 5 s (is the TV on?)");
            g_atv_status.state = ATV_STATE_PAIR_FAILED;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        esp_err_t err = pairing_run(&tls, code_from_web, NULL);
        atv_tls_close(&tls);
        if (err == ESP_OK) {
            ESP_ERROR_CHECK(store_set_paired(true));
            g_atv_status.paired = true;
            g_atv_status.state = ATV_STATE_PAIRED;
            xEventGroupSetBits(g_pair_events, PAIR_EVENT_OK);
            ESP_LOGI(TAG, "Phase 4 complete: paired with the TV");
        } else {
            g_atv_status.state = ATV_STATE_PAIR_FAILED;
            xEventGroupSetBits(g_pair_events, PAIR_EVENT_FAIL);
            ESP_LOGW(TAG, "Pairing attempt failed (%s); restarting exchange",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    // Control channel with reconnect + backoff.
    uint32_t backoff_ms = 1000;
    while (true) {
        atv_tls_t tls;
        if (atv_tls_connect(&tls, CONFIG_ATV_TV_IP, 6466) == ESP_OK) {
            backoff_ms = 1000;
            remote_session(&tls);
            atv_tls_close(&tls);
        }
        g_atv_status.connected = false;
        g_atv_status.state = ATV_STATE_PAIRED;
        ESP_LOGW(TAG, "Control channel down; reconnecting in %u ms", (unsigned)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        // Cap low enough that recovery after a WiFi/TV outage never feels
        // stuck: worst case ~15 s after the network is back.
        if (backoff_ms < 15000) {
            backoff_ms *= 2;
        }
    }
}

void app_main(void)
{
    logbuf_init();  // before anything else, so boot-time logs are captured too

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    app_state_init();
    proto_selftest();
    ESP_ERROR_CHECK(pairing_selftest());  // §5: never pair live with unverified math

    g_atv_status.paired = store_get_paired();

    if (wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not configured; halting here.");
        return;
    }

    ESP_LOGI(TAG, "Phase 0 complete: WiFi up, nanopb working.");

    ESP_ERROR_CHECK(webserver_start());

    if (g_atv_status.paired) {
        g_atv_status.state = ATV_STATE_PAIRED;
    }
    xTaskCreate(tv_session_task, "tv_session", 10240, NULL, 5, NULL);
}
