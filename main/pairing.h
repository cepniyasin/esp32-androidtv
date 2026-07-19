#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mbedtls/x509_crt.h"
#include "net_tls.h"

// Blocks until the user provides a pairing code (e.g. from the web form).
// Writes 6 hex chars + NUL into code. Returns false on timeout/abort.
typedef bool (*pairing_code_provider_t)(char code[7], void *ctx);

// SHA256(client_N ++ client_E ++ server_N ++ server_E ++ nonce) per
// pairing.py; code is 6 hex chars, nonce = bytes of code[2:6].
// Verifies hash[0] == code[0:2]; returns ESP_ERR_INVALID_ARG on mismatch
// (mistyped code), ESP_FAIL on non-RSA certs.
esp_err_t pairing_compute_secret(const mbedtls_x509_crt *client_cert,
                                 const mbedtls_x509_crt *server_cert,
                                 const char *code, uint8_t out[32]);

// Verifies pairing_compute_secret against the Python oracle values
// (test/golden_pairing.inc). Returns ESP_OK when the bytes match.
esp_err_t pairing_selftest(void);

// Runs the full pairing exchange on an established TLS connection to
// port 6467. Calls get_code once the TV is displaying the code.
esp_err_t pairing_run(atv_tls_t *tls, pairing_code_provider_t get_code, void *ctx);
