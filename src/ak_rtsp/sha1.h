#pragma once
/* SHA-1 + Base64, only for the WebSocket handshake (RFC 6455 mandates
 * SHA-1 there; it is not used for anything security-relevant). */
#include <stddef.h>
#include <stdint.h>

void sha1(const void *data, size_t len, uint8_t out[20]);
/* Base64 of n bytes into out (needs 4*((n+2)/3)+1 bytes). Returns length. */
size_t base64_encode(const uint8_t *in, size_t n, char *out);
/* Sec-WebSocket-Accept for a client key (out: 29 bytes incl. NUL). */
void ws_accept_key(const char *client_key, char out[29]);
