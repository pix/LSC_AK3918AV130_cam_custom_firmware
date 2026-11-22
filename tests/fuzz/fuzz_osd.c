/* Fuzz harness: OSD settings (osd.c: osd_set/osd_init/osd_json, the
 * /api/osd POST path and the saved OSD_FILE) and the burned-in overlay
 * renderer (osd_hw.c compose() + canvas_render() with osd_font), fed with
 * arbitrary text (all byte values, long strings).
 * byte 0 % 3: 0 = settings as the web handler builds them, 1 = OSD_FILE
 * content, 2 = raw text straight into the renderer. */
#include "osd_hw.c"
#include <stdint.h>
int g_isp_fd = -1;     /* main.c on the camera; osd_ioctl() is never reached here */

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < 2; i++) {          /* same canvas sizes as canvas_init() */
        canvas_t *c = &g_cv[i];
        int maxw = c->chn == 0 ? 1536 : 852;
        c->cw = (c->frame_w - 2 * c->margin) & ~15; if (c->cw > maxw) c->cw = maxw;
        c->ch = (c->font[1]->h + 2 * c->outline[1] + 1) & ~1;
        c->cov = malloc((size_t)c->cw * c->ch);
    }
    return 0;
}

/* one corner / font size per canvas (chosen by the input): a full render
 * of the 1536-pixel canvas costs ~10 ms under ASan */
static unsigned g_sel;
static void render_all(const char *text)
{
    for (int i = 0; i < 2; i++) {
        unsigned s = g_sel >> (3 * i);
        canvas_render(&g_cv[i], text, s & 1, s >> 1 & 1, s >> 2 & 1);
    }
}

static void show_cfg(void)
{
    osd_cfg_t c;
    osd_get(&c);
    char json[320];
    osd_json(json, sizeof json);
    osd_json(json, 16);
    char text[160];
    compose(&c, text, sizeof text);
    compose(&c, text, 7);
    compose(&c, text, sizeof text);
    int x, y;
    for (int i = 0; i < 2; i++) region_xy(&g_cv[i], c.pos, &x, &y);
    render_all(text);
}

/* copy one length-prefixed field: [len u8][bytes] */
static size_t field(const uint8_t **p, const uint8_t *end, char *out, size_t cap)
{
    size_t n = 0;
    if (*p < end) { n = **p; (*p)++; }
    if (n > (size_t)(end - *p)) n = (size_t)(end - *p);
    size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(out, *p, k); out[k] = 0;
    *p += n;
    return k;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 1) return 0;
    const uint8_t *p = d + 1, *end = d + n;
    g_sel = d[0] >> 2;
    switch (d[0] % 3) {
    case 0: {          /* web: form fields -> osd_cfg_t -> osd_set (as web.c /api/osd POST) */
        osd_cfg_t c;
        osd_get(&c);
        uint8_t fl = p < end ? *p++ : 0;
        c.enabled = fl & 1; c.show_time = fl >> 1 & 1; c.show_mode = fl >> 2 & 1; c.show_fps = fl >> 3 & 1;
        char b[8], t[96];
        field(&p, end, b, sizeof b); snprintf(c.pos, sizeof(c.pos), "%.2s", b);
        field(&p, end, b, sizeof b); c.size = (b[0] == '1' || b[0] == '2') && !b[1] ? b[0] - '0' : 0;
        if (fl & 16) c.size = (int)(int8_t)b[0];
        if (field(&p, end, t, sizeof t) <= OSD_TEXT_MAX) snprintf(c.prefix, sizeof(c.prefix), "%s", t);
        if (field(&p, end, t, sizeof t) <= OSD_TEXT_MAX) snprintf(c.suffix, sizeof(c.suffix), "%s", t);
        const char *err;
        osd_set(&c, &err);
        osd_init();      /* read back what was saved */
        show_cfg();
        break;
    }
    case 1: {          /* saved settings file */
        FILE *f = fopen(OSD_FILE, "wb");
        if (!f) return 0;
        fwrite(p, 1, (size_t)(end - p), f);
        fclose(f);
        osd_init();
        show_cfg();
        break;
    }
    case 2: {          /* arbitrary text into the renderer */
        size_t k = (size_t)(end - p);
        char *s = malloc(k + 1);
        memcpy(s, p, k); s[k] = 0;
        render_all(s);
        free(s);
        break;
    }
    }
    return 0;
}
