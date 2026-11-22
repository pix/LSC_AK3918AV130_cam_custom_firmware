/* Host unit test for md_logic.c (tests/motion/run.sh: ASan+UBSan and valgrind).
 *  - all options off == the original md.c decision (md_legacy.c), on long
 *    random sequences for both sources (motion, whole-picture motion, score,
 *    red cells, background, event calls, diagnostics);
 *  - synthetic scenes for each option: line crossing both ways (+ direction
 *    filter), minimum object size, brief changes / one-frame flicker, global
 *    light changes, loitering vs passing through, per-zone sensitivity;
 *  - settings parsers; worst-case time per frame.
 * --write-log <file>: write a hardware-statistic log (an object crossing the
 * middle of the picture) in the md_replay format. */
#include "md_logic.h"
#include "md_legacy.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 11); }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* ---- the new md.c, as far as the decision goes (mask building + md_act) ---- */
typedef struct {
    mdl_t st;
    mdl_out_t out;
    int g_motion, g_motion_any, g_score, g_hwmax, g_hwp90, g_line, g_loiter;
    uint8_t g_active[L_NB];
    int ev_on, ev_off, cross_n, cross_dir[64], line_off, loiter_on, loiter_off;
} nw_t;

static void nw_init(nw_t *N) { memset(N, 0, sizeof *N); mdl_init(&N->st); }
static void nw_act(nw_t *N, const mdl_out_t *o)
{
    N->g_motion_any = o->motion_any;
    if (o->motion != N->g_motion) { N->g_motion = o->motion; if (o->motion) N->ev_on++; else N->ev_off++; }
    for (int i = 0; i < o->ncross; i++) { if (N->cross_n < 64) N->cross_dir[N->cross_n] = o->cross[i].dir; N->cross_n++; }
    if (o->line != N->g_line) { N->g_line = o->line; if (!o->line) N->line_off++; }
    if (o->loiter != N->g_loiter) { N->g_loiter = o->loiter; if (o->loiter) N->loiter_on++; else N->loiter_off++; }
}
static int nw_top(nw_t *N, int night_changed, int enabled, int hw, int64_t now)
{
    if (night_changed) mdl_night_switch(&N->st, now, hw);
    if (!enabled) { mdl_disabled(&N->st); N->g_score = 0; N->g_motion = 0; N->g_motion_any = 0; N->g_line = 0; N->g_loiter = 0; return 1; }
    return 0;
}
static void nw_hw_pass(nw_t *N, const uint16_t *g, const uint8_t *g_zone, int o0, int o1, const mdl_cfg_t *cfg, int night, int64_t t)
{
    uint8_t watch[L_HW_ROWS * 32], watch_all[L_HW_ROWS * 32];
    for (int r = 0; r < L_HW_ROWS; r++)
        for (int c = 0; c < 32; c++) {
            watch_all[r * 32 + c] = (uint8_t)!(r >= o0 && r <= o1);
            watch[r * 32 + c] = (uint8_t)(g_zone[(r * L_ROWS / L_HW_ROWS) * L_COLS + c] && watch_all[r * 32 + c]);
        }
    mdl_in_t in = { t, night, watch, watch_all };
    mdl_hw_frame(&N->st, cfg, g, &in, &N->out);
    N->g_hwmax = N->out.hwmax; N->g_hwp90 = N->out.hwp90;
    for (int i = 0; i < L_NB; i++) N->g_active[i] = 0;
    for (int r = 0; r < L_HW_ROWS; r++) for (int c = 0; c < 32; c++)
        if (N->out.active[r * 32 + c]) N->g_active[(r * L_ROWS / L_HW_ROWS) * L_COLS + c] = 1;
    N->g_score = N->out.score;
    nw_act(N, &N->out);
}
static int nw_sw_frame(nw_t *N, const float *cur, const uint8_t *g_zone, int osd_r0, int osd_r1, const mdl_cfg_t *cfg, int64_t now)
{
    uint8_t watch[L_NB], watch_all[L_NB];
    for (int i = 0; i < L_NB; i++) {
        int r = i / L_COLS;
        watch_all[i] = (uint8_t)!(r >= osd_r0 && r <= osd_r1);
        watch[i] = (uint8_t)(g_zone[i] && watch_all[i]);
    }
    mdl_in_t in = { now, 0, watch, watch_all };
    if (mdl_sw_frame(&N->st, cfg, cur, &in, &N->out)) return 1;
    for (int i = 0; i < L_NB; i++) N->g_active[i] = N->out.active[i];
    N->g_score = N->out.score;
    nw_act(N, &N->out);
    return 0;
}

static void cfg_base(mdl_cfg_t *c, int sens, int hold)
{
    memset(c, 0, sizeof *c);
    c->sens = sens; c->hold_s = hold;
}

/* ---- 1. regression: options off == original ------------------------------------ */
static int same(const leg_t *L, const nw_t *N, int sw, const char *what, long step)
{
    int ok = L->g_motion == N->g_motion && L->g_motion_any == N->g_motion_any && L->g_score == N->g_score &&
             !memcmp(L->g_active, N->g_active, L_NB) && L->ev_on == N->ev_on && L->ev_off == N->ev_off;
    /* and the decision state itself */
    ok = ok && L->quiet_until == N->st.quiet_until && L->last_hit == N->st.last_hit && L->last_any == N->st.last_any;
    if (sw) ok = ok && !memcmp(L->bg, N->st.bg, sizeof L->bg) && L->have_bg == N->st.have_bg && L->run == N->st.run && L->run_any == N->st.run_any;
    else ok = ok && L->g_hwmax == N->g_hwmax && L->g_hwp90 == N->g_hwp90 && L->hist == N->st.hist && L->hist_all == N->st.hist_all &&
              L->base50 == N->st.base50 && L->glob_n == N->st.glob_n;
    if (!ok)
        printf("  %s step %ld: legacy motion %d any %d score %d ev %d/%d | new %d %d %d ev %d/%d\n", what, step,
               L->g_motion, L->g_motion_any, L->g_score, L->ev_on, L->ev_off,
               N->g_motion, N->g_motion_any, N->g_score, N->ev_on, N->ev_off);
    return ok;
}

static void random_zone(uint8_t *z)
{
    int mode = rr(0, 3);
    if (mode == 0) { memset(z, 1, L_NB); return; }
    if (mode == 1) { for (int i = 0; i < L_NB; i++) z[i] = (uint8_t)(rnd() & 1); return; }
    memset(z, 0, L_NB);
    int c0 = rr(0, 31), c1 = rr(c0, 31), r0 = rr(0, 17), r1 = rr(r0, 17);
    for (int r = r0; r <= r1; r++) for (int c = c0; c <= c1; c++) z[r * 32 + c] = 1;
}

static void test_regression_sw(long steps)
{
    static leg_t L; static nw_t N;
    leg_init(&L); nw_init(&N);
    static float scene[L_NB], cur[L_NB];
    uint8_t zone[L_NB];
    memset(zone, 1, sizeof zone);
    for (int i = 0; i < L_NB; i++) scene[i] = (float)rr(20, 200);
    int sens = 50, hold = 3, osd0 = -1, osd1 = -1, enabled = 1, night = 0, bad = 0, motions = 0, lights = 0;
    int64_t now = 100000;
    int ox = 5, oy = 5, ow = 3, oh = 3, odx = 1, ody = 0, oamp = 60, obj = 0;
    for (long s = 0; s < steps && bad < 5; s++) {
        now += rr(200, 300);
        if (rr(0, 2000) == 0) now += rr(1000, 20000);
        if (rr(0, 300) == 0) sens = rr(1, 100);
        if (rr(0, 300) == 0) hold = rr(2, 6);
        if (rr(0, 400) == 0) random_zone(zone);
        if (rr(0, 400) == 0) { osd0 = rr(-1, 17); osd1 = osd0 < 0 ? -1 : osd0 + rr(0, 1); }
        if (rr(0, 1500) == 0) enabled = !enabled;
        int nc = rr(0, 1200) == 0;
        if (nc) night = !night;
        if (rr(0, 40) == 0) { obj = !obj; ox = rr(0, 28); oy = rr(0, 15); ow = rr(1, 5); oh = rr(1, 4); odx = rr(-2, 2); ody = rr(-1, 1); oamp = rr(-90, 90); }
        for (int i = 0; i < L_NB; i++) {        /* one rr() per statement: same sequence with every compiler */
            cur[i] = scene[i] + (float)rr(-3, 3);
            if (rr(0, 50) == 0) cur[i] += (float)rr(-30, 30);
        }
        if (obj) {
            ox += odx; oy += ody;
            if (ox < 0 || ox > 31) odx = -odx;
            if (oy < 0 || oy > 17) ody = -ody;
            for (int r = oy; r < oy + oh; r++) for (int c = ox; c < ox + ow; c++)
                if (r >= 0 && r < 18 && c >= 0 && c < 32) cur[r * 32 + c] += (float)oamp;
        }
        int lt = rr(0, 150);
        if (lt == 0) { lights++; for (int i = 0; i < L_NB; i++) scene[i] += 40; }                       /* whole picture */
        else if (lt == 1) { lights++; for (int i = 0; i < L_NB * 55 / 100; i++) scene[i] -= 35; }       /* 55 %: motion in the old rule */
        else if (lt == 2) { for (int i = 0; i < L_NB; i++) scene[i] = (float)rr(20, 200); }            /* new scene (> 60 %) */
        else if (lt < 6) { lights++; int k = rr(335, 355), o = rr(0, L_NB - 1);                         /* around the 60 % limit */
                           for (int i = 0; i < k; i++) scene[(o + i) % L_NB] += scene[(o + i) % L_NB] > 110 ? -80 : 80; }
        int c1 = leg_top(&L, nc, enabled, 0, now), c2 = nw_top(&N, nc, enabled, 0, now);
        if (!c1) leg_sw_frame(&L, cur, zone, osd0, osd1, sens, hold, now);
        mdl_cfg_t cfg; cfg_base(&cfg, sens, hold);
        if (!c2) nw_sw_frame(&N, cur, zone, osd0, osd1, &cfg, now);
        if (!same(&L, &N, 1, "software", s)) bad++;
        motions += L.ev_on;
    }
    CHECK(!bad, "software: options off differ from the original decision");
    CHECK(L.ev_on > steps / 1000, "software regression run saw too few motion events (%d)", L.ev_on);
    printf("regression software: %ld frames, %d motion events, %d light changes, %s\n", steps, L.ev_on, lights, bad ? "DIFFERENT" : "identical");
    (void)motions;
}

static void test_regression_hw(long steps)
{
    static leg_t L; static nw_t N;
    leg_init(&L); nw_init(&N);
    uint16_t g[L_HW_ROWS * 32];
    uint8_t zone[L_NB];
    memset(zone, 1, sizeof zone);
    int sens = 50, hold = 3, o0 = -1, o1 = -1, enabled = 1, night = 0, bad = 0, floor = 300;
    int64_t now = 100000;
    int ox = 5, oy = 5, ow = 4, oh = 4, amp = 6000, obj = 0, glob = 0;
    for (long s = 0; s < steps && bad < 5; s++) {
        now += rr(240, 260);
        if (rr(0, 300) == 0) { sens = rr(1, 100); if (sens < 30) sens = rr(1, 100); }   /* mostly >= 30: regions of 4-22 cells */
        if (rr(0, 300) == 0) hold = rr(2, 6);
        if (rr(0, 400) == 0) random_zone(zone);
        if (rr(0, 400) == 0) { o0 = rr(-1, 23); o1 = o0 < 0 ? -1 : o0 + rr(0, 2); }
        if (rr(0, 1500) == 0) enabled = !enabled;
        if (rr(0, 500) == 0) floor = rr(50, 1500);
        int nc = rr(0, 1200) == 0;
        if (nc) night = !night;
        if (rr(0, 30) == 0) {
            static const int big[][2] = { { 25, 12 }, { 20, 15 }, { 15, 20 }, { 30, 10 }, { 31, 10 }, { 19, 16 } };
            obj = !obj; ox = rr(0, 30); oy = rr(0, 22); ow = rr(1, 8); oh = rr(1, 8); amp = rr(1000, 15000);
            if (rr(0, 3) == 0) { int b = rr(0, 5); ow = big[b][0]; oh = big[b][1]; ox = rr(0, 32 - ow); oy = rr(0, 24 - oh); }
        }
        if (rr(0, 200) == 0) glob = rr(3, 60);
        int c1 = leg_top(&L, nc, enabled, 1, now), c2 = nw_top(&N, nc, enabled, 1, now);
        for (int pass = 0; pass < 2; pass++) {
            int64_t t = now + pass * 100 + rr(0, 3);
            for (int i = 0; i < L_HW_ROWS * 32; i++) {
                g[i] = (uint16_t)(floor / 2 + rr(0, floor));
                if (rr(0, 100) == 0) g[i] = (uint16_t)(g[i] + rr(0, 2500));
            }
            if (glob) { glob--; for (int i = 0; i < L_HW_ROWS * 32; i++) g[i] = (uint16_t)(g[i] + 2500 + rr(0, 4000)); }
            if (obj) {
                if (ow < 15) {
                    ox += rr(-1, 1); oy += rr(-1, 1);
                    ox = ox < -2 ? -2 : ox > 30 ? 30 : ox; oy = oy < -2 ? -2 : oy > 22 ? 22 : oy;
                }
                for (int r = oy; r < oy + oh; r++) for (int c = ox; c < ox + ow; c++)
                    if (r >= 0 && r < 24 && c >= 0 && c < 32 && (ow < 15 || r == oy || r == oy + oh - 1 || c == ox || c == ox + ow - 1))
                        g[r * 32 + c] = (uint16_t)(amp / 2 + rr(0, amp));    /* big ones: outline only (bounding box ~300) */
            }
            if (rr(0, 500) == 0) for (int i = 0; i < L_HW_ROWS * 32; i++) g[i] = (uint16_t)rnd();   /* garbage */
            if (!c1) leg_hw_pass(&L, g, zone, o0, o1, sens, hold, night, t);
            mdl_cfg_t cfg; cfg_base(&cfg, sens, hold);
            if (!c2) nw_hw_pass(&N, g, zone, o0, o1, &cfg, night, t);
            if (!same(&L, &N, 0, "hardware", s)) bad++;
            if (c1) break;
        }
    }
    CHECK(!bad, "hardware: options off differ from the original decision");
    CHECK(L.ev_on > steps / 1000, "hardware regression run saw too few motion events (%d)", L.ev_on);
    printf("regression hardware: %ld polls, %d motion events, %s\n", steps, L.ev_on, bad ? "DIFFERENT" : "identical");
}

/* ---- synthetic scenes (software source, flat background) ------------------------- */
#define BG 60.0f
typedef struct { nw_t N; int64_t now; uint8_t zone[L_NB]; mdl_cfg_t cfg; } scene_t;

static void sc_init(scene_t *S, const mdl_cfg_t *cfg)
{
    nw_init(&S->N);
    S->now = 1000000;
    memset(S->zone, 1, sizeof S->zone);
    S->cfg = *cfg;
}
/* one frame with an object of w x h cells at column c, row r (amp above the background) */
static void sc_frame(scene_t *S, int c, int r, int w, int h, float amp)
{
    float cur[L_NB];
    for (int i = 0; i < L_NB; i++) cur[i] = BG;
    for (int y = r; y < r + h; y++) for (int x = c; x < c + w; x++)
        if (y >= 0 && y < 18 && x >= 0 && x < 32) cur[y * 32 + x] += amp;
    S->now += 250;
    nw_sw_frame(&S->N, cur, S->zone, -1, -1, &S->cfg, S->now);
}
static void sc_empty(scene_t *S, int n) { for (int i = 0; i < n; i++) sc_frame(S, 0, 0, 0, 0, 0); }

static void test_line(void)
{
    mdl_cfg_t c; cfg_base(&c, 80, 2);              /* need = 9 cells: the 3x3 object is motion */
    c.line_on = 1; c.line[0] = 500; c.line[1] = 0; c.line[2] = 500; c.line[3] = 1000;     /* vertical, top -> bottom */
    static scene_t S; sc_init(&S, &c);
    sc_empty(&S, 4);
    for (int x = 2; x <= 27; x++) sc_frame(&S, x, 7, 3, 3, 100);       /* left -> right */
    CHECK(S.N.cross_n == 1 && S.N.cross_dir[0] == 1, "left->right: %d crossings, dir %d (want 1, 1 = toward the left side of top->bottom)", S.N.cross_n, S.N.cross_dir[0]);
    CHECK(S.N.ev_on == 1, "the object is also motion");
    sc_empty(&S, 16);
    CHECK(!S.N.g_line && S.N.line_off == 1, "line state off after hold");
    for (int x = 27; x >= 2; x--) sc_frame(&S, x, 7, 3, 3, 100);       /* right -> left */
    CHECK(S.N.cross_n == 2 && S.N.cross_dir[1] == 2, "right->left: %d crossings, dir %d", S.N.cross_n, S.N.cross_dir[1]);
    /* moving parallel to the line, and passing beyond the end of a short line: nothing */
    sc_empty(&S, 16);
    for (int y = 0; y <= 15; y++) sc_frame(&S, 10, y, 3, 3, 100);
    CHECK(S.N.cross_n == 2, "parallel movement crossed (%d)", S.N.cross_n);
    S.cfg.line[3] = 300;                                                 /* line only in the top 30 % */
    sc_empty(&S, 16);
    for (int x = 2; x <= 27; x++) sc_frame(&S, x, 12, 3, 3, 100);
    CHECK(S.N.cross_n == 2, "passing below the end of the line counted (%d)", S.N.cross_n);
    /* direction filter: only toward the right side */
    S.cfg.line[3] = 1000; S.cfg.line_dir = 2;
    sc_empty(&S, 16);
    for (int x = 2; x <= 27; x++) sc_frame(&S, x, 7, 3, 3, 100);
    CHECK(S.N.cross_n == 2, "filtered direction counted (%d)", S.N.cross_n);
    sc_empty(&S, 16);
    for (int x = 27; x >= 2; x--) sc_frame(&S, x, 7, 3, 3, 100);
    CHECK(S.N.cross_n == 3 && S.N.cross_dir[2] == 2, "wanted direction missed (%d)", S.N.cross_n);
    /* a diagonal line, object moving down */
    S.cfg.line_dir = 0; S.cfg.line[0] = 0; S.cfg.line[1] = 300; S.cfg.line[2] = 1000; S.cfg.line[3] = 700;
    sc_empty(&S, 16);
    for (int y = 0; y <= 15; y++) sc_frame(&S, 14, y, 3, 3, 100);
    CHECK(S.N.cross_n == 4 && S.N.cross_dir[3] == 2, "diagonal: %d crossings dir %d (down across left->right line = right side)", S.N.cross_n, S.N.cross_dir[3]);
    /* hesitating on the line (centroid within the dead band): no repeated events */
    sc_empty(&S, 16);
    S.cfg.line[0] = 500; S.cfg.line[1] = 0; S.cfg.line[2] = 500; S.cfg.line[3] = 1000;
    int before = S.N.cross_n;
    for (int i = 0; i < 20; i++) sc_frame(&S, 14 + (i & 1), 7, 4, 3, 100);   /* centroid 500 +- 15.6 */
    CHECK(S.N.cross_n - before <= 1, "jitter on the line gave %d crossings", S.N.cross_n - before);
    printf("line crossing: ok (%d crossings)\n", S.N.cross_n);
}

static void test_min_size(void)
{
    mdl_cfg_t c; cfg_base(&c, 100, 2);
    static scene_t S;
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 8; i++) sc_frame(&S, 5 + i, 5, 2, 1, 50);
    CHECK(S.N.ev_on == 1, "2-cell object without min_size: motion expected");
    c.min_size = 3;
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 8; i++) sc_frame(&S, 5 + i, 5, 2, 1, 50);
    CHECK(S.N.ev_on == 0, "2-cell object with min_size 3: no motion");
    c.min_size = 4;
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 8; i++) sc_frame(&S, 5 + i, 5, 2, 2, 50);
    CHECK(S.N.ev_on == 1, "2x2 object with min_size 4: motion expected");
    /* scattered single cells (noise) */
    float cur[L_NB];
    for (int pass = 0; pass < 2; pass++) {
        c.min_size = pass ? 2 : 0;
        sc_init(&S, &c); sc_empty(&S, 4);
        for (int f = 0; f < 12; f++) {
            for (int i = 0; i < L_NB; i++) cur[i] = BG;
            for (int k = 0; k < 6; k++) { int y = rr(0, 8) * 2, x = rr(0, 15) * 2; cur[y * 32 + x] += 40; }   /* never adjacent */
            S.now += 250;
            nw_sw_frame(&S.N, cur, S.zone, -1, -1, &S.cfg, S.now);
        }
        CHECK(pass ? S.N.ev_on == 0 : S.N.ev_on == 1, "scattered cells, min_size %d: %d motion events", c.min_size, S.N.ev_on);
    }
    printf("minimum size: ok\n");
}

static void hw_frame(nw_t *N, const mdl_cfg_t *c, int64_t t, int night, int bx, int by, int bw, int bh, int amp, int floor, int frac_hi)
{
    uint16_t g[L_HW_ROWS * 32];
    uint8_t zone[L_NB];
    memset(zone, 1, sizeof zone);
    for (int i = 0; i < L_HW_ROWS * 32; i++) g[i] = (uint16_t)(floor + (i * 37 % 97));
    for (int i = 0; i < L_HW_ROWS * 32 * frac_hi / 100; i++) g[(i * 7) % (L_HW_ROWS * 32)] = 6000;
    for (int y = by; y < by + bh; y++) for (int x = bx; x < bx + bw; x++)
        if (y >= 0 && y < 24 && x >= 0 && x < 32) g[y * 32 + x] = (uint16_t)amp;
    nw_hw_pass(N, g, zone, -1, -1, c, night, t);
}

static void test_persist(void)
{
    mdl_cfg_t c; cfg_base(&c, 50, 2);
    static scene_t S;
    /* one-frame flicker: ignored with and without the option (2 frames needed already) */
    sc_init(&S, &c); sc_empty(&S, 4);
    sc_frame(&S, 10, 5, 5, 4, 80); sc_empty(&S, 4);
    CHECK(S.N.ev_on == 0, "one-frame flicker gave motion");
    /* 2 frames (500 ms): original rule triggers, persist 1000 ms does not */
    sc_init(&S, &c); sc_empty(&S, 4);
    sc_frame(&S, 10, 5, 5, 4, 80); sc_frame(&S, 10, 5, 5, 4, 80); sc_empty(&S, 4);
    CHECK(S.N.ev_on == 1, "2 frames without persist: motion expected");
    c.persist_ms = 1000;
    sc_init(&S, &c); sc_empty(&S, 4);
    sc_frame(&S, 10, 5, 5, 4, 80); sc_frame(&S, 10, 5, 5, 4, 80); sc_frame(&S, 10, 5, 5, 4, 80); sc_empty(&S, 4);
    CHECK(S.N.ev_on == 0, "750 ms change with persist 1000: no motion");
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 6; i++) sc_frame(&S, 10 + i, 5, 5, 4, 80);
    CHECK(S.N.ev_on == 1, "1.5 s of movement with persist 1000: motion expected");
    /* hardware, day: the original rule fires on a single poll */
    static nw_t N;
    cfg_base(&c, 50, 2);
    nw_init(&N);
    int64_t t = 1000000;
    for (int i = 0; i < 20; i++) hw_frame(&N, &c, t += 100, 0, 0, 0, 0, 0, 0, 300, 0);
    hw_frame(&N, &c, t += 100, 0, 10, 10, 4, 4, 8000, 300, 0);
    for (int i = 0; i < 5; i++) hw_frame(&N, &c, t += 100, 0, 0, 0, 0, 0, 0, 300, 0);
    CHECK(N.ev_on == 1, "hardware single-poll flicker: the original rule should fire");
    c.persist_ms = 300;
    nw_init(&N);
    for (int i = 0; i < 20; i++) hw_frame(&N, &c, t += 100, 0, 0, 0, 0, 0, 0, 300, 0);
    hw_frame(&N, &c, t += 100, 0, 10, 10, 4, 4, 8000, 300, 0);
    for (int i = 0; i < 5; i++) hw_frame(&N, &c, t += 100, 0, 0, 0, 0, 0, 0, 300, 0);
    CHECK(N.ev_on == 0, "hardware single-poll flicker with persist 300: no motion");
    for (int i = 0; i < 5; i++) hw_frame(&N, &c, t += 100, 0, 10 + i, 10, 4, 4, 8000, 300, 0);
    CHECK(N.ev_on == 1, "hardware 500 ms of movement with persist 300: motion expected");
    printf("brief changes: ok\n");
}

static void test_light(void)
{
    mdl_cfg_t c; cfg_base(&c, 50, 2);
    static scene_t S;
    float cur[L_NB];
    for (int opt = 0; opt < 2; opt++) {
        /* a lamp lights half of the room (50 % of the cells, < the original 60 %) */
        c.light_pct = opt ? 40 : 0;
        sc_init(&S, &c); sc_empty(&S, 4);
        for (int f = 0; f < 8; f++) {
            for (int i = 0; i < L_NB; i++) cur[i] = BG + (i % 32 < 16 ? 60.0f : 0.0f);
            S.now += 250; nw_sw_frame(&S.N, cur, S.zone, -1, -1, &S.cfg, S.now);
        }
        CHECK(opt ? S.N.ev_on == 0 : S.N.ev_on == 1, "half-picture light, light=%d: %d motion events", c.light_pct, S.N.ev_on);
        /* whole picture: both suppress */
        sc_init(&S, &c); sc_empty(&S, 4);
        for (int f = 0; f < 8; f++) {
            for (int i = 0; i < L_NB; i++) cur[i] = BG + 70.0f;
            S.now += 250; nw_sw_frame(&S.N, cur, S.zone, -1, -1, &S.cfg, S.now);
        }
        CHECK(S.N.ev_on == 0, "whole-picture light, light=%d gave motion", c.light_pct);
        /* auto exposure step: everything +8, bright 20 % +25 */
        sc_init(&S, &c); sc_empty(&S, 4);
        for (int f = 0; f < 8; f++) {
            for (int i = 0; i < L_NB; i++) cur[i] = BG + (i % 5 == 0 ? 25.0f : 8.0f);
            S.now += 250; nw_sw_frame(&S.N, cur, S.zone, -1, -1, &S.cfg, S.now);
        }
        CHECK(opt ? S.N.ev_on == 0 : S.N.ev_on == 1, "brightness step, light=%d: %d motion events", c.light_pct, S.N.ev_on);
        /* and a real object after the quiet time is still motion */
        sc_empty(&S, 16);
        int before = S.N.ev_on;
        for (int i = 0; i < 6; i++) sc_frame(&S, 5 + i, 5, 5, 4, 90);
        CHECK(S.N.ev_on == before + 1, "object after a light change missed (light=%d)", c.light_pct);
    }
    /* hardware: a flash (40 % of the cells) then the exposure settles with a blob */
    static nw_t N;
    for (int opt = 0; opt < 2; opt++) {
        cfg_base(&c, 50, 2); c.light_pct = opt ? 30 : 0;
        nw_init(&N);
        int64_t t = 1000000;
        for (int i = 0; i < 30; i++) hw_frame(&N, &c, t += 100, 0, 0, 0, 0, 0, 0, 300, 0);
        hw_frame(&N, &c, t += 100, 0, 0, 0, 0, 0, 0, 300, 40);
        for (int i = 0; i < 3; i++) hw_frame(&N, &c, t += 100, 0, 8, 8, 4, 3, 6000, 300, 0);
        CHECK(opt ? N.ev_on == 0 : N.ev_on == 1, "hardware flash, light=%d: %d motion events", c.light_pct, N.ev_on);
        for (int i = 0; i < 40; i++) hw_frame(&N, &c, t += 100, 0, 0, 0, 0, 0, 0, 300, 0);
        int before = N.ev_on;
        for (int i = 0; i < 3; i++) hw_frame(&N, &c, t += 100, 0, 8 + i, 8, 4, 3, 6000, 300, 0);
        CHECK(N.ev_on == before + 1, "hardware object after the quiet time missed (light=%d)", c.light_pct);
    }
    printf("light changes: ok\n");
}

static void test_loiter(void)
{
    mdl_cfg_t c; cfg_base(&c, 50, 2);
    c.nzones = 1;
    c.zone[0] = (mdl_zone_t){ 20, 4, 27, 12, 50 };
    c.loiter_s = 5; c.loiter_zone = 1;
    static scene_t S;
    /* passing through the zone: ~2 s inside */
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int x = 12; x <= 31; x++) sc_frame(&S, x, 7, 3, 3, 100);
    sc_empty(&S, 8);
    CHECK(S.N.loiter_on == 0, "passing through gave loitering");
    CHECK(S.N.ev_on == 1, "passing through is motion");
    /* staying (moving a little) for 8 s */
    sc_init(&S, &c); sc_empty(&S, 4);
    int64_t t0 = S.now, at = 0;
    for (int f = 0; f < 32; f++) { sc_frame(&S, 22 + (f & 1), 7 + ((f >> 1) & 1), 3, 3, 100); if (S.N.g_loiter && !at) at = S.now - t0; }
    CHECK(S.N.loiter_on == 1, "staying 8 s in the zone: loitering expected");
    CHECK(at >= 5000 && at <= 6000, "loitering after %lld ms (want 5-6 s)", (long long)at);
    sc_empty(&S, 60);                    /* the background needs ~9 s to forget a 8 s visitor (ghost) */
    CHECK(S.N.loiter_off == 1 && !S.N.g_loiter, "loitering off after the object left");
    /* staying 8 s OUTSIDE the zone: nothing */
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int f = 0; f < 32; f++) sc_frame(&S, 5 + (f & 1), 7, 3, 3, 100);
    CHECK(S.N.loiter_on == 0, "loitering outside the zone");
    /* zone 0: the watched area */
    c.loiter_zone = 0;
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int f = 0; f < 32; f++) sc_frame(&S, 5 + (f & 1), 7, 3, 3, 100);
    CHECK(S.N.loiter_on == 1, "loitering in the watched area expected");
    printf("loitering: ok\n");
}

static void test_zone_sens(void)
{
    mdl_cfg_t c; cfg_base(&c, 50, 2);
    static scene_t S;
    /* without zones (sensitivity 50, threshold 22.5): a change of 20 is nothing */
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 6; i++) sc_frame(&S, 20, 5, 2, 2, 20);
    CHECK(S.N.ev_on == 0, "faint object at sensitivity 50 gave motion");
    c.nzones = 2;
    c.zone[0] = (mdl_zone_t){ 0, 0, 15, 17, 10 };       /* left: insensitive */
    c.zone[1] = (mdl_zone_t){ 16, 0, 31, 17, 100 };     /* right: very sensitive */
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 6; i++) sc_frame(&S, 20, 5, 2, 2, 20);
    CHECK(S.N.ev_on == 1, "faint object in the sensitive zone: motion expected");
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 6; i++) sc_frame(&S, 4, 5, 2, 2, 20);
    CHECK(S.N.ev_on == 0, "faint object in the insensitive zone gave motion");
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 6; i++) sc_frame(&S, 4, 5, 3, 3, 45);          /* 9 cells < need 15 */
    CHECK(S.N.ev_on == 0, "small object in the insensitive zone gave motion");
    sc_init(&S, &c); sc_empty(&S, 4);
    for (int i = 0; i < 6; i++) sc_frame(&S, 4, 5, 4, 4, 45);          /* 16 cells */
    CHECK(S.N.ev_on == 1, "large object in the insensitive zone: motion expected");
    /* hardware: zone sensitivity = minimum region size */
    static nw_t N;
    for (int z = 0; z < 2; z++) {
        nw_init(&N);
        int64_t t = 1000000;
        for (int i = 0; i < 20; i++) hw_frame(&N, &c, t += 100, 0, 0, 0, 0, 0, 0, 300, 0);
        for (int i = 0; i < 3; i++) hw_frame(&N, &c, t += 100, 0, z ? 22 : 6, 10, 2, 2, 8000, 300, 0);
        CHECK(z ? N.ev_on == 1 : N.ev_on == 0, "hardware 2x2 in zone %d (sens %d): %d events", z + 1, c.zone[z].sens, N.ev_on);
    }
    printf("per-zone sensitivity: ok\n");
}

static void test_parsers(void)
{
    mdl_zone_t z[MDL_MAX_ZONES];
    int l[4];
    CHECK(mdl_parse_zones("", z, 8) == 0, "empty zones");
    CHECK(mdl_parse_zones("0,0,31,17,50", z, 8) == 1 && z[0].c1 == 31 && z[0].sens == 50, "one zone");
    CHECK(mdl_parse_zones("1,2,3,4,5;6,7,8,9,100", z, 8) == 2 && z[1].r1 == 9, "two zones");
    const char *bad_z[] = { "0,0,32,17,50", "0,0,31,18,50", "0,0,31,17,0", "0,0,31,17,101", "5,0,4,17,50", "0,5,31,4,50",
                            "0,0,31,17", "0,0,31,17,50;", ";", "a,0,1,1,1", "0,0,1,1,1,", "00000,0,1,1,1", "-1,0,1,1,1",
                            "0,0,1,1,1;0,0,1,1,1;0,0,1,1,1;0,0,1,1,1;0,0,1,1,1;0,0,1,1,1;0,0,1,1,1;0,0,1,1,1;0,0,1,1,1", " 0,0,1,1,1" };
    for (size_t i = 0; i < sizeof bad_z / sizeof bad_z[0]; i++) CHECK(mdl_parse_zones(bad_z[i], z, 8) < 0, "zones accepted: %s", bad_z[i]);
    CHECK(mdl_parse_line("", l) == 0, "empty line");
    CHECK(mdl_parse_line("500,0,500,1000", l) == 1 && l[3] == 1000, "line");
    const char *bad_l[] = { "500,0,500", "500,0,500,1001", "0,0,10,10", "1,2,3,4,5", "a,0,0,0", "500,0,500,1000,", "-1,0,500,500" };
    for (size_t i = 0; i < sizeof bad_l / sizeof bad_l[0]; i++) CHECK(mdl_parse_line(bad_l[i], l) < 0, "line accepted: %s", bad_l[i]);
    printf("parsers: ok\n");
}

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }

static void test_cost(void)
{
    /* worst cases with every option on: checkerboard (most blobs), all cells (one big flood) */
    static nw_t N;
    mdl_cfg_t c; cfg_base(&c, 100, 2);
    c.nzones = 8;
    for (int k = 0; k < 8; k++) c.zone[k] = (mdl_zone_t){ k * 4, 0, k * 4 + 3, 17, 10 + k * 10 };
    c.min_size = 1; c.persist_ms = 100; c.light_pct = 90; c.line_on = 1;
    c.line[0] = 0; c.line[1] = 0; c.line[2] = 1000; c.line[3] = 1000; c.loiter_s = 1;
    nw_init(&N);
    uint8_t zone[L_NB]; memset(zone, 1, sizeof zone);
    float cur[L_NB];
    uint16_t g[L_HW_ROWS * 32];
    double worst_sw = 0, worst_hw = 0;
    int64_t t = 1000000;
    for (int f = 0; f < 400; f++) {
        for (int i = 0; i < L_NB; i++) cur[i] = ((i / 32 + i + f) & 1) ? 200.0f : 10.0f;
        double a = now_us();
        nw_sw_frame(&N, cur, zone, -1, -1, &c, t += 250);
        double d = now_us() - a;
        if (d > worst_sw) worst_sw = d;
        for (int i = 0; i < L_HW_ROWS * 32; i++) g[i] = (f & 2) ? (((i / 32 + i + f) & 1) ? 60000 : 1000) : 9000;
        a = now_us();
        nw_hw_pass(&N, g, zone, -1, -1, &c, f & 1, t += 100);
        d = now_us() - a;
        if (d > worst_hw) worst_hw = d;
    }
    printf("cost: worst frame on this PC %.0f us (software) / %.0f us (hardware), state %zu bytes\n", worst_sw, worst_hw, sizeof(mdl_t));
    CHECK(sizeof(mdl_t) < 16384, "state too big");
}

/* hardware log in the md_replay format: an object crossing the middle */
static int write_log(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    int64_t t = 0;
    for (int fr = 0; fr < 80; fr++) {
        int x = fr < 20 ? -10 : fr - 20;           /* enters at frame 20, 1 cell per poll */
        fprintf(f, "%lld {\"hdr\":0,\"rows\":[", (long long)(t += 100));
        for (int r = 0; r < 24; r++) {
            fprintf(f, "%s[", r ? "," : "");
            for (int c = 0; c < 32; c++) {
                int v = 200 + (r * 7 + c * 13 + fr * 5) % 150;
                if (c >= x && c < x + 3 && r >= 10 && r < 14) v = 7000;
                fprintf(f, "%s%d", c ? "," : "", v);
            }
            fprintf(f, "]");
        }
        fprintf(f, "]}\n");
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--write-log")) return write_log(argv[2]);
    long n = argc > 1 ? atol(argv[1]) : 40000;
    test_parsers();
    test_regression_sw(n);
    test_regression_hw(n);
    test_line();
    test_min_size();
    test_persist();
    test_light();
    test_loiter();
    test_zone_sens();
    test_cost();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
