/* Host test: render overlay text with osd_hw.c's renderer and write a PGM
 * (white text, black outline, grey edges, mid-grey = transparent).
 * Also checks bounds: long text is truncated inside the canvas. */
#include "../../src/ak_rtsp/osd_hw.c"
#include <assert.h>
int g_isp_fd = -1;
int night_is_night(void) { return 0; }
int sys_time_synced(void) { return 1; }
void osd_get(osd_cfg_t *o) { memset(o, 0, sizeof *o); }

static void dump(canvas_t *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P5 %d %d 255\n", c->cw, c->ch);
    static const unsigned char lut[4] = { 100, 255, 0, 170 };
    for (int i = 0; i < c->cw * c->ch; i++) fputc(lut[c->cov[i]], f);
    fclose(f);
}
int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    char p[256];
    for (int i = 0; i < 2; i++) {
        canvas_t *c = &g_cv[i];
        int maxw = c->chn == 0 ? 1536 : 852;
        c->cw = (c->frame_w - 2 * c->margin) & ~15; if (c->cw > maxw) c->cw = maxw;
        c->ch = (c->font[1]->h + 2 * c->outline[1] + 1) & ~1;
        c->cov = malloc((size_t)c->cw * c->ch);
        assert(c->cw % 16 == 0 && c->ch % 2 == 0);
        osd_cfg_t cfg = { 1, 1, 1, "Front door", "cam 1", "tl", 2 };
        char text[160];
        compose(&cfg, text, sizeof text);
        printf("%s: canvas %dx%d text '%s'\n", c->name, c->cw, c->ch, text);
        canvas_render(c, text, 0, 0, 1);
        snprintf(p, sizeof p, "%s/osd_%s_left.pgm", out, c->name); dump(c, p);
        canvas_render(c, text, 1, 1, 0);
        snprintf(p, sizeof p, "%s/osd_%s_right.pgm", out, c->name); dump(c, p);
        char lng[200]; memset(lng, 'W', 199); lng[199] = 0;
        for (int big = 0; big < 2; big++)
            for (int corner = 0; corner < 4; corner++) {
                canvas_render(c, lng, corner & 1, corner >> 1, big);   /* must not overflow (ASan) */
                canvas_render(c, "\x01\x7f\xff", corner & 1, corner >> 1, big);
            }
    }
    puts("ok");
    return 0;
}
