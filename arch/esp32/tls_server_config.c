/*
 * arch/esp32/tls_server_config.c
 *
 * Configures the TLS server via a linker-wrap of mbedtls_ssl_config_defaults():
 *
 *  1. Restricts cipher suites to RSA key exchange, eliminating the ECDH
 *     computation that starves IDLE0 and triggers the task watchdog during
 *     HTTPS handshakes.
 *
 *  2. Enables TLS session tickets so that browsers can resume sessions
 *     without a full RSA handshake.
 *
 * Background (cipher suite restriction):
 *   Browsers negotiate ECDHE cipher suites by default. On ESP32-S3 the
 *   hardware RSA accelerator handles RSA key exchange efficiently, but
 *   there is no ECP hardware accelerator. ECDHE requires the server to
 *   compute an ephemeral key pair: ecp_precompute_comb() builds a comb
 *   table through many sequential ECP point doublings, each dispatched
 *   to the hardware bignum unit (esp_bignum.c), but the ECP layer has
 *   no RTOS yield points between iterations. The entire computation runs
 *   to completion on CPU 0 without ever resetting the task watchdog.
 *
 *   A single handshake does not exceed the watchdog timeout on its own,
 *   but two consecutive handshakes (e.g. a browser retry after a failed
 *   attempt) accumulate enough uninterrupted runtime to starve IDLE0:
 *
 *     E (54924) esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x0050
 *     E (57208) esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x7280
 *     E (57638) task_wdt: Task watchdog got triggered.
 *     E (57638) task_wdt:  - IDLE0 (CPU 0)
 *     E (57638) task_wdt: Tasks currently running:
 *     E (57638) task_wdt: CPU 0: httpd
 *
 *   The crash occurs in ecp_precompute_comb() → ecp_double_jac() →
 *   mbedtls_mpi_mul_mpi() during the ServerKeyExchange step.
 *
 *   There is no sdkconfig knob accessible at runtime through
 *   esp_https_server. The user_cb hook fires after the handshake, too
 *   late to change cipher suites. The only pre-handshake intercept point
 *   is mbedtls_ssl_config_defaults(), called once per ssl_config init.
 *
 *   MQTT uses MBEDTLS_SSL_IS_CLIENT; the HTTPS server uses
 *   MBEDTLS_SSL_IS_SERVER — this is the discriminator.
 *
 * Note: this file is compiled only for [esp32_base] targets (IDF v4).
 * CONFIG_MBEDTLS_SSL_SESSION_TICKETS is enabled by default in the
 * arduino-esp32 2.x pre-built framework, so no sdkconfig changes are needed.
 */

#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ticket.h"
#include "esp_random.h"

/*
 * RSA key exchange cipher suites only.
 * Must be static — mbedTLS stores the pointer, does not copy the array.
 * Terminated with 0.
 */
static const int kServerOnlyCipherSuites[] = {
  MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_RSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA256,
  MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA256,
  0
};

/*
 * Session ticket context. Must be static — mbedTLS stores the pointer for
 * the lifetime of the server, analogous to kServerOnlyCipherSuites.
 * Initialized once on first server start.
 */
static mbedtls_ssl_ticket_context s_ticket_ctx;
static int s_ticket_ctx_ready = 0;

/* Thin wrapper around the ESP32 hardware RNG for mbedtls_ssl_ticket_setup(). */
static int esp_rng(void *ctx, unsigned char *buf, size_t len) {
  (void)ctx;
  esp_fill_random(buf, len);
  return 0;
}

extern int __real_mbedtls_ssl_config_defaults(
  mbedtls_ssl_config *conf,
  int endpoint,
  int transport,
  int preset
);

int __wrap_mbedtls_ssl_config_defaults(
  mbedtls_ssl_config *conf,
  int endpoint,
  int transport,
  int preset
) {
  int ret = __real_mbedtls_ssl_config_defaults(conf, endpoint, transport, preset);
  if (ret == 0 && endpoint == MBEDTLS_SSL_IS_SERVER) {
    mbedtls_ssl_conf_ciphersuites(conf, kServerOnlyCipherSuites);

    if (!s_ticket_ctx_ready) {
      mbedtls_ssl_ticket_init(&s_ticket_ctx);
      int ticket_ret = mbedtls_ssl_ticket_setup(&s_ticket_ctx, esp_rng, NULL,
                                                MBEDTLS_CIPHER_AES_256_GCM, 86400);
      if (ticket_ret == 0) {
        s_ticket_ctx_ready = 1;
      }
    }
    if (s_ticket_ctx_ready) {
      mbedtls_ssl_conf_session_tickets_cb(conf,
        mbedtls_ssl_ticket_write,
        mbedtls_ssl_ticket_parse,
        &s_ticket_ctx);
    }
  }
  return ret;
}
