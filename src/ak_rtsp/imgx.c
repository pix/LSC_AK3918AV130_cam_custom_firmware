#include "imgx.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isp.h"
#include "ae.h"
#include "ak_ioctls.h"
#include "ctrl_params.h"
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#define GUARD "/etc/config/ak_rtsp_imgx_guard"
#define INNER_GET_WDR      0x8004491c
#define INNER_SET_WDR      0x4004491b
#define WDR_SIZE           0x4228          /* u32 mode + 17 sets of 0x3e4 */
#define WDR_SET            0x3e4
#define INNER_MASK_AREA_M  0x4004495a
#define INNER_MASK_AREA_S  0x40044965
#define INNER_MASK_COLOR_M 0x80044992      /* "read-style" code is the SET here */
#define INNER_MASK_COLOR_S 0x80044994
#define INNER_USER_PARAMS  0x4004496d
#define OSD_PALETTE_CMD    0x4005490a

static int s_wdr, s_flicker = 50, g_ready, g_skip;
static char s_areas[100], s_color[16] = "#000000";

/* ---- WDR: blend the calibrated tone curves with a straight line (stock's
 *      FUN_0012b0d0): k = (v+50)*1024/50; new = (k*B + (1024-k)*D) >> 10 ---- */
static uint8_t *g_wdr_base;
static int apply_wdr(void)
{
    static uint8_t buf[WDR_SIZE];
    if (!g_wdr_base) {
        g_wdr_base = malloc(WDR_SIZE);
        if (!g_wdr_base || isp_tunnel(INNER_GET_WDR, g_wdr_base) != 0) { free(g_wdr_base); g_wdr_base = NULL; return -1; }
    }
    memcpy(buf, g_wdr_base, WDR_SIZE);
    int k = (s_wdr + 50) * 1024 / 50;
    for (int set = 0; set < 17; set++) {
        uint16_t *b = (uint16_t *)(g_wdr_base + 4 + set * WDR_SET) + 8;    /* area_tb1..6, 65 points each */
        uint16_t *o = (uint16_t *)(buf + 4 + set * WDR_SET) + 8;
        for (int t = 0; t < 6; t++)
            for (int j = 0; j < 65; j++) {
                int d = j < 64 ? j * 16 : 1023;
                int v = (k * b[t * 65 + j] + (1024 - k) * d) >> 10;
                o[t * 65 + j] = (uint16_t)(v < 0 ? 0 : v > 1023 ? 1023 : v);
            }
    }
    return isp_tunnel(INNER_SET_WDR, buf);
}

/* ---- privacy masks ------------------------------------------------------------ */
typedef struct { uint16_t sx, ex, sy, ey; uint8_t en, pad; } __attribute__((packed)) mask_area_t;
static int parse_areas(const char *v, mask_area_t out[4])
{
    memset(out, 0, sizeof(mask_area_t) * 4);
    int n = 0;
    const char *p = v;
    while (*p) {
        int x0, y0, x1, y1, used = 0;
        if (n == 4 || sscanf(p, "%d,%d,%d,%d%n", &x0, &y0, &x1, &y1, &used) != 4) return -1;
        if (x0 < 0 || y0 < 0 || x1 >= 1920 || y1 >= 1080 || x1 <= x0 || y1 <= y0) return -1;
        out[n].sx = (uint16_t)x0; out[n].ex = (uint16_t)x1; out[n].sy = (uint16_t)y0; out[n].ey = (uint16_t)y1; out[n].en = 1;
        n++;
        p += used;
        if (*p == ';') p++; else if (*p) return -1;
    }
    return n;
}
static int areas_ok(const char *v) { mask_area_t a[4]; return parse_areas(v, a) >= 0; }
static int color_ok(const char *v)
{
    if (!strcmp(v, "mosaic")) return 1;
    if (strlen(v) != 7 || v[0] != '#') return 0;
    for (int i = 1; i < 7; i++) if (!isxdigit((unsigned char)v[i])) return 0;
    return 1;
}

static volatile int g_privacy;
static int apply_mask(void)
{
    mask_area_t a[4];
    int n = parse_areas(s_areas, a);
    if (n < 0) n = 0;
    int mosaic = !strcmp(s_color, "mosaic");
    const char *color = s_color;
    if (g_privacy) {                    /* privacy mode: whole picture black, whatever the user masks are */
        memset(a, 0, sizeof a);
        a[0].sx = 0; a[0].ex = 1919; a[0].sy = 0; a[0].ey = 1079; a[0].en = 1;
        n = 1; mosaic = 0; color = "#000000";
    }
    int ret = 0;
    /* colour: palette entry 19 (selector 2: only that entry, OSD palette untouched) */
    if (n && !mosaic) {
        unsigned r, g, b;
        sscanf(color + 1, "%02x%02x%02x", &r, &g, &b);
        int Y = (int)(306 * r + 601 * g + 117 * b) >> 10;
        int U = (((int)(512 * b) - 173 * (int)r - 339 * (int)g) >> 10) + 128;
        int V = (((int)(512 * r) - 429 * (int)g - 83 * (int)b) >> 10) + 128;
        if (U < 0) U = 0; if (U > 255) U = 255; if (V < 0) V = 0; if (V > 255) V = 255; if (Y > 255) Y = 255;
        uint32_t blk[0x84 / 4];
        memset(blk, 0, sizeof blk);
        blk[0] = OSD_PALETTE_CMD;
        blk[0x50 / 4] = 0xffu << 24 | (uint32_t)Y << 16 | (uint32_t)U << 8 | (uint32_t)V;
        blk[0x54 / 4] = 2;
        ret |= isp_tunnel(INNER_USER_PARAMS, blk);
    }
    uint8_t col[0x68];
    memset(col, 0, sizeof col);
    for (int i = 0; i < 4; i++) {
        col[i * 6] = mosaic ? 1 : 2;
        uint16_t idx = mosaic ? 19 : 19;
        memcpy(col + i * 6 + 2, &idx, 2);
    }
    uint16_t mh = mosaic ? 7 : 0;
    memcpy(col + 0x60, &mh, 2); memcpy(col + 0x62, &mh, 2);
    uint32_t ff = 0xffffffff;
    memcpy(col + 0x64, &ff, 4);
    ret |= isp_tunnel(INNER_MASK_COLOR_M, col);
    ret |= isp_tunnel(INNER_MASK_COLOR_S, col);
    uint8_t am[0xa0], as[0xa0];
    memset(am, 0, sizeof am); memset(as, 0, sizeof as);
    memcpy(am, a, sizeof(mask_area_t) * 4);
    mask_area_t sub[4];
    memcpy(sub, a, sizeof sub);
    for (int i = 0; i < 4; i++) {
        if (!sub[i].en) continue;
        sub[i].sx = (uint16_t)(sub[i].sx * 640 / 1920); sub[i].ex = (uint16_t)(sub[i].ex * 640 / 1920);
        sub[i].sy = (uint16_t)(sub[i].sy * 360 / 1080); sub[i].ey = (uint16_t)(sub[i].ey * 360 / 1080);
    }
    memcpy(as, sub, sizeof sub);
    ret |= isp_tunnel(INNER_MASK_AREA_M, am);
    ret |= isp_tunnel(INNER_MASK_AREA_S, as);
    return ret;
}

/* WDR/mask writes come from the web/settings thread, the day/night thread
 * (after an ISP reload) and the schedule thread: one at a time (they share
 * g_wdr_base and the static WDR buffer). */
static pthread_mutex_t g_ix_lock = PTHREAD_MUTEX_INITIALIZER;
static int wdr_apply_l(void) { pthread_mutex_lock(&g_ix_lock); int r = apply_wdr(); pthread_mutex_unlock(&g_ix_lock); return r; }
static int mask_apply_l(void) { pthread_mutex_lock(&g_ix_lock); int r = apply_mask(); pthread_mutex_unlock(&g_ix_lock); return r; }
static void wdr_reset_l(void) { pthread_mutex_lock(&g_ix_lock); free(g_wdr_base); g_wdr_base = NULL; pthread_mutex_unlock(&g_ix_lock); }

void imgx_set_privacy(int on)
{
    g_privacy = on;
    if (g_ready) mask_apply_l();          /* also when the boot guard skipped the user's masks */
}

int imgx_flicker_step(void) { return s_flicker == 60 ? 285 : s_flicker == 0 ? 1 : 342; }

/* ---- settings ----------------------------------------------------------------- */
static int wdr_get(char *o, size_t c) { snprintf(o, c, "%d", s_wdr); return 0; }
static int wdr_set(const char *v, const char **e)
{
    char *end; long x = strtol(v, &end, 10);
    if (!*v || *end || x < -50 || x > 50) { *e = "must be -50..50"; return -1; }
    s_wdr = (int)x;
    if (g_ready && !g_skip && wdr_apply_l() != 0) { *e = "the ISP refused the WDR change"; return -1; }
    return 0;
}
static int flk_get(char *o, size_t c) { if (s_flicker) snprintf(o, c, "%d", s_flicker); else snprintf(o, c, "off"); return 0; }
static int flk_set(const char *v, const char **e)
{
    if (!strcmp(v, "50")) s_flicker = 50; else if (!strcmp(v, "60")) s_flicker = 60; else if (!strcmp(v, "off")) s_flicker = 0;
    else { *e = "must be 50, 60 or off"; return -1; }
    if (g_ready && !g_skip) { ae_tuning_t t; ae_get_tuning(&t); ae_set_tuning(&t); }   /* writes the step */
    return 0;
}
CTRL_STR(p_areas, s_areas, areas_ok, "up to 4 rectangles x0,y0,x1,y1 (1920x1080) separated by ';'")
CTRL_STR(p_color, s_color, color_ok, "#rrggbb or mosaic")
static int areas_setx(const char *v, const char **e) { int r = p_areas_set(v, e); if (!r && g_ready && !g_skip) mask_apply_l(); return r; }
static int color_setx(const char *v, const char **e) { int r = p_color_set(v, e); if (!r && g_ready && !g_skip) mask_apply_l(); return r; }

/* image.nr: noise reduction strength (isp_set_nr) */
static int s_nr;
static int nr_get(char *o, size_t c) { snprintf(o, c, "%d", s_nr); return 0; }
static int nr_set(const char *v, const char **e)
{
    char *end; long x = strtol(v, &end, 10);
    if (!*v || *end || x < -50 || x > 50) { *e = "must be -50..50"; return -1; }
    if (g_ready && !g_skip) {
        pthread_mutex_lock(&g_ix_lock);
        int r = isp_set_nr((int)x);
        pthread_mutex_unlock(&g_ix_lock);
        if (r != 0) { *e = "the ISP refused the noise reduction change"; return -1; }
    }
    s_nr = (int)x;
    return 0;
}
static void nr_apply_l(void) { pthread_mutex_lock(&g_ix_lock); isp_set_nr(s_nr); pthread_mutex_unlock(&g_ix_lock); }

void imgx_apply_all(void)
{
    if (!g_ready || g_skip) return;
    wdr_reset_l();            /* the reload brought new calibrated curves */
    if (s_wdr) wdr_apply_l();
    if (s_nr) nr_apply_l();   /* baselines were dropped by isp_reset_picture_baselines() */
    if (s_areas[0] || g_privacy) mask_apply_l();
}

void imgx_ready(void)
{
    if (access(GUARD, F_OK) == 0) {
        fprintf(stderr, "[imgx] WDR/mask settings skipped: previous start did not complete (%s); saving any of them retries\n", GUARD);
        unlink(GUARD);
        g_skip = 1;
        g_ready = 1;
        return;
    }
    int need = s_wdr || s_areas[0] || s_nr;
    if (need) { int fd = open(GUARD, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) { fsync(fd); close(fd); sync(); } }
    g_ready = 1;
    if (s_wdr) wdr_apply_l();
    if (s_nr) nr_apply_l();
    if (s_areas[0] || g_privacy) mask_apply_l();
    if (need) { unlink(GUARD); sync(); }
}

static int any_setk(void) { g_skip = 0; return 0; }   /* a deliberate change clears a guard skip */
static int wdr_setk(const char *v, const char **e) { any_setk(); return wdr_set(v, e); }
static int nr_setk(const char *v, const char **e) { any_setk(); return nr_set(v, e); }
static int areas_setk(const char *v, const char **e) { any_setk(); return areas_setx(v, e); }
static int color_setk(const char *v, const char **e) { any_setk(); return color_setx(v, e); }

void imgx_init(void)
{
    ctrl_param_t a = { "image.wdr", wdr_get, wdr_setk, CTRL_PERSIST };          control_register(&a);
    ctrl_param_t b = { "image.flicker", flk_get, flk_set, CTRL_PERSIST };       control_register(&b);
    ctrl_param_t n = { "image.nr", nr_get, nr_setk, CTRL_PERSIST };             control_register(&n);
    ctrl_param_t c = { "mask.areas", p_areas_get, areas_setk, CTRL_PERSIST };   control_register(&c);
    ctrl_param_t d = { "mask.color", p_color_get, color_setk, CTRL_PERSIST };   control_register(&d);
}
