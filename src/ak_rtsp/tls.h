#pragma once
/* HTTPS for the admin page: TLS 1.2 (mbedTLS) with keys and certificates
 * made on the camera.
 *
 * - A private CA (EC P-256, 20 years) is created once. Users install its
 *   certificate (GET /api/tls/ca.crt) on their devices to trust the camera.
 * - The server certificate (EC P-256, 800 days, serverAuth) is signed by
 *   that CA for the camera's current IP address(es) and the DNS names the
 *   user configures. It is re-issued automatically when the address or
 *   names change, or 30 days before it expires; the CA stays the same, so
 *   devices keep trusting the camera.
 * Everything lives on the CONFIG partition (TLS_DIR). */
#include <stddef.h>
#include "conn.h"

#ifndef TLS_DIR
#define TLS_DIR "/etc/config"
#endif
#define TLS_NAMES_MAX 4                 /* DNS names in the certificate */

/* Load or create keys/certificates and set up the TLS server. 0 ok. */
int  tls_init(void);
int  tls_ready(void);
/* Re-check addresses / expiry and re-issue the server certificate if
 * needed (cheap when nothing changed). Called periodically. */
void tls_maintain(void);

/* Run the TLS handshake on an accepted socket. Returns a connection or NULL
 * (socket closed). handshake_ms bounds the whole handshake. */
conn_t *tls_accept(int fd, int handshake_ms);

/* Settings (TLS_DIR/ak_rtsp_tls.conf). names: comma/space separated DNS
 * names, validated. Re-issues the certificate when names change.
 * 0 ok, -1 with *err. */
int  tls_set_config(const char *names, int redirect, const char **err);
int  tls_name_ok(const char *host);   /* host is a configured certificate name */
int  tls_redirect(void);              /* 1 = send HTTP clients to HTTPS */
/* New CA (devices must re-install it) + new server certificate. */
int  tls_regenerate_ca(const char **err);

/* CA / server certificate, PEM. Returns length or -1. */
int  tls_ca_pem(char *out, size_t cap);
int  tls_server_pem(char *out, size_t cap);
/* {"ready":..,"names":"a,b","ips":"..","not_after":"..","ca_sha256":hex,
 *  "ca_spki":b64,"srv_sha256":hex,"srv_spki":b64,...}. The server key is kept
 *  across renewals, so srv_spki is a stable pin; srv_sha256 changes. */
int  tls_info_json(char *out, size_t cap);

/* ---- MQTT over TLS (client side) ----
 * verify: 0 none (testing), 1 broker CA (chain + host name/IP), 2 pinned broker key.
 * client_cert: 0 none, 1 issued by the camera CA (CN = cn), 2 uploaded /
 * signed from our CSR. Takes ownership of fd. NULL + err on failure. */
conn_t *tls_client_connect(int fd, const char *host, int verify, int client_cert, const char *cn,
                           int timeout_ms, char *err, size_t errcap);
/* what: "ca" (broker CA), "cert" (client cert), "key" (client key, PEM unencrypted) */
int  tls_mqtt_upload(const char *what, const char *pem, size_t n, const char **err);
int  tls_mqtt_clear(const char *what);             /* "ca" or "client" */
int  tls_mqtt_csr(const char *cn, char *out, size_t cap);           /* new key + CSR PEM */
int  tls_mqtt_camera_cert_pem(const char *cn, char *out, size_t cap);
int  tls_mqtt_info_json(char *out, size_t cap);
/* Fetch the chain a broker presents (fd: connected TCP socket, taken).
 * JSON array, leaf first. Then tls_mqtt_trust("ca"|"pin"). */
int  tls_mqtt_fetch(int fd, const char *host, char *out, size_t cap, char *err, size_t errcap);
/* expect: what the user was shown and confirmed: the SHA-256 (hex) of the
 * top certificate for "ca", the key pin (base64) of the leaf for "pin". */
int  tls_mqtt_trust(const char *what, const char *expect, const char **err);
