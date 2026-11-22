/* Valid little recordings for the Profile G fuzz harnesses (fuzz_fmp4r.c,
 * fuzz_onvif_g.c), written with the firmware's own muxer (fmp4.c): a GOP of
 * gop frames per fragment, 15 fps, optional 8 kHz audio in 32 ms periods.
 * The H.264 payload is not decodable video, only well-formed NAL units. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fmp4.h"

static int pg_make_mp4(const char *path, int frames, int gop, int audio)
{
    static const uint8_t key[] = {
        0, 0, 0, 1, 0x67, 0x4d, 0x00, 0x28, 0x95, 0xa0, 0x1e, 0x00, 0x89, 0xf9, 0x50,   /* SPS 1920x1080 */
        0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80,                                              /* PPS */
        0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33, 0xff, 0x12, 0x34 };                    /* IDR slice */
    uint8_t big[3000], p[40];
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fmp4_t *m = fmp4_open(f, key, sizeof key, audio);
    if (!m) { fclose(f); return -1; }
    int16_t pcm[256];
    for (int i = 0; i < 256; i++) pcm[i] = (int16_t)(i * 97);
    uint64_t t8 = 0;
    for (int i = 0; i < frames; i++) {
        uint64_t ts = 5000 + (uint64_t)i * 1000 / 15;
        int k = i % gop == 0;
        if (k) {
            fmp4_flush(m);
            if (i % (2 * gop) == 0) {                /* every other keyframe: one NAL > one RTP packet (FU-A) */
                memcpy(big, key, sizeof key);
                memset(big + sizeof key, 0x5a, sizeof big - sizeof key);
                fmp4_video(m, big, sizeof big, 1, ts);
            } else {
                fmp4_video(m, key, sizeof key, 1, ts);
            }
        } else {
            memcpy(p, "\0\0\0\1\x41\x9a\x02\x03", 8);
            memset(p + 8, i & 0xff, sizeof p - 8);
            fmp4_video(m, p, sizeof p, 0, ts);
        }
        while (audio && t8 < (uint64_t)(i + 1) * 8000 / 15) { fmp4_audio(m, pcm, 256, t8); t8 += 256; }
    }
    fmp4_close(m);
    fclose(f);
    return 0;
}
