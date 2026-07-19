#include "net_tls.h"

#include <string.h>

#include "esp_log.h"
#include "mbedtls/error.h"

static const char *TAG = "net_tls";

// Embedded via board_build.embed_txtfiles (null-terminated).
extern const uint8_t client_pem_start[] asm("_binary_client_pem_start");
extern const uint8_t client_key_start[] asm("_binary_client_key_start");

static void log_mbedtls_err(const char *what, int ret)
{
    char err[100];
    mbedtls_strerror(ret, err, sizeof(err));
    ESP_LOGE(TAG, "%s failed: -0x%04x (%s)", what, (unsigned)-ret, err);
}

esp_err_t atv_tls_connect(atv_tls_t *t, const char *host, uint16_t port)
{
    memset(t, 0, sizeof(*t));
    mbedtls_net_init(&t->sock);
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->ctr_drbg);
    mbedtls_x509_crt_init(&t->client_cert);
    mbedtls_pk_init(&t->client_key);

    int ret;
    if ((ret = mbedtls_ctr_drbg_seed(&t->ctr_drbg, mbedtls_entropy_func, &t->entropy,
                                     (const unsigned char *)"atvremote", 9)) != 0) {
        log_mbedtls_err("ctr_drbg_seed", ret);
        goto fail;
    }
    if ((ret = mbedtls_x509_crt_parse(&t->client_cert, client_pem_start,
                                      strlen((const char *)client_pem_start) + 1)) != 0) {
        log_mbedtls_err("parse client cert", ret);
        goto fail;
    }
    if ((ret = mbedtls_pk_parse_key(&t->client_key, client_key_start,
                                    strlen((const char *)client_key_start) + 1, NULL, 0,
                                    mbedtls_ctr_drbg_random, &t->ctr_drbg)) != 0) {
        log_mbedtls_err("parse client key", ret);
        goto fail;
    }
    if ((ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        log_mbedtls_err("ssl_config_defaults", ret);
        goto fail;
    }
    // The TV's cert is self-signed; trust is established by the pairing
    // secret, not a CA chain.
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->ctr_drbg);
    if ((ret = mbedtls_ssl_conf_own_cert(&t->conf, &t->client_cert, &t->client_key)) != 0) {
        log_mbedtls_err("conf_own_cert", ret);
        goto fail;
    }
    if ((ret = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0) {
        log_mbedtls_err("ssl_setup", ret);
        goto fail;
    }

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);
    ESP_LOGI(TAG, "Connecting to %s:%s...", host, port_str);
    if ((ret = mbedtls_net_connect(&t->sock, host, port_str, MBEDTLS_NET_PROTO_TCP)) != 0) {
        log_mbedtls_err("net_connect", ret);
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, &t->sock, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);

    while ((ret = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_mbedtls_err("handshake", ret);
            goto fail;
        }
    }
    ESP_LOGI(TAG, "TLS handshake OK (%s, %s)", mbedtls_ssl_get_version(&t->ssl),
             mbedtls_ssl_get_ciphersuite(&t->ssl));
    t->connected = true;
    return ESP_OK;

fail:
    atv_tls_close(t);
    return ESP_FAIL;
}

int atv_tls_read(atv_tls_t *t, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    mbedtls_ssl_conf_read_timeout(&t->conf, timeout_ms);
    int ret;
    do {
        ret = mbedtls_ssl_read(&t->ssl, buf, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }
    return ret;
}

int atv_tls_write(atv_tls_t *t, const uint8_t *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(&t->ssl, buf + written, len - written);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret < 0) {
            log_mbedtls_err("ssl_write", ret);
            return ret;
        }
        written += ret;
    }
    return 0;
}

const mbedtls_x509_crt *atv_tls_peer_cert(const atv_tls_t *t)
{
    return t->connected ? mbedtls_ssl_get_peer_cert(&t->ssl) : NULL;
}

const mbedtls_x509_crt *atv_tls_own_cert(const atv_tls_t *t)
{
    return t->connected ? &t->client_cert : NULL;
}

void atv_tls_close(atv_tls_t *t)
{
    if (t->connected) {
        mbedtls_ssl_close_notify(&t->ssl);
    }
    mbedtls_net_free(&t->sock);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->ctr_drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->client_cert);
    mbedtls_pk_free(&t->client_key);
    t->connected = false;
}
