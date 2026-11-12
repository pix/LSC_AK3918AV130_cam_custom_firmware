#pragma once
/* RFC 1321 MD5 — used only for RTSP/HTTP Digest authentication (RFC 2617),
 * which mandates MD5. Not used for anything else. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[4];
    uint64_t count;   /* bytes processed */
    uint8_t  buf[64];
} md5_ctx;

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
void md5_final(md5_ctx *c, uint8_t out[16]);

/* Lowercase hex digest of data, NUL-terminated. */
void md5_hex(const void *data, size_t len, char out_hex[33]);
/* Lowercase hex of a 16-byte digest. */
void md5_to_hex(const uint8_t d[16], char out_hex[33]);
