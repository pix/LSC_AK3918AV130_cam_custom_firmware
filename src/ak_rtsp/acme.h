#pragma once
/* ACME client (RFC 8555): gets a publicly trusted certificate (Let's
 * Encrypt, or any ACME server) for the names in acme.domains and installs
 * it as the custom HTTPS certificate (tls_custom_install), renewing it 30
 * days before it expires.
 *
 * Challenges: http-01 (the camera answers on port 80, which must be
 * reachable from the internet) or dns-01 through Cloudflare, DuckDNS or
 * acme-dns. Settings are acme.* (control registry); off by default. */
#include <stddef.h>

void acme_init(void);      /* register settings (before control_load_ext_settings) */
void acme_start(void);     /* background thread */

/* GET /api/acme */
int  acme_status_json(char *out, size_t cap);
/* Ask for an issuance now (ignores the retry delay). */
void acme_run_now(void);
/* http-01: key authorization for token (port 80, no login). Length or -1. */
int  acme_http01(const char *token, char *out, size_t cap);
/* acme-dns: register an account on acme.dns_api, store its credentials in
 * the acme.* settings (acme.dns_target = the CNAME target).
 * _start: done by the ACME thread (the web thread never waits on the
 * network); result in the log / acme.dns_target. The direct call blocks. */
void acme_dns_register_start(void);
int  acme_dns_register(char *out, size_t cap, char *err, size_t errcap);
