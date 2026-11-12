#include "osd.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static osd_cfg_t g_cfg = { 1, 1, 1, "", "", "tl", 2 };

int osd_text_ok(const char *s)
{
    size_t n = strlen(s);
    if (n > OSD_TEXT_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return 0;
    return 1;
}

static int pos_ok(const char *p)
{
    return !strcmp(p, "tl") || !strcmp(p, "tr") || !strcmp(p, "bl") || !strcmp(p, "br");
}

void osd_init(void)
{
    osd_cfg_t c = { 1, 1, 1, "", "", "tl", 2 };
    FILE *f = fopen(OSD_FILE, "r");
    if (f) {
        char line[96];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *v = eq + 1;
            if (!strcmp(line, "enabled")) c.enabled = v[0] == '1';
            else if (!strcmp(line, "show_time")) c.show_time = v[0] == '1';
            else if (!strcmp(line, "show_mode")) c.show_mode = v[0] == '1';
            else if (!strcmp(line, "show_fps")) c.show_fps = v[0] == '1';
            else if (!strcmp(line, "prefix") && osd_text_ok(v)) snprintf(c.prefix, sizeof(c.prefix), "%s", v);
            else if (!strcmp(line, "suffix") && osd_text_ok(v)) snprintf(c.suffix, sizeof(c.suffix), "%s", v);
            else if (!strcmp(line, "pos") && pos_ok(v)) snprintf(c.pos, sizeof(c.pos), "%s", v);
            else if (!strcmp(line, "size") && (v[0] == '1' || v[0] == '2') && !v[1]) c.size = v[0] - '0';
        }
        fclose(f);
    }
    pthread_mutex_lock(&g_lock);
    g_cfg = c;
    pthread_mutex_unlock(&g_lock);
}

void osd_get(osd_cfg_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_cfg;
    pthread_mutex_unlock(&g_lock);
}

int osd_set(const osd_cfg_t *in, const char **err)
{
    if (!osd_text_ok(in->prefix) || !osd_text_ok(in->suffix)) {
        *err = "text: up to 32 plain characters (letters, digits, punctuation, spaces)";
        return -1;
    }
    if (!pos_ok(in->pos)) { *err = "position must be tl, tr, bl or br"; return -1; }
    if (in->size != 1 && in->size != 2) { *err = "size must be 1 or 2"; return -1; }

    const char *tmp = OSD_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { *err = "cannot save"; return -1; }
    fprintf(f, "enabled=%d\nshow_time=%d\nshow_mode=%d\nshow_fps=%d\nprefix=%s\nsuffix=%s\npos=%s\nsize=%d\n",
            in->enabled ? 1 : 0, in->show_time ? 1 : 0, in->show_mode ? 1 : 0, in->show_fps ? 1 : 0,
            in->prefix, in->suffix, in->pos, in->size);
    fflush(f);
    int ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, OSD_FILE) != 0) { unlink(tmp); *err = "cannot save"; return -1; }

    pthread_mutex_lock(&g_lock);
    g_cfg = *in;
    g_cfg.enabled = !!in->enabled; g_cfg.show_time = !!in->show_time; g_cfg.show_mode = !!in->show_mode; g_cfg.show_fps = !!in->show_fps;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static size_t jstr(char *o, size_t cap, const char *s)
{
    size_t n = 0;
    if (n < cap) o[n++] = '"';
    for (; *s && n + 3 < cap; s++) {
        if (*s == '"' || *s == '\\') o[n++] = '\\';
        o[n++] = *s;          /* osd_text_ok() guarantees printable ASCII */
    }
    if (n < cap) o[n++] = '"';
    if (n < cap) o[n] = 0;
    return n;
}

int osd_json(char *out, size_t cap)
{
    osd_cfg_t c;
    osd_get(&c);
    char p[80], s[80];
    jstr(p, sizeof(p), c.prefix);
    jstr(s, sizeof(s), c.suffix);
    return snprintf(out, cap,
        "{\"enabled\":%s,\"show_time\":%s,\"show_mode\":%s,\"show_fps\":%s,\"prefix\":%s,\"suffix\":%s,\"pos\":\"%s\",\"size\":%d}",
        c.enabled ? "true" : "false", c.show_time ? "true" : "false",
        c.show_mode ? "true" : "false", c.show_fps ? "true" : "false", p, s, c.pos, c.size);
}
