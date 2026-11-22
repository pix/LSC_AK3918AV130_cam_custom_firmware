/* Fuzz harness: sdn.c day/night decision ("soft photosensor") with fuzzed ISP
 * statistics: AE run info, exposure limits, AWB block sums (extremes, zero),
 * frame timing, and (optionally) parameters.
 *
 * Input: [mode][fps i16][w i16][h i16][92 bytes params]
 *        then frames of 85 bytes:
 *        [36 run_info][36 exposure pub (expo+0x1c)][dt u16][night u8][awb off u16][8 awb bytes]
 * mode bit0: 0 = camera parameters (SDN_DEFAULTS, as night.c), 1 = fuzzed
 *            parameters after sdn_params_sanitize() (skipped if rejected),
 *      bit1: fps / w / h from the input instead of night.c's 10 fps 1920x1080,
 *      bit2: start the AWB buffer from zeros instead of a mid-grey scene. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sdn.h"

#define AWB_SIZE 0x24a0
static void wr32(uint8_t *b, unsigned off, uint32_t v) { b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8); b[off + 2] = (uint8_t)(v >> 16); b[off + 3] = (uint8_t)(v >> 24); }

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 1 + 6 + 92) return 0;
    int mode = d[0];
    int16_t fps = (int16_t)(d[1] | d[2] << 8), w = (int16_t)(d[3] | d[4] << 8), h = (int16_t)(d[5] | d[6] << 8);
    if (!(mode & 2)) { fps = 10; w = 1920; h = 1080; }
    /* known (latent, night.c always passes 10): fps < 0 or > 1000 gives
     * frame_ms 0 and a division by zero in sdn_step(); skipped so the rest
     * keeps being fuzzed. Reproducer kept in the scratch dir. */
    if (fps < 0 || fps > 1000) fps = 10;
    sdn_params p = SDN_DEFAULTS;
    if (mode & 1) {
        memcpy(&p, d + 7, sizeof p);
        if (sdn_params_sanitize(&p) != 0) return 0;
    } else sdn_params_sanitize(&p);

    uint8_t *awb = malloc(AWB_SIZE);       /* exact size: ASan catches reads past it */
    uint8_t *expo = malloc(0x720);
    memset(expo, 0, 0x720);
    if (mode & 4) memset(awb, 0, AWB_SIZE);
    else {
        uint32_t s = 1;
        for (unsigned off = 0; off + 4 <= AWB_SIZE; off += 4) {
            s = s * 1664525u + 1013904223u;
            wr32(awb, off, 2700u * (60 + (s >> 25)));
        }
        wr32(awb, 0x9c, 768);
    }
    sdn_state st;
    sdn_reset(&st, fps);
    uint64_t now = 100000;
    int night = 0;
    const uint8_t *q = d + 7 + 92, *end = d + n;
    while (end - q >= 85) {
        uint8_t ri[36];
        memcpy(ri, q, 36);
        memcpy(expo + 0x1c, q + 36, 36);
        uint16_t dt = (uint16_t)(q[72] | q[73] << 8);
        int cur = q[74] & 1;
        unsigned off = (unsigned)(q[75] | q[76] << 8) % (AWB_SIZE - 7);
        memcpy(awb + off, q + 77, 8);
        q += 85;
        now += dt;
        uint64_t ev = sdn_ev(ri, expo);
        unsigned ae = sdn_ae_classify(ri, expo);
        uint32_t thr;
        int r = sdn_non_ir_ratio(awb, w, h, &p, &thr);
        int32_t dis = sdn_rgb_dis(awb, &p);
        uint32_t y[SDN_BLKS];
        sdn_blk_lumi(awb, w, h, y);
        uint32_t rd = p.blk_check_flag ? (r < 0 ? 0 : (uint32_t)r) : (uint32_t)dis;
        int out = sdn_step(&st, &p, (uint32_t)ev, rd, ae, now, (mode & 8) ? cur : night);
        if (out == SDN_DAY) night = 0;
        else if (out == SDN_NIGHT) night = 1;
    }
    free(awb);
    free(expo);
    return 0;
}
