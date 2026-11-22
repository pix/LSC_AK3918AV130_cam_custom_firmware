#include "recidx.h"
#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* days since 1970-01-01 of a proleptic Gregorian date (no TZ, no overflow for 0..9999) */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void recidx_iso(time_t t, char out[24])
{
    struct tm tm;
    if (!gmtime_r(&t, &tm)) { snprintf(out, 24, "1970-01-01T00:00:00Z"); return; }
    snprintf(out, 24, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* n digits -> value, -1 if not all digits */
static int digits(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

int64_t recidx_parse_time_ms(const char *s)
{
    if (!s) return -1;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    int y, mo, d, h, mi, se, ext;
    y = digits(s, 4);
    if (y < 0) return -1;
    ext = s[4] == '-';                          /* 2026-11-22T10:00:00 or 20261122T100000 */
    /* every step stops at the first unexpected character: never past the NUL */
    const char *p = s + 4 + ext;
    if ((mo = digits(p, 2)) < 0) return -1;
    p += 2;
    if (ext && *p++ != '-') return -1;
    if ((d = digits(p, 2)) < 0) return -1;
    p += 2;
    if (*p != 'T' && *p != 't') return -1;
    p++;
    if ((h = digits(p, 2)) < 0) return -1;
    p += 2;
    if (ext && *p++ != ':') return -1;
    if ((mi = digits(p, 2)) < 0) return -1;
    p += 2;
    if (ext && *p++ != ':') return -1;
    if ((se = digits(p, 2)) < 0) return -1;
    p += 2;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) return -1;
    if (y < 1970 || y > 2200) return -1;
    int ms = 0;
    if (*p == '.' || *p == ',') {               /* fraction: first 3 digits count */
        p++;
        int k = 0;
        while (isdigit((unsigned char)*p)) { if (k < 3) { ms = ms * 10 + (*p - '0'); k++; } p++; }
        while (k < 3) { ms *= 10; k++; }
    }
    int64_t off = 0;                            /* Z, +hh:mm, -hhmm or nothing (UTC) */
    if (*p == '+' || *p == '-') {
        int sg = *p == '-' ? -1 : 1;
        p++;
        int oh = digits(p, 2), om = 0;
        if (oh < 0 || oh > 23) return -1;
        p += 2;
        if (*p == ':') p++;
        if (isdigit((unsigned char)*p)) { om = digits(p, 2); if (om < 0 || om > 59) return -1; }
        off = sg * (int64_t)(oh * 3600 + om * 60);
    }
    int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - off;
    return t * 1000 + ms;
}

time_t recidx_name_time(const char *day, const char *name)
{
    if (!day || !name || strlen(day) != 10 || strlen(name) < 8) return -1;
    int y = digits(day, 4), mo = digits(day + 5, 2), d = digits(day + 8, 2);
    int h = digits(name, 2), mi = digits(name + 3, 2), se = digits(name + 6, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59 ||
        name[2] != '-' || name[5] != '-')
        return -1;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
    tm.tm_isdst = -1;                           /* local time of the camera (TZ setting) */
    return mktime(&tm);
}

/* kind from "HH-MM-SS_<kind>.mp4"; -1 = not a replayable recording */
static int name_kind(const char *name)
{
    size_t l = strlen(name);
    if (l < 13 || strcmp(name + l - 4, ".mp4") || name[8] != '_') return -1;
    const char *k = name + 9;
    size_t kl = l - 4 - 9;
    if (kl == 6 && !strncmp(k, "motion", 6)) return RK_MOTION;
    if (kl == 5 && !strncmp(k, "sound", 5)) return RK_SOUND;
    if (kl == 5 && !strncmp(k, "event", 5)) return RK_EVENT;
    if (kl == 4 && !strncmp(k, "cont", 4)) return RK_CONT;
    return -1;                                  /* timelapse and anything else */
}

static int fill(const char *day, const char *name, recfile_t *f)
{
    char path[300];
    int kind = name_kind(name);
    if (kind < 0 || strlen(name) >= sizeof f->name || record_path(day, name, path, sizeof path) != 0) return -1;
    time_t st = recidx_name_time(day, name);
    if (st == (time_t)-1) return -1;
    struct stat sb;
    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) return -1;
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->kind = (uint8_t)kind;
    f->start = st;
    f->size = (int64_t)sb.st_size;
    f->live = (uint8_t)(record_is_live(day, name) != 0);
    f->end = f->live ? time(NULL) : sb.st_mtime;
    if (f->end < f->start) f->end = f->start;
    return 0;
}

int recidx_file(const char *day, const char *name, recfile_t *out)
{
    return fill(day, name, out);
}

static int cmp_day(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int cmp_file(const void *a, const void *b) { return strcmp(((const recfile_t *)a)->name, ((const recfile_t *)b)->name); }

int recidx_days(char (*days)[11], int max)
{
    DIR *d = opendir(REC_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max)
        if (strlen(e->d_name) == 10 && record_day_ok(e->d_name)) memcpy(days[n++], e->d_name, 11);
    closedir(d);
    qsort(days, (size_t)n, sizeof days[0], cmp_day);
    return n;
}

int recidx_files(const char *day, recfile_t *out, int max)
{
    if (!day || strlen(day) != 10 || !record_day_ok(day)) return 0;
    char dp[256];
    snprintf(dp, sizeof dp, "%s/%s", REC_DIR, day);
    DIR *d = opendir(dp);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max)
        if (fill(day, e->d_name, &out[n]) == 0) n++;
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_file);
    return n;
}

int recidx_parse_token(const char *token, char day[11], char name[44])
{
    size_t l = token ? strlen(token) : 0;
    if (l < 10 || l >= RECIDX_TOKEN_MAX) return -1;
    for (size_t i = 0; i < l; i++)
        if (!(isalnum((unsigned char)token[i]) || token[i] == '-' || token[i] == '_')) return -1;
    memcpy(day, token, 10);
    day[10] = 0;
    if (!record_day_ok(day)) return -1;
    name[0] = 0;
    if (l == 10) return 0;
    if (token[10] != '_' || l - 11 + 4 >= 44) return -1;
    snprintf(name, 44, "%s.mp4", token + 11);
    char path[300];
    if (record_path(day, name, path, sizeof path) != 0 || name_kind(name) < 0 || recidx_name_time(day, name) == (time_t)-1) {
        name[0] = 0;
        return -1;
    }
    return 0;
}

void recidx_file_token(const char *day, const char *name, char *out, size_t cap)
{
    size_t l = strlen(name);
    if (l > 4 && !strcmp(name + l - 4, ".mp4")) l -= 4;
    snprintf(out, cap, "%s_%.*s", day, (int)l, name);
}
