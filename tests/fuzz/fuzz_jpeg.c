/* Fuzz harness: jpeg_enc.c jpeg_encode_nv12() (web snapshot / recording
 * snapshots) with fuzzed sizes (odd, 0, 1, tiny, wide), quality (out of
 * range too), pixel content and output buffer size.
 * Input: [w u16][h u16][quality i16][out_cap u16][pixels...] (pixels repeat
 * to fill w*h*3/2). Both buffers are exact-size heap blocks for ASan. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "jpeg_enc.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 8) return 0;
    int w = d[0] | d[1] << 8, h = d[2] | d[3] << 8;
    int q = (int16_t)(d[4] | d[5] << 8);
    int cap = d[6] | d[7] << 8;
    if (w & 0x8000) w = -(w & 0x7fff);          /* negative sizes too */
    if ((int64_t)abs(w) * abs(h) > 640 * 368) return 0;   /* the sub stream is at most 640x368 */
    const uint8_t *px = d + 8;
    size_t pn = n - 8;
    size_t sz = w > 0 && h > 0 ? (size_t)w * h * 3 / 2 : 1;
    uint8_t *img = malloc(sz);
    for (size_t i = 0; i < sz; i++) img[i] = pn ? px[i % pn] : 0;
    if (cap == 0xffff) cap = 1 << 20;
    uint8_t *out = malloc(cap ? (size_t)cap : 1);
    int r = jpeg_encode_nv12(img, w, h, q, out, cap);
    if (r > cap) __builtin_trap();
    if (r > 0 && (out[0] != 0xff || out[1] != 0xd8 || out[r - 2] != 0xff || out[r - 1] != 0xd9)) __builtin_trap();
    free(out);
    free(img);
    return 0;
}
