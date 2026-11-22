#include "md.h"
#include "md_logic.h"
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
 * The decision (noise-following thresholds, region growing, light changes)
 * is in md_logic.c. */
#define INNER_GET_3DNR_STAT 0x80044970
#define HW_ROWS 24
static char s_zones[NB / 4 + 1];                    /* hex; "" = whole picture */
static uint8_t g_zone[NB];                          /* expanded mask */
static volatile int g_score, g_motion;
static volatile int g_motion_any;                   /* same detector, whole picture (no events) */
int md_motion_any(void) { return s_enabled && g_motion_any; }
static uint8_t g_active[NB];

/* options of md_logic.h (all off by default) */
static int s_min_size, s_persist, s_light, s_loiter, s_loiter_zone, s_line_dir;
static char s_zone_sens[8 * 17], s_line[24];
static mdl_zone_t s_zone[MDL_MAX_ZONES];
static int s_nzones, s_line_xy[4], s_line_on;
static volatile int g_line, g_loiter, g_light, g_ntrk;
static mdl_track_out_t g_trk[MDL_MAX_TRACKS];

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
static int p_en_setx(const char *v, const char **e)
{
    int r = p_en_set(v, e);
    if (!r && !s_enabled) { events_motion(0); events_line(0, 0, 0); events_loiter(0, 0); }
    return r;
}

/* ---- options (docs/motion.md) ---- */
CTRL_INT(p_minsz, s_min_size, 0, 50)
CTRL_INT(p_persist, s_persist, 0, 10000)
CTRL_INT(p_loiter, s_loiter, 0, 600)
CTRL_INT(p_lzone, s_loiter_zone, 0, 8)
static int zsens_ok(const char *v) { return mdl_parse_zones(v, NULL, MDL_MAX_ZONES) >= 0; }
CTRL_STR(p_zsens, s_zone_sens, zsens_ok, "must be up to 8 zones c0,r0,c1,r1,sensitivity separated by ; (columns 0-31, rows 0-17, sensitivity 1-100) or empty")
static int p_zsens_setx(const char *v, const char **e)
{
    int r = p_zsens_set(v, e);
    if (!r) { mdl_zone_t z[MDL_MAX_ZONES]; int n = mdl_parse_zones(s_zone_sens, z, MDL_MAX_ZONES); s_nzones = 0; if (n > 0) { memcpy(s_zone, z, sizeof z); s_nzones = n; } }
    return r;
}
static int line_ok(const char *v) { return mdl_parse_line(v, NULL) >= 0; }
CTRL_STR(p_line, s_line, line_ok, "must be x0,y0,x1,y1 (0-1000, end points at least 20 apart) or empty")
static int p_line_setx(const char *v, const char **e)
{
    int r = p_line_set(v, e);
    if (!r) { int l[4]; s_line_on = 0; if (mdl_parse_line(s_line, l) == 1) { memcpy(s_line_xy, l, sizeof l); s_line_on = 1; } }
    return r;
}
static const char *const k_dirs[] = { "both", "left", "right" };
static int p_ldir_get(char *o, size_t c) { snprintf(o, c, "%s", k_dirs[s_line_dir]); return 0; }
static int p_ldir_set(const char *v, const char **e)
{
    for (int i = 0; i < 3; i++) if (!strcmp(v, k_dirs[i])) { s_line_dir = i; return 0; }
    *e = "must be both, left or right";
    return -1;
}
static int p_light_get(char *o, size_t c) { snprintf(o, c, "%d", s_light); return 0; }
static int p_light_set(const char *v, const char **e)
{
    char *end;
    long x = strtol(v, &end, 10);
    if (!*v || *end || (x != 0 && (x < 10 || x > 90))) { *e = "must be 0 (off) or a percentage from 10 to 90"; return -1; }
    s_light = (int)x;
    return 0;
}

static void cfg_build(mdl_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->sens = s_sens; c->hold_s = s_hold;
    c->nzones = s_nzones; memcpy(c->zone, s_zone, sizeof c->zone);
    c->min_size = s_min_size; c->persist_ms = s_persist; c->light_pct = s_light;
    c->line_on = s_line_on; memcpy(c->line, s_line_xy, sizeof c->line); c->line_dir = s_line_dir;
    c->loiter_s = s_loiter; c->loiter_zone = s_loiter_zone;
}

/* act on a decision: events on changes only */
static void md_act(const mdl_out_t *o)
{
    g_motion_any = o->motion_any;
    if (o->motion != g_motion) { g_motion = o->motion; events_motion(g_motion); }
    for (int i = 0; i < o->ncross; i++) events_line(1, o->cross[i].dir, o->cross[i].id);
    if (o->line != g_line) { g_line = o->line; if (!g_line) events_line(0, 0, 0); }
    if (o->loiter != g_loiter) { g_loiter = o->loiter; events_loiter(g_loiter, s_loiter_zone); }
    g_light = o->light;
    for (int i = 0; i < o->ntracks; i++) g_trk[i] = o->track[i];
    g_ntrk = o->ntracks;
}

int md_status_json(char *out, size_t cap)
{
    char act[NB / 4 + 1], trk[MDL_MAX_TRACKS * 20 + 1] = "";
    for (int i = 0; i < NB / 4; i++) {
        int v = g_active[i * 4] << 3 | g_active[i * 4 + 1] << 2 | g_active[i * 4 + 2] << 1 | g_active[i * 4 + 3];
        act[i] = "0123456789abcdef"[v];
    }
    act[NB / 4] = 0;
    int nt = g_ntrk;
    for (int i = 0, o = 0; i < nt && i < MDL_MAX_TRACKS && o < (int)sizeof trk; i++)
        o += snprintf(trk + o, sizeof trk - (size_t)o, "%s[%d,%d,%d]", i ? "," : "", g_trk[i].id, g_trk[i].x, g_trk[i].y);
    /* the hardware statistic only updates while 3D NR runs (ae.c) */
    const char *note = !s_hw ? "" : ae_tnr_state() == 1 ? "" :
        ae_tnr_state() == -1 ? "the image processor statistic is unavailable: 3D noise reduction was turned off after an "
                               "incomplete start. Choose this detector again and restart the camera to retry."
                             : "the image processor statistic is unavailable (3D noise reduction is not running)";
    return snprintf(out, cap, "{\"enabled\":%s,\"motion\":%s,\"motion_any\":%s,\"score\":%d,\"cols\":%d,\"rows\":%d,\"active\":\"%s\",\"zones\":\"%s\",\"source\":\"%s\",\"hwmax\":%d,\"hwp90\":%d,\"hw_note\":\"%s\","
                    "\"line\":%s,\"loiter\":%s,\"light\":%s,\"tracks\":[%s]}",
                    s_enabled ? "true" : "false", g_motion ? "true" : "false", g_motion_any ? "true" : "false", g_score, MD_COLS, MD_ROWS, act, s_zones,
                    s_hw ? "hardware" : "software", g_hwmax, g_hwp90, note,
                    g_line ? "true" : "false", g_loiter ? "true" : "false", g_light ? "true" : "false", trk);
}

static void *md_thread(void *arg)
{
    (void)arg;
    static uint8_t frame[640 * 368 * 3 / 2];
    static mdl_t st;
    static mdl_out_t res;
    mdl_init(&st);
    int was_night = night_is_night();
    for (;;) {
        usleep(250000);
        int64_t now = mono_ms();
        int night = night_is_night();
        if (night != was_night) { was_night = night; mdl_night_switch(&st, now, s_hw); }
        if (!s_enabled) { mdl_disabled(&st); g_score = 0; g_motion = 0; g_motion_any = 0; g_line = 0; g_loiter = 0; g_ntrk = 0; continue; }
        mdl_cfg_t cfg;
        if (s_hw) {
            /* 10 Hz like stock: this pass plus 1 more inside */
            static uint8_t stat[0x608];
            for (int pass = 0; pass < 2; pass++) {
                if (pass) usleep(100000);
                if (isp_tunnel(INNER_GET_3DNR_STAT, stat) != 0) break;
                const uint16_t *g = (const uint16_t *)(stat + 2);
                /* watched cells: UI zones (32x18) mapped to the 32x24 grid, minus overlay rows */
                uint8_t watch[HW_ROWS * 32], watch_all[HW_ROWS * 32];
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
                cfg_build(&cfg);
                mdl_in_t in = { mono_ms(), night, watch, watch_all };
                mdl_hw_frame(&st, &cfg, g, &in, &res);
                g_hwmax = res.hwmax;
                g_hwp90 = res.hwp90;
                for (int i = 0; i < NB; i++) g_active[i] = 0;
                for (int r = 0; r < HW_ROWS; r++) for (int c = 0; c < 32; c++)
                    if (res.active[r * 32 + c]) g_active[(r * MD_ROWS / HW_ROWS) * MD_COLS + c] = 1;
                g_score = res.score;
                md_act(&res);
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
        uint8_t watch[NB], watch_all[NB];
        for (int r = 0; r < MD_ROWS; r++)
            for (int c = 0; c < MD_COLS; c++) {
                uint32_t sum = 0;
                for (int y = r * bh; y < (r + 1) * bh; y += 2)        /* every other row/column: plenty */
                    for (int x = c * bw; x < (c + 1) * bw; x += 2) sum += frame[y * w + x];
                int i = r * MD_COLS + c;
                cur[i] = (float)sum / (float)((bh / 2) * (bw / 2));
                watch_all[i] = (uint8_t)!(r >= osd_r0 && r <= osd_r1);
                watch[i] = (uint8_t)(g_zone[i] && watch_all[i]);
            }
        cfg_build(&cfg);
        mdl_in_t in = { now, night, watch, watch_all };
        if (mdl_sw_frame(&st, &cfg, cur, &in, &res)) continue;       /* first frame: background only */
        for (int i = 0; i < NB; i++) g_active[i] = res.active[i];
        g_score = res.score;
        md_act(&res);
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
    /* options (docs/motion.md), all off by default */
    ctrl_param_t zs = { "md.zone_sens", p_zsens_get, p_zsens_setx, CTRL_PERSIST }; control_register(&zs);
    CTRL_REG("md.min_size", p_minsz, CTRL_PERSIST);
    CTRL_REG("md.persist", p_persist, CTRL_PERSIST);
    CTRL_REG("md.light", p_light, CTRL_PERSIST);
    ctrl_param_t ln = { "md.line", p_line_get, p_line_setx, CTRL_PERSIST };     control_register(&ln);
    CTRL_REG("md.line_dir", p_ldir, CTRL_PERSIST);
    CTRL_REG("md.loiter", p_loiter, CTRL_PERSIST);
    CTRL_REG("md.loiter_zone", p_lzone, CTRL_PERSIST);
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
