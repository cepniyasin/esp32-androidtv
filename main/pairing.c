#include "pairing.h"

#include <ctype.h>
#include <string.h>

#include "app_state.h"
#include "esp_log.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "pb_decode.h"
#include "polo.pb.h"
#include "proto_frame.h"

#include "../test/golden_pairing.inc"

static const char *TAG = "pairing";

extern const uint8_t client_pem_start[] asm("_binary_client_pem_start");

// --- secret computation (PLAN.md §7) ---

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_decode(const char *hex, size_t n_bytes, uint8_t *out)
{
    for (size_t i = 0; i < n_bytes; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// Feeds the RSA modulus and exponent of cert into the hash, matching
// Python's bytes.fromhex(f"{n:X}") — minimal big-endian, no leading zero.
// mbedtls_mpi_write_binary with mbedtls_mpi_size() gives exactly that.
static esp_err_t hash_cert_pubkey(mbedtls_sha256_context *sha,
                                  const mbedtls_x509_crt *cert)
{
    if (mbedtls_pk_get_type(&cert->pk) != MBEDTLS_PK_RSA) {
        ESP_LOGE(TAG, "Certificate public key is not RSA");
        return ESP_FAIL;
    }
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(cert->pk);
    mbedtls_mpi n, e;
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);
    esp_err_t result = ESP_FAIL;
    uint8_t buf[256 + 4];  // RSA-2048 modulus; exponent is tiny

    if (mbedtls_rsa_export(rsa, &n, NULL, NULL, NULL, &e) != 0) {
        goto out;
    }
    size_t n_len = mbedtls_mpi_size(&n);
    size_t e_len = mbedtls_mpi_size(&e);
    if (n_len > sizeof(buf) || e_len > sizeof(buf)) {
        goto out;
    }
    if (mbedtls_mpi_write_binary(&n, buf, n_len) != 0) {
        goto out;
    }
    mbedtls_sha256_update(sha, buf, n_len);
    if (mbedtls_mpi_write_binary(&e, buf, e_len) != 0) {
        goto out;
    }
    mbedtls_sha256_update(sha, buf, e_len);
    result = ESP_OK;
out:
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);
    return result;
}

esp_err_t pairing_compute_secret(const mbedtls_x509_crt *client_cert,
                                 const mbedtls_x509_crt *server_cert,
                                 const char *code, uint8_t out[32])
{
    if (strlen(code) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t code_bytes[3];
    if (!hex_decode(code, 3, code_bytes)) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    esp_err_t err = hash_cert_pubkey(&sha, client_cert);
    if (err == ESP_OK) {
        err = hash_cert_pubkey(&sha, server_cert);
    }
    if (err != ESP_OK) {
        mbedtls_sha256_free(&sha);
        return err;
    }
    mbedtls_sha256_update(&sha, code_bytes + 1, 2);  // nonce = code[2:6]
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);

    if (out[0] != code_bytes[0]) {
        ESP_LOGW(TAG, "Code checksum mismatch (hash[0]=%02x, code[0:2]=%02x)",
                 out[0], code_bytes[0]);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t pairing_selftest(void)
{
    mbedtls_x509_crt client, server;
    mbedtls_x509_crt_init(&client);
    mbedtls_x509_crt_init(&server);
    esp_err_t err = ESP_FAIL;
    if (mbedtls_x509_crt_parse(&client, client_pem_start,
                               strlen((const char *)client_pem_start) + 1) != 0 ||
        mbedtls_x509_crt_parse(&server, (const uint8_t *)oracle_server_pem,
                               sizeof(oracle_server_pem)) != 0) {
        ESP_LOGE(TAG, "selftest: cert parse failed");
        goto out;
    }
    uint8_t secret[32];
    err = pairing_compute_secret(&client, &server, oracle_code, secret);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "selftest: compute failed (%s)", esp_err_to_name(err));
        goto out;
    }
    if (memcmp(secret, oracle_secret, 32) != 0) {
        ESP_LOGE(TAG, "selftest: secret does not match Python oracle");
        err = ESP_FAIL;
        goto out;
    }
    ESP_LOGI(TAG, "pairing secret selftest OK (matches Python oracle)");
    err = ESP_OK;
out:
    mbedtls_x509_crt_free(&client);
    mbedtls_x509_crt_free(&server);
    return err;
}

// --- message exchange (mirrors pairing.py _handle_message) ---

static int tls_read_adapter(void *ctx, uint8_t *buf, size_t len)
{
    return atv_tls_read(ctx, buf, len, 30000);
}

static int tls_write_adapter(void *ctx, const uint8_t *buf, size_t len)
{
    return atv_tls_write(ctx, buf, len);
}

static polo_wire_protobuf_OuterMessage msg_default(void)
{
    polo_wire_protobuf_OuterMessage m = polo_wire_protobuf_OuterMessage_init_zero;
    m.protocol_version = 2;
    m.status = polo_wire_protobuf_OuterMessage_Status_STATUS_OK;
    return m;
}

static esp_err_t send_msg(atv_tls_t *tls, const polo_wire_protobuf_OuterMessage *m)
{
    int r = frame_send(polo_wire_protobuf_OuterMessage_fields, m, tls_write_adapter, tls);
    if (r != 0) {
        ESP_LOGE(TAG, "send failed (%d)", r);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void fill_encoding(polo_wire_protobuf_Options_Encoding *enc)
{
    enc->type = polo_wire_protobuf_Options_Encoding_EncodingType_ENCODING_TYPE_HEXADECIMAL;
    enc->symbol_length = 6;
}

esp_err_t pairing_run(atv_tls_t *tls, pairing_code_provider_t get_code, void *ctx)
{
    g_atv_status.state = ATV_STATE_PAIRING;

    polo_wire_protobuf_OuterMessage m = msg_default();
    m.has_pairing_request = true;
    strcpy(m.pairing_request.service_name, "atvremote");
    m.pairing_request.has_client_name = true;
    strcpy(m.pairing_request.client_name, "esp32-androidtv");
    if (send_msg(tls, &m) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t buf[FRAME_MAX_MSG];
    while (true) {
        int len = frame_recv(buf, sizeof(buf), tls_read_adapter, tls);
        if (len < 0) {
            ESP_LOGE(TAG, "recv failed (%d)", len);
            return ESP_FAIL;
        }
        polo_wire_protobuf_OuterMessage in = polo_wire_protobuf_OuterMessage_init_zero;
        pb_istream_t is = pb_istream_from_buffer(buf, (size_t)len);
        if (!pb_decode(&is, polo_wire_protobuf_OuterMessage_fields, &in)) {
            ESP_LOGE(TAG, "decode failed: %s", PB_GET_ERROR(&is));
            return ESP_FAIL;
        }
        if (in.status != polo_wire_protobuf_OuterMessage_Status_STATUS_OK) {
            ESP_LOGE(TAG, "TV reported status %d (bad secret = 402)", (int)in.status);
            return ESP_FAIL;
        }

        if (in.has_pairing_request_ack) {
            ESP_LOGI(TAG, "pairing_request acked; sending options");
            polo_wire_protobuf_OuterMessage out = msg_default();
            out.has_options = true;
            out.options.has_preferred_role = true;
            out.options.preferred_role =
                polo_wire_protobuf_Options_RoleType_ROLE_TYPE_INPUT;
            out.options.input_encodings_count = 1;
            fill_encoding(&out.options.input_encodings[0]);
            if (send_msg(tls, &out) != ESP_OK) {
                return ESP_FAIL;
            }
        } else if (in.has_options) {
            ESP_LOGI(TAG, "options received; sending configuration");
            polo_wire_protobuf_OuterMessage out = msg_default();
            out.has_configuration = true;
            out.configuration.client_role =
                polo_wire_protobuf_Options_RoleType_ROLE_TYPE_INPUT;
            fill_encoding(&out.configuration.encoding);
            if (send_msg(tls, &out) != ESP_OK) {
                return ESP_FAIL;
            }
        } else if (in.has_configuration_ack) {
            ESP_LOGI(TAG, "configuration acked; TV is showing the code");
            g_atv_status.state = ATV_STATE_WAIT_CODE;
            char code[7];
            if (!get_code(code, ctx)) {
                ESP_LOGW(TAG, "No pairing code provided");
                return ESP_ERR_TIMEOUT;
            }
            g_atv_status.state = ATV_STATE_PAIRING;
            uint8_t secret[32];
            esp_err_t err = pairing_compute_secret(
                atv_tls_own_cert(tls), atv_tls_peer_cert(tls), code, secret);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Secret computation rejected the code");
                return err;
            }
            polo_wire_protobuf_OuterMessage out = msg_default();
            out.has_secret = true;
            out.secret.secret.size = 32;
            memcpy(out.secret.secret.bytes, secret, 32);
            if (send_msg(tls, &out) != ESP_OK) {
                return ESP_FAIL;
            }
        } else if (in.has_secret_ack) {
            ESP_LOGI(TAG, "secret acked — paired!");
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Unhandled pairing message; ignoring");
        }
    }
}
