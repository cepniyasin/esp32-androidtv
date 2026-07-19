#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

// A mutual-TLS client connection to the TV (pairing :6467 or control :6466).
// Presents the embedded self-signed client cert; server cert is not verified
// (the protocol trusts it via the pairing secret instead).
typedef struct {
    mbedtls_net_context sock;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;
    bool connected;
} atv_tls_t;

// Connects and completes the TLS handshake. Blocking.
esp_err_t atv_tls_connect(atv_tls_t *t, const char *host, uint16_t port);

// Returns bytes read (>0), 0 on clean close, <0 on error (mbedTLS code).
// Blocks until data is available unless timeout_ms elapses first
// (returns MBEDTLS_ERR_SSL_TIMEOUT then).
int atv_tls_read(atv_tls_t *t, uint8_t *buf, size_t len, uint32_t timeout_ms);

// Writes the full buffer. Returns 0 on success, mbedTLS error code otherwise.
int atv_tls_write(atv_tls_t *t, const uint8_t *buf, size_t len);

// Peer certificate of the live session (valid until atv_tls_close). NULL if
// not connected.
const mbedtls_x509_crt *atv_tls_peer_cert(const atv_tls_t *t);

// Our own parsed client certificate (valid while connected).
const mbedtls_x509_crt *atv_tls_own_cert(const atv_tls_t *t);

void atv_tls_close(atv_tls_t *t);
