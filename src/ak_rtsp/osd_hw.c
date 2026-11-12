#include "osd_hw.h"
#include "osd.h"
#include "osd_font.h"
#include "night.h"
#include "system.h"
#include "ak_rtsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>

#ifndef OSD_GUARD_FILE
#define OSD_GUARD_FILE "/etc/config/ak_rtsp_osd_guard"
#endif
#define OSD_KILL_FILE  "/tmp/sd/maint/no_osd"

/* ---- driver interface (see osd_hw.h for provenance) ------------------------ */
#define ISP_TUNNEL       0xc0cc5616
#define ISP_INNER_OSD    0x4004496d     /* Ak_ISP_Set_OSD_Params */
#define OSD_CMD_PALETTE  0x4005490a     /* ak_vpss_osd_set_param type 1 */
#define OSD_CMD_REGISTER 0x40045003     /* type 2: region geometry + 3 buffers */
#define OSD_CMD_GET_BUF  0x80045005     /* type 4: next free buffer */
#define OSD_CMD_SET_BUF  0x40045004     /* type 5: show this buffer */

struct isp_tunnel { int flag; int inner_cmd; void *payload; char pad[192]; } __attribute__((packed));

struct osd_blk {                 /* 0x84 bytes (FUN_00122d88 / ak_osd_create_canvas) */
    uint32_t cmd;                /* 0x00 sub-command */
    uint32_t chn;                /* 0x04 0 = main, 1 = sub */
    uint32_t region;             /* 0x08 0..2 */
    uint32_t one;                /* 0x0c always 1 */
    uint32_t size;               /* 0x10 register: 3 buffers; set: one buffer */
    uint16_t x, y, w, h;         /* 0x14 */
    uint32_t fmt;                /* 0x1c 0 = region off, 1/2 = on */
    uint16_t bpp_code, pad;      /* 0x20 2 = 4 bits per pixel */
    uint32_t phys, virt;         /* 0x24, 0x28 */
    uint8_t  rest[0x84 - 0x2c];
};
_Static_assert(sizeof(struct osd_blk) == 0x84, "osd_blk layout");

struct ion_alloc { uint32_t len, align, heap_mask, flags, handle, phys, kaddr; };
struct ion_share { uint32_t handle; int fd; };
#define ION_IOC_ALLOC 0xc01c4900
#define ION_IOC_SHARE 0xc0084904

/* palette (A Y U V): 0 transparent, 1 white, 2 black, 3 grey (anti-alias edge) */
static const uint32_t k_palette[16] = { 0x00000000, 0xffff8080, 0xff108080, 0xff808080 };
enum { PX_CLEAR = 0, PX_FILL = 1, PX_OUTLINE = 2, PX_EDGE = 3 };

/* ---- per-stream canvas --------------------------------------------------- */
typedef struct {
    const char *name;
    int chn, frame_w, frame_h, margin;
    const osd_font_t *font[2];   /* 1x, 2x */
    int outline[2];
    int cw, ch;                  /* canvas size for the 2x font (cw multiple of 16, ch even) */
    uint32_t one, total;         /* bytes per buffer, all three buffers */
    uint8_t *mem; uint32_t phys; /* ION block */
    uint8_t *cov;                /* cw*ch scratch: per-pixel palette index */
    int ok, shown;
    char pos[4];                 /* position the region is registered at */
} canvas_t;

static canvas_t g_cv[2] = {
    { "main", 0, 1920, 1080, 16, { &osd_font_main1, &osd_font_main2 }, { 2, 3 } },
    { "sub",  1,  640,  360,  6, { &osd_font_sub1,  &osd_font_sub2 },  { 1, 2 } },
};
static int g_ion = -1, g_swap_nibbles, g_fmt = 2;
static volatile const char *g_state = "off";
static volatile int g_run;
static pthread_t g_thr;

const char *osd_hw_state(void) { return (const char *)g_state; }
void osd_hw_clear_guard(void) { if (unlink(OSD_GUARD_FILE) == 0) sync(); }

static int osd_ioctl(struct osd_blk *b, uint32_t cmd)
{
    struct isp_tunnel t;
    memset(&t, 0, sizeof t);
    t.flag = 1; t.inner_cmd = ISP_INNER_OSD; t.payload = b;
    b->cmd = cmd;
    return ioctl(g_isp_fd, ISP_TUNNEL, &t);
}

static int ion_alloc(uint32_t len, uint8_t **mem, uint32_t *phys)
{
    if (g_ion < 0) g_ion = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (g_ion < 0) return -1;
    struct ion_alloc a = { len, 0x20, 0xff, 4, 0, 0, 0 };     /* as osal_dma_mem_alloc */
    if (ioctl(g_ion, ION_IOC_ALLOC, &a) < 0 || !a.phys) return -1;
    struct ion_share s = { a.handle, -1 };
    if (ioctl(g_ion, ION_IOC_SHARE, &s) < 0) return -1;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, 0);
    if (p == MAP_FAILED) return -1;
    memset(p, 0, len);
    *mem = p; *phys = a.phys;
    return 0;
}

static void region_xy(const canvas_t *c, const char *pos, int *x, int *y)
{
    *x = pos[1] == 'r' ? c->frame_w - c->margin - c->cw : c->margin;
    *y = pos[0] == 'b' ? c->frame_h - c->margin - c->ch : c->margin;
    *x &= ~1; *y &= ~1;
}

static int canvas_register(canvas_t *c, const char *pos, int on)
{
    struct osd_blk b;
    memset(&b, 0, sizeof b);
    int x, y;
    region_xy(c, pos, &x, &y);
    b.chn = c->chn; b.region = 0; b.one = 1; b.size = c->total;
    b.x = x; b.y = y; b.w = c->cw; b.h = c->ch;
    b.fmt = on ? g_fmt : 0; b.bpp_code = 2;
    b.phys = c->phys; b.virt = (uint32_t)(uintptr_t)c->mem;
    if (osd_ioctl(&b, OSD_CMD_REGISTER) != 0) {
        fprintf(stderr, "[osd] %s: register failed: %s\n", c->name, strerror(errno));
        return -1;
    }
    snprintf(c->pos, sizeof c->pos, "%s", pos);
    return 0;
}

static int canvas_init(canvas_t *c)
{
    int maxw = c->chn == 0 ? 1536 : 852;              /* driver limits, 3 regions @4bpp */
    c->cw = (c->frame_w - 2 * c->margin) & ~15;
    if (c->cw > maxw) c->cw = maxw;
    c->ch = (c->font[1]->h + 2 * c->outline[1] + 1) & ~1;
    c->one = (uint32_t)c->cw * c->ch / 2;
    c->total = c->one * 3;
    c->cov = malloc((size_t)c->cw * c->ch);
    if (!c->cov || ion_alloc(c->total, &c->mem, &c->phys) != 0) {
        fprintf(stderr, "[osd] %s: memory allocation failed\n", c->name);
        return -1;
    }
    if (canvas_register(c, "tl", 0) != 0) return -1;
    printf("[osd] %s: canvas %dx%d, ION phys %08x\n", c->name, c->cw, c->ch, c->phys);
    c->ok = 1;
    return 0;
}

/* Render text into c->cov (palette indices): solid/edge glyph pixels, then a
 * black outline around them (separable dilation). The canvas is sized for the
 * 2x font; text hugs the corner it is positioned in (right/bottom aligned). */
static void canvas_render(canvas_t *c, const char *text, int right, int bottom, int big)
{
    const osd_font_t *f = c->font[big ? 1 : 0];
    int n = (int)strlen(text), o = c->outline[big ? 1 : 0];
    int maxn = (c->cw - 2 * o) / f->w;
    if (n > maxn) n = maxn;
    int tw = n * f->w + 2 * o, ox = right ? c->cw - tw : 0;
    int oy = bottom ? c->ch - (f->h + 2 * o) : 0;
    memset(c->cov, PX_CLEAR, (size_t)c->cw * c->ch);
    for (int i = 0; i < n; i++)
        for (int y = 0; y < f->h; y++) {
            uint8_t *row = c->cov + (size_t)(oy + y + o) * c->cw + ox + o + i * f->w;
            for (int x = 0; x < f->w; x++) {
                int v = osd_font_px(f, (unsigned char)text[i], x, y);
                if (v) row[x] = v == 2 ? PX_FILL : PX_EDGE;
            }
        }
    if (!o) return;
    /* outline: any clear pixel within o (Chebyshev) of a glyph pixel */
    static uint8_t tmp[1536 * 96];
    if ((size_t)c->cw * c->ch > sizeof tmp) return;
    for (int y = 0; y < c->ch; y++) {
        const uint8_t *r = c->cov + (size_t)y * c->cw;
        uint8_t *t = tmp + (size_t)y * c->cw;
        for (int x = 0; x < c->cw; x++) {
            int hit = 0;
            for (int k = x - o; k <= x + o && !hit; k++)
                if (k >= 0 && k < c->cw && r[k]) hit = 1;
            t[x] = (uint8_t)hit;
        }
    }
    for (int y = 0; y < c->ch; y++)
        for (int x = 0; x < c->cw; x++) {
            uint8_t *p = c->cov + (size_t)y * c->cw + x;
            if (*p) continue;
            for (int k = y - o; k <= y + o; k++)
                if (k >= 0 && k < c->ch && tmp[(size_t)k * c->cw + x]) { *p = PX_OUTLINE; break; }
        }
}

static int canvas_show(canvas_t *c, const char *text, const char *pos, int big)
{
    if (strcmp(c->pos, pos) || !c->shown) {
        if (canvas_register(c, pos, 1) != 0) return -1;
    }
    struct osd_blk g;
    memset(&g, 0, sizeof g);
    g.chn = c->chn; g.region = 0;
    if (osd_ioctl(&g, OSD_CMD_GET_BUF) != 0) {
        fprintf(stderr, "[osd] %s: get buffer failed: %s\n", c->name, strerror(errno));
        return -1;
    }
    if (g.phys < c->phys || g.phys + c->one > c->phys + c->total) {
        fprintf(stderr, "[osd] %s: driver returned buffer %08x outside our block\n", c->name, g.phys);
        return -1;
    }
    uint8_t *buf = c->mem + (g.phys - c->phys);
    canvas_render(c, text, pos[1] == 'r', pos[0] == 'b', big);
    for (size_t i = 0, n = (size_t)c->cw * c->ch; i < n; i += 2) {
        uint8_t a = c->cov[i], b = c->cov[i + 1];
        buf[i / 2] = g_swap_nibbles ? (uint8_t)(a << 4 | b) : (uint8_t)(b << 4 | a);
    }
    __builtin___clear_cache((char *)buf, (char *)buf + c->one);

    int x, y;
    region_xy(c, pos, &x, &y);
    struct osd_blk s;
    memset(&s, 0, sizeof s);
    s.chn = c->chn; s.region = 0; s.one = 1; s.size = c->one;
    s.x = x; s.y = y; s.w = c->cw; s.h = c->ch; s.fmt = g_fmt; s.bpp_code = 2;
    s.phys = g.phys; s.virt = g.virt;
    if (osd_ioctl(&s, OSD_CMD_SET_BUF) != 0) {
        fprintf(stderr, "[osd] %s: set buffer failed: %s\n", c->name, strerror(errno));
        return -1;
    }
    c->shown = 1;
    return 0;
}

static void canvas_hide(canvas_t *c)
{
    if (!c->ok || !c->shown) return;
    if (canvas_register(c, c->pos, 0) == 0) c->shown = 0;
}

/* Same text as the web live view overlay (index.html osdTick). */
static void compose(const osd_cfg_t *cfg, char *out, size_t cap)
{
    char ts[32] = "", mode[32] = "", fps[12] = "";
    if (cfg->show_fps) snprintf(fps, sizeof fps, "%d FPS", night_fps_now());
    if (cfg->show_time) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    }
    if (cfg->show_mode)
        snprintf(mode, sizeof mode, "%s%s", night_is_night() ? "NIGHT" : "DAY",
                 sys_time_synced() ? "" : " (clock not set)");
    const char *parts[5] = { cfg->prefix, ts, mode, fps, cfg->suffix };
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < 5; i++) {
        if (!parts[i][0]) continue;
        n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s%s", n ? "  " : "", parts[i]);
        if (n >= cap) { out[cap - 1] = 0; break; }
    }
}

static void *osd_thread(void *arg)
{
    (void)arg;
    sleep(2);                                   /* let the pipeline settle */
    g_swap_nibbles = access("/tmp/sd/maint/osd_swap", F_OK) == 0;
    if (access("/tmp/sd/maint/osd_fmt1", F_OK) == 0) g_fmt = 1;

    /* Crash guard: marker on the CONFIG partition around the first driver
     * calls; if the camera hangs/reboots in between, the next start skips
     * the hardware overlay (cleared by saving the overlay settings). */
    int gfd = open(OSD_GUARD_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (gfd >= 0) { fsync(gfd); close(gfd); sync(); }

    uint32_t pal[0x84 / 4];
    memset(pal, 0, sizeof pal);
    memcpy(pal + 1, k_palette, sizeof k_palette);
    if (osd_ioctl((struct osd_blk *)pal, OSD_CMD_PALETTE) != 0) {
        fprintf(stderr, "[osd] palette failed: %s\n", strerror(errno));
        g_state = "error";
        osd_hw_clear_guard();                   /* clean failure, not a crash */
        return NULL;
    }
    int any = 0;
    for (int i = 0; i < 2; i++) any |= canvas_init(&g_cv[i]) == 0;
    if (!any) { g_state = "error"; osd_hw_clear_guard(); return NULL; }

    char text[160];
    int first = 1;
    while (g_run) {
        osd_cfg_t cfg;
        osd_get(&cfg);
        compose(&cfg, text, sizeof text);
        int on = cfg.enabled && text[0];
        for (int i = 0; i < 2; i++) {
            canvas_t *c = &g_cv[i];
            if (!c->ok) continue;
            if (!on) { canvas_hide(c); continue; }
            if (canvas_show(c, text, cfg.pos, cfg.size == 2) != 0) {
                canvas_hide(c);
                c->ok = 0;
            }
        }
        if (first) {                            /* survived the first full cycle */
            first = 0;
            unlink(OSD_GUARD_FILE);
            sync();
        }
        g_state = (g_cv[0].ok || g_cv[1].ok) ? (on ? "on" : "off") : "error";
        struct timeval tv;                      /* wake just after the next second */
        gettimeofday(&tv, NULL);
        usleep(1000000 - tv.tv_usec + 20000);
    }
    for (int i = 0; i < 2; i++) canvas_hide(&g_cv[i]);
    return NULL;
}

void osd_hw_start(void)
{
    if (access(OSD_KILL_FILE, F_OK) == 0) {
        printf("[osd] burned-in overlay disabled by %s\n", OSD_KILL_FILE);
        g_state = "disabled";
        return;
    }
    if (access(OSD_GUARD_FILE, F_OK) == 0) {
        fprintf(stderr, "[osd] burned-in overlay skipped: previous start did not complete (%s)\n",
                OSD_GUARD_FILE);
        g_state = "guard";
        return;
    }
    g_state = "starting";
    g_run = 1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&g_thr, &a, osd_thread, NULL) != 0) { g_run = 0; g_state = "error"; }
    pthread_attr_destroy(&a);
}

void osd_hw_stop(void)
{
    if (!g_run) return;
    g_run = 0;
    pthread_join(g_thr, NULL);
}
