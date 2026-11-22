/* libFuzzer harness: motion decision (md_logic.c) with random settings and
 * random grid sequences, plus the md.zone_sens / md.line parsers.
 *   build: tests/fuzz/build_fuzzer.sh tests/fuzz/fuzz_md_logic.c <fuzz build dir>
 * Input: byte 0 bit 0 = 1: parsers only (rest = text). Otherwise bytes 1..
 * fill mdl_cfg_t (any values, also out of range: md_logic clamps), then
 * frames of [op, dt, seed]: op selects software / hardware frame, day/night
 * switch, disabled, a settings change or a raw-bytes grid; the grid comes
 * from a PRNG seeded per frame (blobs, noise, global steps) or straight from
 * the input. Checks: no UB / out of bounds (sanitizers), output in range,
 * every frame under 200 ms. */
#include "md_logic.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t g_r;
static uint32_t rnd(void) { g_r ^= g_r << 13; g_r ^= g_r >> 17; g_r ^= g_r << 5; return g_r; }

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

static void check_out(const mdl_out_t *o, int rows)
{
    if ((o->motion | o->motion_any | o->line | o->loiter) & ~1) abort();
    if (o->score < 0 || o->score > rows * MDL_COLS) abort();
    if (o->ncross < 0 || o->ncross > MDL_MAX_TRACKS || o->ntracks < 0 || o->ntracks > MDL_MAX_TRACKS) abort();
    for (int i = 0; i < o->ncross; i++) if (o->cross[i].dir != 1 && o->cross[i].dir != 2) abort();
    for (int i = 0; i < o->ntracks; i++)
        if (o->track[i].x < 0 || o->track[i].x > 1000 || o->track[i].y < 0 || o->track[i].y > 1000 || o->track[i].id <= 0) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 1) return 0;
    if (d[0] & 1) {
        char s[512];
        size_t l = n - 1 < sizeof s - 1 ? n - 1 : sizeof s - 1;
        memcpy(s, d + 1, l); s[l] = 0;
        mdl_zone_t z[MDL_MAX_ZONES];
        int line[4];
        int k = mdl_parse_zones(s, z, MDL_MAX_ZONES);
        if (k > MDL_MAX_ZONES) abort();
        for (int i = 0; i < k; i++) if (z[i].c0 > z[i].c1 || z[i].c1 > 31 || z[i].r1 > 17 || z[i].sens < 1 || z[i].sens > 100) abort();
        if (mdl_parse_line(s, line) == 1) for (int i = 0; i < 4; i++) if (line[i] < 0 || line[i] > 1000) abort();
        return 0;
    }
    static mdl_t st;
    static mdl_out_t out;
    mdl_cfg_t cfg;
    size_t p = 1;
    memset(&cfg, 0, sizeof cfg);
    {   /* settings: raw ints, mostly small */
        int *ci = (int *)&cfg;
        for (size_t i = 0; i < sizeof cfg / sizeof(int) && p + 1 < n; i++, p += 2) {
            int v = (int8_t)d[p] * (d[p + 1] & 0x80 ? 37 : 1) + (d[p + 1] & 0x7f);
            ci[i] = v;
        }
    }
    mdl_init(&st);
    int64_t t = (int64_t)(d[0] >> 1) * 1000000;
    static float cur[MDL_COLS * MDL_ROWS_SW];
    static uint16_t g[MDL_MAX_CELLS];
    static uint8_t watch[MDL_MAX_CELLS], watch_all[MDL_MAX_CELLS];
    for (int i = 0; i < MDL_MAX_CELLS; i++) { watch[i] = 1; watch_all[i] = 1; }
    int frames = 0;
    while (p + 3 <= n && frames < 3000) {
        uint8_t op = d[p], dt = d[p + 1], seed = d[p + 2];
        p += 3;
        t += dt & 0x80 ? (int64_t)(dt & 0x7f) * 1000 : dt * 4;        /* also long gaps; never backwards except op 7 */
        g_r = 0x9e3779b9u ^ (uint32_t)seed * 2654435761u ^ (uint32_t)frames;
        if (!g_r) g_r = 1;
        switch (op & 7) {
        case 0: mdl_night_switch(&st, t, op >> 7); continue;
        case 1: mdl_disabled(&st); continue;
        case 2: {                                                     /* change one setting */
            int *ci = (int *)&cfg;
            ci[(op >> 3) % (sizeof cfg / sizeof(int))] = (int8_t)seed * ((op & 0x80) ? 50 : 1);
            continue; }
        case 3:                                                       /* masks */
            for (int i = 0; i < MDL_MAX_CELLS; i++) { watch[i] = (uint8_t)(rnd() % 5 ? 1 : 0); watch_all[i] = (uint8_t)(rnd() % 9 ? 1 : rnd()); }
            continue;
        case 7: t -= seed * 10; break;                                /* clock jitter backwards */
        default: break;
        }
        int hw = (op >> 3) & 1, raw = (op >> 4) & 1;
        /* a scene: background + up to 3 blobs + noise, or the next input bytes */
        int nb = (int)(rnd() % 4), bx[3], by[3], bw[3], bh[3], ba[3], step = (int)(rnd() % 8) == 0 ? (int)(rnd() % 120) - 60 : 0;
        for (int b = 0; b < nb && b < 3; b++) { bx[b] = (int)(rnd() % 34) - 2; by[b] = (int)(rnd() % 26) - 2; bw[b] = 1 + (int)(rnd() % 8); bh[b] = 1 + (int)(rnd() % 8); ba[b] = (int)(rnd() % 20000); }
        int rows = hw ? MDL_ROWS_HW : MDL_ROWS_SW;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < MDL_COLS; c++) {
                int i = r * MDL_COLS + c, v;
                if (raw && p < n) v = d[p++] * 257;
                else {
                    v = (hw ? 300 : 80) + (int)(rnd() % 40) + step * (hw ? 50 : 1);
                    for (int b = 0; b < nb && b < 3; b++)
                        if (c >= bx[b] && c < bx[b] + bw[b] && r >= by[b] && r < by[b] + bh[b]) v += hw ? ba[b] : ba[b] / 100;
                }
                if (hw) g[i] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
                else cur[i] = (float)(v < 0 ? 0 : v > 255 ? 255 : v) + (float)(rnd() % 100) / 100.0f;
            }
        mdl_in_t in = { t, (op >> 5) & 1, watch, watch_all };
        double a = now_ms();
        if (hw) { mdl_hw_frame(&st, &cfg, g, &in, &out); check_out(&out, rows); }
        else if (!mdl_sw_frame(&st, &cfg, cur, &in, &out)) check_out(&out, rows);
        if (now_ms() - a > 200) { fprintf(stderr, "frame took %.0f ms\n", now_ms() - a); abort(); }
        frames++;
    }
    return 0;
}
