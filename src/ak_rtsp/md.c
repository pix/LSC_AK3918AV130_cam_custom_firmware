#include "md.h"
#include "events.h"
#include "osd.h"
#include "isp.h"
#include "night.h"
#include "ae.h"
#include "vi.h"
#include "monotime.h"
#include "ctrl_params.h"
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>

#define NB (MD_COLS * MD_ROWS)
static int s_enabled, s_sens = 50, s_hold = 10, s_ignore_osd = 1, s_hw;   /* s_hw: hardware statistics */
static volatile int g_hwmax, g_hwp90;

/* ---- hardware motion statistics (stock libak_md / libmrd, see RE notes) ----
 * ISP 3D-NR statistic: u16[24][32] per-block motion values at payload+2,
 * refreshed every frame once the 3D NR reference frames exist (ae.c).
 * Cells >= hi seed regions grown over cells >= lo (8-connected); a region
 * counts when its bounding box is between min_cells(sensitivity) and 300
 * cells and at least min_cells of its cells are in the watched zones.
 * Stock uses fixed 10000/5000; measured on this camera the still-scene
 * floor is p90 ~500 (day) / ~200 (night IR) with single cells up to ~1700,
 * and a person gives 10-20 cells >= 1000, peaks 3000-11000. So the
 * thresholds follow the noise: hi = max(1800, 3.5 p90), lo = hi / 2. */
#define INNER_GET_3DNR_STAT 0x80044970
#define HW_ROWS 24
static const uint8_t k_min_cells[101] = {
    50,49,48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,32,31,30,29,28,27,27,26,25,24,24,23,22,21,21,20,19,18,18,17,16,15,15,14,
    14,13,13,12,12,12,12,12,12,12,12,11,11,11,11,11,11,11,11,10,10,10,10,9,9,9,9,9,9,9,9,8,8,8,8,8,8,8,8,7,7,7,7,6,6,6,6,6,6,6,6,
    5,5,5,5,4,4,4,4 };          /* stock table @0x410480 */

/* returns 1 if a qualifying region exists; marks cells of accepted regions in act[24*32] */
static int hw_detect(const uint16_t *g, const uint8_t *watch24, int sens, uint8_t *act, int hi, int lo)
{
    static uint8_t seen[HW_ROWS * 32];
    static uint16_t stack[HW_ROWS * 32];
    memset(seen, 0, sizeof seen);
    memset(act, 0, HW_ROWS * 32);
    int minc = k_min_cells[sens < 0 ? 0 : sens > 100 ? 100 : sens], found = 0;
    for (int i = 0; i < HW_ROWS * 32; i++) {
        if (seen[i] || g[i] < hi) continue;
        int sp = 0, r0 = 99, r1 = -1, c0 = 99, c1 = -1, watched = 0, n = 0;
        static uint16_t cells[HW_ROWS * 32];
        stack[sp++] = (uint16_t)i; seen[i] = 1;
        while (sp) {
            int k = stack[--sp], r = k / 32, c = k % 32;
            cells[n++] = (uint16_t)k;
            if (r < r0) r0 = r; if (r > r1) r1 = r; if (c < c0) c0 = c; if (c > c1) c1 = c;
            watched += watch24[k];
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    int rr = r + dr, cc = c + dc;
                    if (rr < 0 || rr >= HW_ROWS || cc < 0 || cc >= 32) continue;
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
static char s_zones[NB / 4 + 1];                    /* hex; "" = whole picture */
static uint8_t g_zone[NB];                          /* expanded mask */
static volatile int g_score, g_motion;
static volatile int g_motion_any;                   /* same detector, whole picture (no events) */
int md_motion_any(void) { return s_enabled && g_motion_any; }
static uint8_t g_active[NB];

static void zones_expand(void)
{
    for (int i = 0; i < NB; i++) {
        if (!s_zones[0]) { g_zone[i] = 1; continue; }
        char c = s_zones[i / 4];
        int v = isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
        g_zone[i] = (v >> (3 - i % 4)) & 1;
    }
}
static int zones_ok(const char *v)
{
    if (!*v) return 1;
    if (strlen(v) != NB / 4) return 0;
    for (; *v; v++) if (!isxdigit((unsigned char)*v)) return 0;
    return 1;
}
CTRL_BOOL(p_en, s_enabled)
int md_enabled(void) { return s_enabled; }
CTRL_BOOL(p_igosd, s_ignore_osd)
static int src_get(char *o, size_t c) { snprintf(o, c, "%s", s_hw ? "hardware" : "software"); return 0; }
static int src_set(const char *v, const char **e)
{
    if (!strcmp(v, "software")) s_hw = 0; else if (!strcmp(v, "hardware")) s_hw = 1;
    else { *e = "must be software or hardware"; return -1; }
    if (s_hw && ae_tnr_state() == -1) ae_tnr_retry();   /* chosen again: retry TNR at the next start */
    return 0;
}
CTRL_INT(p_sens, s_sens, 1, 100)
CTRL_INT(p_hold, s_hold, 2, 300)
CTRL_STR(p_zones, s_zones, zones_ok, "must be 144 hex digits (32x18 grid) or empty")
static int p_zones_setx(const char *v, const char **e) { int r = p_zones_set(v, e); if (!r) zones_expand(); return r; }
static int p_en_setx(const char *v, const char **e) { int r = p_en_set(v, e); if (!r && !s_enabled) events_motion(0); return r; }

int md_status_json(char *out, size_t cap)
{
    char act[NB / 4 + 1];
    for (int i = 0; i < NB / 4; i++) {
        int v = g_active[i * 4] << 3 | g_active[i * 4 + 1] << 2 | g_active[i * 4 + 2] << 1 | g_active[i * 4 + 3];
        act[i] = "0123456789abcdef"[v];
    }
    act[NB / 4] = 0;
    /* the hardware statistic only updates while 3D NR runs (ae.c) */
    const char *note = !s_hw ? "" : ae_tnr_state() == 1 ? "" :
        ae_tnr_state() == -1 ? "the image processor statistic is unavailable: 3D noise reduction was turned off after an "
                               "incomplete start. Choose this detector again and restart the camera to retry."
                             : "the image processor statistic is unavailable (3D noise reduction is not running)";
    return snprintf(out, cap, "{\"enabled\":%s,\"motion\":%s,\"motion_any\":%s,\"score\":%d,\"cols\":%d,\"rows\":%d,\"active\":\"%s\",\"zones\":\"%s\",\"source\":\"%s\",\"hwmax\":%d,\"hwp90\":%d,\"hw_note\":\"%s\"}",
                    s_enabled ? "true" : "false", g_motion ? "true" : "false", g_motion_any ? "true" : "false", g_score, MD_COLS, MD_ROWS, act, s_zones,
                    s_hw ? "hardware" : "software", g_hwmax, g_hwp90, note);
}

static void *md_thread(void *arg)
{
    (void)arg;
    static uint8_t frame[640 * 368 * 3 / 2];
    static float bg[NB];
    int have_bg = 0, run = 0, was_night = night_is_night();
    int64_t last_hit = 0, quiet_until = 0, last_any = 0;
    int run_any = 0;
    for (;;) {
        usleep(250000);
        int64_t now = mono_ms();
        int night = night_is_night();
        if (night != was_night) { was_night = night; have_bg = 0; quiet_until = now + (s_hw ? 10000 : 5000); }   /* IR switching: stock pauses 5 s; the 3D-NR statistic settles in ~8 s */
        if (!s_enabled) { have_bg = 0; g_score = 0; g_motion = 0; g_motion_any = 0; continue; }
        if (s_hw) {
            /* 10 Hz like stock: this pass plus 1 more inside */
            static uint8_t stat[0x608];
            static int hist;                           /* night: 2 of the last 3 polls */
            for (int pass = 0; pass < 2; pass++) {
                if (pass) usleep(100000);
                if (isp_tunnel(INNER_GET_3DNR_STAT, stat) != 0) break;
                const uint16_t *g = (const uint16_t *)(stat + 2);
                int mx = 0;
                for (int i = 0; i < HW_ROWS * 32; i++) if (g[i] > mx) mx = g[i];
                g_hwmax = mx;
                /* watched cells: UI zones (32x18) mapped to the 32x24 grid, minus overlay rows */
                uint8_t watch[HW_ROWS * 32], act[HW_ROWS * 32], watch_all[HW_ROWS * 32], act_all[HW_ROWS * 32];
                osd_cfg_t oc; osd_get(&oc);
                int o0 = -1, o1 = -1;
                if (s_ignore_osd && oc.enabled) {           /* main overlay: 78 px band at 16 px from an edge */
                    int y0 = oc.pos[0] == 'b' ? 1080 - 16 - 78 : 16;
                    o0 = y0 / 45; o1 = (y0 + 77) / 45;
                }
                for (int r = 0; r < HW_ROWS; r++)
                    for (int c = 0; c < 32; c++) {
                        watch_all[r * 32 + c] = (uint8_t)!(r >= o0 && r <= o1);
                        watch[r * 32 + c] = (uint8_t)(g_zone[(r * MD_ROWS / HW_ROWS) * MD_COLS + c] && watch_all[r * 32 + c]);
                    }
                /* noise floor: 90th percentile (counting sort on a coarse scale) */
                static uint16_t hcnt[256];
                memset(hcnt, 0, sizeof hcnt);
                for (int i = 0; i < HW_ROWS * 32; i++) hcnt[g[i] >= 65280 ? 255 : g[i] >> 8]++;
                int acc = 0, p50 = -1, p90 = 0;
                for (int b = 0; b < 256; b++) {
                    acc += hcnt[b];
                    if (p50 < 0 && acc >= HW_ROWS * 32 / 2) p50 = b * 256 + 128;
                    if (acc >= HW_ROWS * 32 * 9 / 10) { p90 = b * 256 + 128; break; }
                }
                int hi = p90 * 7 / 2; if (hi < 1800) hi = 1800;
                int lo = hi / 2;
                g_hwp90 = p90;
                /* most of the picture changed at once (lights, IR switch, AE jump): not
                 * motion. The median follows the light level (~100 bright IR, ~300 lit
                 * room, ~1000 dark room), so compare it with a slow baseline; a jump
                 * that lasts over 5 s becomes the new baseline. */
                static int base50 = -1, glob_n;
                if (base50 < 0) base50 = p50;
                int global = p50 > base50 * 3 + 400;
                if (global) { if (++glob_n > 50) { base50 = p50; glob_n = 0; } }
                else { glob_n = 0; base50 += (p50 - base50) / 32; }
                int hit = global ? 0 : hw_detect(g, watch, s_sens, act, hi, lo);
                int64_t t = mono_ms();
                for (int i = 0; i < NB; i++) g_active[i] = 0;
                int score = 0;
                for (int r = 0; r < HW_ROWS; r++) for (int c = 0; c < 32; c++)
                    if (act[r * 32 + c]) { g_active[(r * MD_ROWS / HW_ROWS) * MD_COLS + c] = 1; score++; }
                g_score = score;
                hist = ((hist << 1) | hit) & 7;
                int ok = night ? __builtin_popcount(hist) >= 2 : hit;
                if (t >= quiet_until && ok) { last_hit = t; if (!g_motion) { g_motion = 1; events_motion(1); } }
                if (g_motion && t - last_hit > (int64_t)s_hold * 1000) { g_motion = 0; events_motion(0); }
                /* whole picture, same rules (only wakes the frame rate) */
                static int hist_all;
                int hit_all = global ? 0 : hw_detect(g, watch_all, s_sens, act_all, hi, lo);
                hist_all = ((hist_all << 1) | hit_all) & 7;
                if (t >= quiet_until && (night ? __builtin_popcount(hist_all) >= 2 : hit_all)) { last_any = t; g_motion_any = 1; }
                if (g_motion_any && t - last_any > (int64_t)s_hold * 1000) g_motion_any = 0;
            }
            continue;
        }
        int w, h;
        if (vi_copy_sub_frame(frame, sizeof frame, &w, &h) < 0) continue;
        int bw = w / MD_COLS, bh = h / MD_ROWS;
        /* rows covered by the burned-in overlay (its clock changes every second) */
        int osd_r0 = -1, osd_r1 = -1;
        if (s_ignore_osd) {
            osd_cfg_t oc;
            osd_get(&oc);
            if (oc.enabled) {
                int th = oc.size == 2 ? 34 : 18, y0 = oc.pos[0] == 'b' ? h - 6 - th : 6;
                osd_r0 = y0 / bh; osd_r1 = (y0 + th - 1) / bh;
            }
        }
        float cur[NB];
        for (int r = 0; r < MD_ROWS; r++)
            for (int c = 0; c < MD_COLS; c++) {
                uint32_t sum = 0;
                for (int y = r * bh; y < (r + 1) * bh; y += 2)        /* every other row/column: plenty */
                    for (int x = c * bw; x < (c + 1) * bw; x += 2) sum += frame[y * w + x];
                cur[r * MD_COLS + c] = (float)sum / (float)((bh / 2) * (bw / 2));
            }
        if (!have_bg) { for (int i = 0; i < NB; i++) bg[i] = cur[i]; have_bg = 1; continue; }

        /* sensitivity 1..100 -> per-block luma threshold 40..5, area 6%..0.4% of watched blocks */
        float thr = 40.0f - 0.35f * (float)s_sens;
        int watched = 0, changed = 0, changed_all = 0, watched_any = 0, changed_any = 0;
        for (int i = 0; i < NB; i++) {
            float d = cur[i] - bg[i];
            if (d < 0) d = -d;
            int hit = d > thr;
            changed_all += hit;
            int row = i / MD_COLS, outside_osd = !(row >= osd_r0 && row <= osd_r1), z = g_zone[i] && outside_osd;
            if (outside_osd) { watched_any++; changed_any += hit; }
            if (z) { watched++; changed += hit; }
            g_active[i] = (uint8_t)(hit && z);
            bg[i] += (cur[i] - bg[i]) * (hit ? 0.02f : 0.08f);       /* adapt slowly where things move */
        }
        g_score = changed;
        if (changed_all > NB * 6 / 10) {                              /* light switched: not motion */
            for (int i = 0; i < NB; i++) bg[i] = cur[i];
            quiet_until = now + 2000; run = 0;
            continue;
        }
        int need = watched * (60 - (s_sens * 56) / 100) / 1000;       /* 6% .. 0.4% */
        if (need < 1) need = 1;
        if (now >= quiet_until && watched && changed >= need) {
            if (++run >= 2) { last_hit = now; if (!g_motion) { g_motion = 1; events_motion(1); } }
        } else run = 0;
        if (g_motion && now - last_hit > (int64_t)s_hold * 1000) { g_motion = 0; events_motion(0); }
        /* whole picture, same rules (only wakes the frame rate) */
        int need_any = watched_any * (60 - (s_sens * 56) / 100) / 1000;
        if (need_any < 1) need_any = 1;
        if (now >= quiet_until && changed_any >= need_any) { if (++run_any >= 2) { last_any = now; g_motion_any = 1; } }
        else run_any = 0;
        if (g_motion_any && now - last_any > (int64_t)s_hold * 1000) g_motion_any = 0;
    }
    return NULL;
}

/* raw hardware statistic, for tuning: {"hdr":n,"rows":[[...32],...24],"tail":[..]} */
int md_hw_raw_json(char *out, size_t cap)
{
    static uint8_t stat[0x608];
    if (isp_tunnel(INNER_GET_3DNR_STAT, stat) != 0) return snprintf(out, cap, "{\"error\":\"ioctl failed\"}");
    size_t o = 0;
    const uint16_t *u = (const uint16_t *)stat;
    o += (size_t)snprintf(out + o, cap - o, "{\"hdr\":%u,\"rows\":[", u[0]);
    for (int r = 0; r < HW_ROWS && o < cap; r++) {
        o += (size_t)snprintf(out + o, cap - o, "%s[", r ? "," : "");
        for (int c = 0; c < 32 && o < cap; c++) o += (size_t)snprintf(out + o, cap - o, "%s%u", c ? "," : "", u[1 + r * 32 + c]);
        if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]");
    }
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]");
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "}");
    return (int)o;
}

void md_init(void)
{
    zones_expand();
    ctrl_param_t a = { "md.enabled", p_en_get, p_en_setx, CTRL_PERSIST };       control_register(&a);
    CTRL_REG("md.sensitivity", p_sens, CTRL_PERSIST);
    CTRL_REG("md.hold", p_hold, CTRL_PERSIST);
    CTRL_REG("md.ignore_osd", p_igosd, CTRL_PERSIST);
    ctrl_param_t sr = { "md.source", src_get, src_set, CTRL_PERSIST };          control_register(&sr);
    ctrl_param_t z = { "md.zones", p_zones_get, p_zones_setx, CTRL_PERSIST };   control_register(&z);
}

void md_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, md_thread, NULL);
    pthread_attr_destroy(&a);
}
