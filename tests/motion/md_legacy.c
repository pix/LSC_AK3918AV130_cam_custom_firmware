/* The motion decision of md.c before md_logic.c (git 679bdc8), copied as
 * literally as possible: the md_thread locals and statics became the fields
 * of leg_t, events_motion() calls are counted. Hardware access (frames,
 * ISP statistic, overlay, night flag, clock) comes in as arguments.
 * One deliberate change: when a hardware frame was a global change the old
 * code read act[] without filling it (uninitialised stack; in practice the
 * previous pass's cells). Here it is cleared, as md_logic does. That only
 * affected the score and the red cells on the web page, never an event. */
#include <stdint.h>
#include <string.h>

#define L_COLS 32
#define L_ROWS 18
#define L_NB (L_COLS * L_ROWS)
#define L_HW_ROWS 24

typedef struct {
    int g_motion, g_motion_any, g_score, g_hwmax, g_hwp90;
    uint8_t g_active[L_NB];
    float bg[L_NB];
    int have_bg, run, run_any;
    int64_t last_hit, quiet_until, last_any;
    int hist, base50, glob_n, hist_all;
    int ev_on, ev_off;                  /* events_motion(1) / (0) calls */
} leg_t;

static void leg_init(leg_t *L) { memset(L, 0, sizeof *L); L->base50 = -1; }
static void leg_events_motion(leg_t *L, int on) { if (on) L->ev_on++; else L->ev_off++; }

static const uint8_t leg_min_cells[101] = {
    50,49,48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,32,31,30,29,28,27,27,26,25,24,24,23,22,21,21,20,19,18,18,17,16,15,15,14,
    14,13,13,12,12,12,12,12,12,12,12,11,11,11,11,11,11,11,11,10,10,10,10,9,9,9,9,9,9,9,9,8,8,8,8,8,8,8,8,7,7,7,7,6,6,6,6,6,6,6,6,
    5,5,5,5,4,4,4,4 };

static int leg_hw_detect(const uint16_t *g, const uint8_t *watch24, int sens, uint8_t *act, int hi, int lo)
{
    static uint8_t seen[L_HW_ROWS * 32];
    static uint16_t stack[L_HW_ROWS * 32];
    memset(seen, 0, sizeof seen);
    memset(act, 0, L_HW_ROWS * 32);
    int minc = leg_min_cells[sens < 0 ? 0 : sens > 100 ? 100 : sens], found = 0;
    for (int i = 0; i < L_HW_ROWS * 32; i++) {
        if (seen[i] || g[i] < hi) continue;
        int sp = 0, r0 = 99, r1 = -1, c0 = 99, c1 = -1, watched = 0, n = 0;
        static uint16_t cells[L_HW_ROWS * 32];
        stack[sp++] = (uint16_t)i; seen[i] = 1;
        while (sp) {
            int k = stack[--sp], r = k / 32, c = k % 32;
            cells[n++] = (uint16_t)k;
            if (r < r0) r0 = r;
            if (r > r1) r1 = r;
            if (c < c0) c0 = c;
            if (c > c1) c1 = c;
            watched += watch24[k];
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    int rr = r + dr, cc = c + dc;
                    if (rr < 0 || rr >= L_HW_ROWS || cc < 0 || cc >= 32) continue;
                    int kk = rr * 32 + cc;
                    if (!seen[kk] && g[kk] >= lo) { seen[kk] = 1; stack[sp++] = (uint16_t)kk; }
                }
        }
        int area = (r1 - r0 + 1) * (c1 - c0 + 1);
        if (area >= minc && area <= 300 && watched >= minc) {
            found = 1;
            for (int j = 0; j < n; j++) act[cells[j]] = 1;
        }
    }
    return found;
}

/* the top of the loop: day/night switch and the disabled branch; returns 1 = continue */
static int leg_top(leg_t *L, int night_changed, int enabled, int s_hw, int64_t now)
{
    if (night_changed) { L->have_bg = 0; L->quiet_until = now + (s_hw ? 10000 : 5000); }
    if (!enabled) { L->have_bg = 0; L->g_score = 0; L->g_motion = 0; L->g_motion_any = 0; return 1; }
    return 0;
}

/* one hardware pass. o0/o1: overlay rows on the 24-row grid (-1 = none). */
static void leg_hw_pass(leg_t *L, const uint16_t *g, const uint8_t *g_zone, int o0, int o1,
                        int s_sens, int s_hold, int night, int64_t t)
{
    int mx = 0;
    for (int i = 0; i < L_HW_ROWS * 32; i++) if (g[i] > mx) mx = g[i];
    L->g_hwmax = mx;
    uint8_t watch[L_HW_ROWS * 32], act[L_HW_ROWS * 32], watch_all[L_HW_ROWS * 32], act_all[L_HW_ROWS * 32];
    for (int r = 0; r < L_HW_ROWS; r++)
        for (int c = 0; c < 32; c++) {
            watch_all[r * 32 + c] = (uint8_t)!(r >= o0 && r <= o1);
            watch[r * 32 + c] = (uint8_t)(g_zone[(r * L_ROWS / L_HW_ROWS) * L_COLS + c] && watch_all[r * 32 + c]);
        }
    static uint16_t hcnt[256];
    memset(hcnt, 0, sizeof hcnt);
    for (int i = 0; i < L_HW_ROWS * 32; i++) hcnt[g[i] >= 65280 ? 255 : g[i] >> 8]++;
    int acc = 0, p50 = -1, p90 = 0;
    for (int b = 0; b < 256; b++) {
        acc += hcnt[b];
        if (p50 < 0 && acc >= L_HW_ROWS * 32 / 2) p50 = b * 256 + 128;
        if (acc >= L_HW_ROWS * 32 * 9 / 10) { p90 = b * 256 + 128; break; }
    }
    int hi = p90 * 7 / 2; if (hi < 1800) hi = 1800;
    int lo = hi / 2;
    L->g_hwp90 = p90;
    if (L->base50 < 0) L->base50 = p50;
    int global = p50 > L->base50 * 3 + 400;
    if (global) { if (++L->glob_n > 50) { L->base50 = p50; L->glob_n = 0; } }
    else { L->glob_n = 0; L->base50 += (p50 - L->base50) / 32; }
    memset(act, 0, sizeof act);                     /* see the note at the top */
    int hit = global ? 0 : leg_hw_detect(g, watch, s_sens, act, hi, lo);
    for (int i = 0; i < L_NB; i++) L->g_active[i] = 0;
    int score = 0;
    for (int r = 0; r < L_HW_ROWS; r++) for (int c = 0; c < 32; c++)
        if (act[r * 32 + c]) { L->g_active[(r * L_ROWS / L_HW_ROWS) * L_COLS + c] = 1; score++; }
    L->g_score = score;
    L->hist = ((L->hist << 1) | hit) & 7;
    int ok = night ? __builtin_popcount(L->hist) >= 2 : hit;
    if (t >= L->quiet_until && ok) { L->last_hit = t; if (!L->g_motion) { L->g_motion = 1; leg_events_motion(L, 1); } }
    if (L->g_motion && t - L->last_hit > (int64_t)s_hold * 1000) { L->g_motion = 0; leg_events_motion(L, 0); }
    int hit_all = global ? 0 : leg_hw_detect(g, watch_all, s_sens, act_all, hi, lo);
    L->hist_all = ((L->hist_all << 1) | hit_all) & 7;
    if (t >= L->quiet_until && (night ? __builtin_popcount(L->hist_all) >= 2 : hit_all)) { L->last_any = t; L->g_motion_any = 1; }
    if (L->g_motion_any && t - L->last_any > (int64_t)s_hold * 1000) L->g_motion_any = 0;
}

/* one software frame: cur = block means; osd_r0/r1 overlay rows on the 18-row grid */
static void leg_sw_frame(leg_t *L, const float *cur, const uint8_t *g_zone, int osd_r0, int osd_r1,
                         int s_sens, int s_hold, int64_t now)
{
    float *bg = L->bg;
    if (!L->have_bg) { for (int i = 0; i < L_NB; i++) bg[i] = cur[i]; L->have_bg = 1; return; }
    float thr = 40.0f - 0.35f * (float)s_sens;
    int watched = 0, changed = 0, changed_all = 0, watched_any = 0, changed_any = 0;
    for (int i = 0; i < L_NB; i++) {
        float d = cur[i] - bg[i];
        if (d < 0) d = -d;
        int hit = d > thr;
        changed_all += hit;
        int row = i / L_COLS, outside_osd = !(row >= osd_r0 && row <= osd_r1), z = g_zone[i] && outside_osd;
        if (outside_osd) { watched_any++; changed_any += hit; }
        if (z) { watched++; changed += hit; }
        L->g_active[i] = (uint8_t)(hit && z);
        bg[i] += (cur[i] - bg[i]) * (hit ? 0.02f : 0.08f);
    }
    L->g_score = changed;
    if (changed_all > L_NB * 6 / 10) {
        for (int i = 0; i < L_NB; i++) bg[i] = cur[i];
        L->quiet_until = now + 2000; L->run = 0;
        return;
    }
    int need = watched * (60 - (s_sens * 56) / 100) / 1000;
    if (need < 1) need = 1;
    if (now >= L->quiet_until && watched && changed >= need) {
        if (++L->run >= 2) { L->last_hit = now; if (!L->g_motion) { L->g_motion = 1; leg_events_motion(L, 1); } }
    } else L->run = 0;
    if (L->g_motion && now - L->last_hit > (int64_t)s_hold * 1000) { L->g_motion = 0; leg_events_motion(L, 0); }
    int need_any = watched_any * (60 - (s_sens * 56) / 100) / 1000;
    if (need_any < 1) need_any = 1;
    if (now >= L->quiet_until && changed_any >= need_any) { if (++L->run_any >= 2) { L->last_any = now; L->g_motion_any = 1; } }
    else L->run_any = 0;
    if (L->g_motion_any && now - L->last_any > (int64_t)s_hold * 1000) L->g_motion_any = 0;
}
