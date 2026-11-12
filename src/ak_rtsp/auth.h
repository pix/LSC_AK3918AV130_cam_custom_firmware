#pragma once
/* Credential store + Digest (RFC 2617) authentication shared by the RTSP
 * server (port 554), the web admin page (port 80) and the tuning server
 * (port 8091).
 *
 * Storage: AUTH_FILE on the persistent CONFIG JFFS2 partition (mtd6),
 * holding the username and HA1 = MD5(user:realm:password) — the password
 * itself is never stored. When the file is missing the default login
 * admin/admin applies (auth_is_default() reports this so the web page can
 * nag). Reset = delete the file (reset button held 10 s, or a reset_auth
 * file on the SD card at boot). */
#include <stddef.h>

#ifndef AUTH_ONVIF_FILE
#define AUTH_ONVIF_FILE "/etc/config/ak_rtsp_onvif_pw"
#endif
#ifndef AUTH_FILE
#define AUTH_FILE     "/etc/config/ak_rtsp_auth"
#endif
#define AUTH_REALM    "LSC Camera"
#define AUTH_DEF_USER "admin"
#define AUTH_DEF_PASS "admin"

void auth_init(void);
int  auth_is_default(void);
void auth_get_user(char *out, size_t cap);

/* Validate + persist new credentials. Returns 0 or -1 with *err set. */
int  auth_set(const char *user, const char *pass, const char **err);
/* Delete stored credentials -> back to admin/admin. */
void auth_reset(void);

/* Plaintext check (port 8091 AUTH command). 1 = ok. */
int  auth_check_plain(const char *user, const char *pass);

/* Digest. challenge(): fills "Digest realm=..., nonce=..." header value.
 * with_qop: add qop="auth" (browsers); RTSP clients get RFC 2069 style.
 * verify(): authz = full Authorization header value, method = request
 * method. Returns 1 ok, 0 bad credentials, -1 stale nonce (re-challenge
 * with stale=true). */
void auth_digest_challenge(char *out, size_t cap, int with_qop, int stale);
int  auth_digest_verify(const char *authz, const char *method);
/* Web / ONVIF: also binds the credentials to req_uri (the request target as
 * sent), requires qop=auth with a fresh nc and a short nonce lifetime. */
int  auth_digest_verify_ex(const char *authz, const char *method, const char *req_uri);

/* Login failure backoff per client address (all front ends):
 * auth_throttled() = seconds the address is still locked out (0 = free);
 * auth_record() after every credential check (ok = 1 success). */
struct sockaddr;
int  auth_throttled(const struct sockaddr *sa);
void auth_record(const struct sockaddr *sa, int ok);

/* Plain password for ONVIF WS-Security digests (0 ok, -1 unknown). */
int  auth_onvif_password(char *out, size_t cap);
