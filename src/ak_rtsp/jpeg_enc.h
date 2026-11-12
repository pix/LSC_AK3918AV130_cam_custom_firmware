#pragma once
/* Minimal baseline JPEG encoder (4:2:0, Annex K tables, integer DCT).
 * Used for the web admin page's snapshot from the 640x360 NV12 sub channel.
 * No malloc, no mutable static state (reentrant), integer math only. */
#include <stdint.h>

/* nv12: Y plane (w*h bytes) followed by interleaved UV plane (w*h/2 bytes,
 * U first). w and h must be even (edge MCUs are padded by replication).
 * quality 1..100.
 * Returns bytes written to out, or -1 on bad args / out_cap too small.
 * Never writes past out_cap. */
int jpeg_encode_nv12(const uint8_t *nv12, int w, int h, int quality,
                     uint8_t *out, int out_cap);
