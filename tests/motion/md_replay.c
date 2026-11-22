/* Replay a recorded grid log through md_logic (no camera needed).
 *   md_replay <log> [setting=value ...]
 * Log lines (one frame each):
 *   [<ms>] {"hdr":..,"rows":[[32 numbers] x 24],...}   the output of GET /api/md/raw
 *   <ms> <768 numbers>                                  hardware statistic, row-major
 *   <ms> <576 numbers>                                  software block means (32x18)
 * Without a time stamp frames are 100 ms apart. Settings use the md.* names
 * without the prefix: sensitivity, hold, zones (hex mask), zone_sens, min_size,
 * persist, light, line, line_dir (both|left|right), loiter, loiter_zone, and
 * night=1 (hardware 2-of-3 rule), ignore_rows=r0-r1 (overlay rows to drop),
 * -v (print every frame's score and tracks).
 * Prints motion / crossing / loitering / light events with their times. */
#include "md_logic.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mdl_t g_st;
static mdl_out_t g_out;

static int zones_hex(const char *v, uint8_t *z)
{
    if (!*v) { memset(z, 1, MDL_COLS * MDL_ROWS_SW); return 0; }
    if (strlen(v) != MDL_COLS * MDL_ROWS_SW / 4) return -1;
    for (int i = 0; i < MDL_COLS * MDL_ROWS_SW; i++) {
        char c = v[i / 4];
        if (!isxdigit((unsigned char)c)) return -1;
        int d = isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
        z[i] = (uint8_t)((d >> (3 - i % 4)) & 1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: md_replay <log> [setting=value ...] [-v]\n"); return 2; }
    mdl_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sens = 50; cfg.hold_s = 10;
    uint8_t zone[MDL_COLS * MDL_ROWS_SW];
    memset(zone, 1, sizeof zone);
    int night = 0, verbose = 0, ig0 = -1, ig1 = -1;
    for (int a = 2; a < argc; a++) {
        char *k = argv[a], *v = strchr(k, '=');
        if (!strcmp(k, "-v")) { verbose = 1; continue; }
        if (!v) { fprintf(stderr, "bad argument %s\n", k); return 2; }
        *v++ = 0;
        int bad = 0;
        if (!strcmp(k, "sensitivity")) cfg.sens = atoi(v);
        else if (!strcmp(k, "hold")) cfg.hold_s = atoi(v);
        else if (!strcmp(k, "zones")) bad = zones_hex(v, zone);
        else if (!strcmp(k, "zone_sens")) { int n = mdl_parse_zones(v, cfg.zone, MDL_MAX_ZONES); bad = n < 0; cfg.nzones = n > 0 ? n : 0; }
        else if (!strcmp(k, "min_size")) cfg.min_size = atoi(v);
        else if (!strcmp(k, "persist")) cfg.persist_ms = atoi(v);
        else if (!strcmp(k, "light")) cfg.light_pct = atoi(v);
        else if (!strcmp(k, "line")) { int r = mdl_parse_line(v, cfg.line); bad = r < 0; cfg.line_on = r == 1; }
        else if (!strcmp(k, "line_dir")) cfg.line_dir = !strcmp(v, "left") ? 1 : !strcmp(v, "right") ? 2 : 0;
        else if (!strcmp(k, "loiter")) cfg.loiter_s = atoi(v);
        else if (!strcmp(k, "loiter_zone")) cfg.loiter_zone = atoi(v);
        else if (!strcmp(k, "night")) night = atoi(v);
        else if (!strcmp(k, "ignore_rows")) bad = sscanf(v, "%d-%d", &ig0, &ig1) != 2;
        else bad = 1;
        if (bad) { fprintf(stderr, "bad setting %s=%s\n", k, v); return 2; }
    }
    FILE *f = strcmp(argv[1], "-") ? fopen(argv[1], "r") : stdin;
    if (!f) { perror(argv[1]); return 2; }
    mdl_init(&g_st);
    static char line[65536];
    long nframes = 0, nmotion = 0, ncross = 0, nloiter = 0, nlight = 0;
    int64_t t = 0;
    int motion = 0, loiter = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '#') continue;
        int stamped = 0;
        if (isdigit((unsigned char)*p)) { t = strtoll(p, &p, 10); stamped = 1; }
        char *rows = strstr(p, "\"rows\"");
        if (rows) p = rows + 6;
        static double v[MDL_MAX_CELLS + 1];
        int n = 0;
        while (*p && n <= MDL_MAX_CELLS) {
            if (isdigit((unsigned char)*p) || ((*p == '-' || *p == '.') && isdigit((unsigned char)p[1]))) v[n++] = strtod(p, &p);
            else p++;
            if (rows && *p == ']' && p[1] == ']' ) break;      /* end of the rows array */
        }
        int hw = n == MDL_MAX_CELLS, sw = n == MDL_COLS * MDL_ROWS_SW;
        if (!hw && !sw) { fprintf(stderr, "line %ld: %d numbers (want 768 or 576), skipped\n", nframes + 1, n); continue; }
        if (!stamped) t += hw ? 100 : 250;
        int rows_n = hw ? MDL_ROWS_HW : MDL_ROWS_SW;
        uint8_t watch[MDL_MAX_CELLS], watch_all[MDL_MAX_CELLS];
        for (int r = 0; r < rows_n; r++)
            for (int c = 0; c < MDL_COLS; c++) {
                watch_all[r * MDL_COLS + c] = (uint8_t)!(r >= ig0 && r <= ig1);
                watch[r * MDL_COLS + c] = (uint8_t)(zone[(r * MDL_ROWS_SW / rows_n) * MDL_COLS + c] && watch_all[r * MDL_COLS + c]);
            }
        mdl_in_t in = { t, night, watch, watch_all };
        if (hw) {
            uint16_t g[MDL_MAX_CELLS];
            for (int i = 0; i < MDL_MAX_CELLS; i++) g[i] = (uint16_t)(v[i] < 0 ? 0 : v[i] > 65535 ? 65535 : v[i]);
            mdl_hw_frame(&g_st, &cfg, g, &in, &g_out);
        } else {
            float cur[MDL_COLS * MDL_ROWS_SW];
            for (int i = 0; i < MDL_COLS * MDL_ROWS_SW; i++) cur[i] = (float)v[i];
            if (mdl_sw_frame(&g_st, &cfg, cur, &in, &g_out)) { nframes++; continue; }
        }
        nframes++;
        if (g_out.light) { nlight++; printf("%10lld light change (suppressed)\n", (long long)t); }
        if (g_out.motion != motion) { motion = g_out.motion; nmotion += motion; printf("%10lld motion %s\n", (long long)t, motion ? "ON" : "off"); }
        for (int i = 0; i < g_out.ncross; i++, ncross++)
            printf("%10lld line crossed by track %d toward the %s\n", (long long)t, g_out.cross[i].id, g_out.cross[i].dir == 1 ? "left" : "right");
        if (g_out.loiter != loiter) { loiter = g_out.loiter; nloiter += loiter; printf("%10lld loitering %s (track %d)\n", (long long)t, loiter ? "ON" : "off", g_out.loiter_id); }
        if (verbose) {
            printf("%10lld score %d", (long long)t, g_out.score);
            if (hw) printf(" max %d p90 %d", g_out.hwmax, g_out.hwp90);
            for (int i = 0; i < g_out.ntracks; i++) printf(" [%d %d,%d]", g_out.track[i].id, g_out.track[i].x, g_out.track[i].y);
            printf("\n");
        }
    }
    if (f != stdin) fclose(f);
    printf("frames %ld, motion events %ld, line crossings %ld, loitering events %ld, light changes %ld\n", nframes, nmotion, ncross, nloiter, nlight);
    return 0;
}
