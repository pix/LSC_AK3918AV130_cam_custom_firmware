/* Push notifications (ntfy, Telegram, Pushover, Discord, Gotify). See notify.h
 * and docs/notifications.md.
 *
 * notify_event() (events thread) and notify_test() (web thread) only apply
 * the limits and put a small job in a queue of NQ; one sender thread does
 * the rest: snapshot JPEG -> first message; then, if notify.clip is on, a
 * short MP4 from the live hub as a second, quiet message (Pushover and
 * Gotify take no video). Memory: the JPEG (<= 200 KB) and the clip
 * (<= CLIP_MAX, one at a time) are allocated per job and freed after it.
 *
 * Limits: per-kind cooldown; notify.max per notify.window_min (the rest is
 * counted and reported in one summary when the window ends); at most TRIES
 * attempts per message (network errors before the request was sent, 5xx,
 * 429 with Retry-After <= 30 s); connect 10 s, TLS handshake 15 s, send
 * >= 16 KB/s after 30 s, response 30 s. Secrets (tokens, the URL path, which
 * holds the ntfy topic or the Discord webhook token) are never logged. */
#define _GNU_SOURCE
#include "notify.h"
#include "tls.h"
#include "conn.h"
#include "control.h"
#include "ctrl_params.h"
#include "monotime.h"
#include "snapshot.h"
#include "system.h"
#include "live.h"
#include "fmp4.h"
#include "venc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "mbedtls/base64.h"

#define NQ          4                      /* queued jobs (+1 being sent) */
#define JPG_MAX     (200 * 1024)
#ifndef NOTIFY_CLIP_MAX
#define NOTIFY_CLIP_MAX (2 * 1024 * 1024)  /* hard cap of a clip in RAM */
#endif
#define RESP_CAP    16384                  /* we only need the status and a short JSON */
#define LOG_N       16
#define CONNECT_MS  10000
#define TLS_MS      15000
#define RESP_MS     30000
#define TRIES       3
#define RETRY_MAX_MS 30000                 /* a longer Retry-After: give up */
#define DISCORD_MAX (10 * 1024 * 1024)     /* webhook attachment limit without boosts */

static const char *const k_sv[NOTIFY_NSERVICES] = { "ntfy", "telegram", "pushover", "discord", "gotify" };
#define NKIND 5
static const char *const k_kind[NKIND] = { "motion", "sound", "tamper", "line", "loiter" };   /* line / loiter: md_logic.c */

/* ---- settings ------------------------------------------------------------------ */
static int  s_en, s_sv, s_cool = 60, s_snap = 1, s_max = 10, s_win = 60, s_clip, s_clip_s = 5, s_prio = 5;
static unsigned s_evmask = 1;              /* bit per k_kind */
static char s_url[256], s_token[160], s_user[96], s_title[100], s_img[256];
static volatile unsigned g_gen;            /* bumped on every settings change */

typedef struct { int tls, port; char host[128], path[400]; } url_t;

static int url_parse(const char *u, url_t *o)
{
    const char *p;
    memset(o, 0, sizeof *o);
    if (!strncmp(u, "https://", 8)) { o->tls = 1; o->port = 443; p = u + 8; }
    else if (!strncmp(u, "http://", 7)) { o->port = 80; p = u + 7; }
    else return -1;
    size_t hl = strcspn(p, ":/?");
    if (!hl || hl >= sizeof o->host) return -1;
    for (size_t i = 0; i < hl; i++) if (!(isalnum((unsigned char)p[i]) || p[i] == '.' || p[i] == '-')) return -1;
    memcpy(o->host, p, hl);
    p += hl;
    if (*p == ':') {
        char *e;
        long port = strtol(p + 1, &e, 10);
        if (e == p + 1 || port < 1 || port > 65535) return -1;
        o->port = (int)port;
        p = e;
    }
    if (*p && *p != '/' && *p != '?') return -1;
    if (strlen(p) + 2 > sizeof o->path) return -1;
    snprintf(o->path, sizeof o->path, "%s%s", *p == '/' ? "" : "/", p);
    for (const char *q = o->path; *q; q++)
        if ((unsigned char)*q <= 0x20 || (unsigned char)*q >= 0x7f || *q == '"' || *q == '\\') return -1;
    return 0;
}

static int url_ok(const char *v) { url_t u; return !*v || url_parse(v, &u) == 0; }
static int img_ok(const char *v)          /* goes into Markdown: no brackets */
{
    url_t u;
    if (!*v) return 1;
    return url_parse(v, &u) == 0 && !strpbrk(v, "()[]<>");
}
static int tok_ok(const char *v)          /* goes into headers / URLs */
{
    for (; *v; v++) if ((unsigned char)*v <= 0x20 || (unsigned char)*v >= 0x7f || *v == '"' || *v == '\\') return 0;
    return 1;
}
static int user_ok(const char *v)         /* chat id, @channel, user key, channel id */
{
    for (; *v; v++) if (!isalnum((unsigned char)*v) && !strchr("@_.-", *v)) return 0;
    return 1;
}

CTRL_BOOL(p_en, s_en)
CTRL_STR(p_url, s_url, url_ok, "must be an http:// or https:// URL")
CTRL_STR(p_token, s_token, tok_ok, "no spaces or quotes")
CTRL_STR(p_user, s_user, user_ok, "letters, digits, '@', '_', '.' and '-' only")
CTRL_STR(p_title, s_title, NULL, "at most 99 characters")
CTRL_STR(p_img, s_img, img_ok, "must be an http:// or https:// URL without brackets")
CTRL_INT(p_cool, s_cool, 0, 86400)
CTRL_BOOL(p_snap, s_snap)
CTRL_INT(p_max, s_max, 1, 1000)
CTRL_INT(p_win, s_win, 1, 1440)
CTRL_BOOL(p_clip, s_clip)
CTRL_INT(p_clip_s, s_clip_s, 3, 10)
CTRL_INT(p_prio, s_prio, 0, 10)
static int p_sv_get(char *o, size_t c) { snprintf(o, c, "%s", k_sv[s_sv]); return 0; }
static int p_sv_set(const char *v, const char **e)
{
    for (int i = 0; i < NOTIFY_NSERVICES; i++) if (!strcmp(v, k_sv[i])) { s_sv = i; return 0; }
    *e = "must be ntfy, telegram, pushover, discord or gotify";
    return -1;
}
static int p_ev_get(char *o, size_t c)
{
    size_t n = 0;
    o[0] = 0;
    for (int i = 0; i < NKIND; i++)
        if (s_evmask & (1u << i)) n += (size_t)snprintf(o + n, n < c ? c - n : 0, "%s%s", n ? "," : "", k_kind[i]);
    return 0;
}
static int p_ev_set(const char *v, const char **e)
{
    unsigned m = 0;
    char w[16];
    while (*v) {
        size_t l = strcspn(v, ", ");
        if (l) {
            if (l >= sizeof w) goto bad;
            memcpy(w, v, l); w[l] = 0;
            int k = -1;
            for (int i = 0; i < NKIND; i++) if (!strcmp(w, k_kind[i])) k = i;
            if (k < 0) goto bad;
            m |= 1u << k;
        }
        v += l;
        while (*v == ',' || *v == ' ') v++;
    }
    s_evmask = m;
    return 0;
bad:
    *e = "motion, sound, tamper, line and/or loiter, comma separated";
    return -1;
}
#define WRAP(fn) static int fn##_setg(const char *v, const char **e) { int r = fn##_set(v, e); if (!r) g_gen++; return r; }
WRAP(p_en) WRAP(p_sv) WRAP(p_url) WRAP(p_token) WRAP(p_user) WRAP(p_title) WRAP(p_img) WRAP(p_ev) WRAP(p_cool)
WRAP(p_snap) WRAP(p_max) WRAP(p_win) WRAP(p_clip) WRAP(p_clip_s) WRAP(p_prio)

/* One consistent copy of the settings for a job (the page may change them). */
typedef struct {
    int sv, snap, clip, clip_s, prio;
    char url[256], token[160], user[96], title[100], img[256];
} cfg_t;

static void cfg_snapshot(cfg_t *c)
{
    char v[16];
    memset(c, 0, sizeof *c);
    control_get_value("notify.url", c->url, sizeof c->url, 1);
    control_get_value("notify.token", c->token, sizeof c->token, 1);
    control_get_value("notify.user", c->user, sizeof c->user, 1);
    control_get_value("notify.title", c->title, sizeof c->title, 1);
    control_get_value("notify.image_url", c->img, sizeof c->img, 1);
    control_get_value("notify.service", v, sizeof v, 1);
    for (int i = 0; i < NOTIFY_NSERVICES; i++) if (!strcmp(v, k_sv[i])) c->sv = i;
    control_get_value("notify.snapshot", v, sizeof v, 1); c->snap = !strcmp(v, "on");
    control_get_value("notify.clip", v, sizeof v, 1);     c->clip = !strcmp(v, "on");
    control_get_value("notify.clip_s", v, sizeof v, 1);   c->clip_s = atoi(v);
    control_get_value("notify.priority", v, sizeof v, 1); c->prio = atoi(v);
    if (c->clip_s < 3 || c->clip_s > 10) c->clip_s = 5;
    size_t l = strlen(c->url);
    while (l && c->url[l - 1] == '/' && c->sv != NOTIFY_NTFY) c->url[--l] = 0;   /* API base URLs */
}

static int bot_token_ok(const char *t)    /* Telegram: goes into the URL path */
{
    if (!*t) return 0;
    for (; *t; t++) if (!isalnum((unsigned char)*t) && *t != ':' && *t != '_' && *t != '-') return 0;
    return 1;
}
static int digits_ok(const char *t) { if (!*t) return 0; for (; *t; t++) if (!isdigit((unsigned char)*t)) return 0; return 1; }

/* What is missing for the chosen service (no network). */
static int cfg_check(const cfg_t *c, char *err, size_t cap)
{
    switch (c->sv) {
    case NOTIFY_NTFY:
        if (!c->url[0]) { snprintf(err, cap, "ntfy: set the topic URL (e.g. https://ntfy.sh/your-topic)"); return -1; }
        break;
    case NOTIFY_TELEGRAM:
        if (!bot_token_ok(c->token)) { snprintf(err, cap, "Telegram: set the bot token (from @BotFather)"); return -1; }
        if (!c->user[0]) { snprintf(err, cap, "Telegram: set the chat id"); return -1; }
        break;
    case NOTIFY_PUSHOVER:
        if (!c->token[0] || !c->user[0]) { snprintf(err, cap, "Pushover: set the application token and the user key"); return -1; }
        break;
    case NOTIFY_DISCORD:
        if (!c->token[0] && !c->url[0]) { snprintf(err, cap, "Discord: set a webhook URL, or a bot token and a channel id"); return -1; }
        if (c->token[0] && !digits_ok(c->user)) { snprintf(err, cap, "Discord bot: set the channel id (digits)"); return -1; }
        break;
    case NOTIFY_GOTIFY:
        if (!c->url[0] || !c->token[0]) { snprintf(err, cap, "Gotify: set the server URL and the application token"); return -1; }
        break;
    }
    return 0;
}

/* ---- status and log ------------------------------------------------------------ */
static pthread_mutex_t g_st = PTHREAD_MUTEX_INITIALIZER;
static char g_log[LOG_N][160];
static unsigned g_logn;
static char g_state[12] = "idle", g_err[200], g_clip_note[160];
static time_t g_last_ok;
static unsigned g_sent, g_failed, g_clips, g_clip_failed;
static size_t g_last_clip_bytes, g_clip_limit;     /* g_clip_limit: skip clips this large (after a 413; 0 = none) */
static int g_last_clip_ms;
static unsigned g_clip_limit_gen;

static void clean(char *s)                /* for JSON and logs */
{
    for (; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f) *s = '\'';
}

static void nlog(const char *fmt, ...)
{
    char m[140];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    clean(m);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    pthread_mutex_lock(&g_st);
    snprintf(g_log[g_logn++ % LOG_N], sizeof g_log[0], "%02d:%02d:%02d %s", tm.tm_hour, tm.tm_min, tm.tm_sec, m);
    pthread_mutex_unlock(&g_st);
    printf("[notify] %s\n", m);
    fflush(stdout);
}

/* err: the last error, kept (with its time) until the next error */
static void set_state(const char *st, const char *err)
{
    char ts[24] = "";
    if (err) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    }
    pthread_mutex_lock(&g_st);
    snprintf(g_state, sizeof g_state, "%s", st);
    if (err) { snprintf(g_err, sizeof g_err, "%s %s", ts, err); clean(g_err); }
    pthread_mutex_unlock(&g_st);
}

/* ---- queue and limits ------------------------------------------------------------ */
typedef struct { char kind[8]; int level, test, summary; time_t t; int64_t mono; } job_t;
static pthread_mutex_t g_ql = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_qc = PTHREAD_COND_INITIALIZER;
static job_t g_q[NQ];
static int g_qh, g_qn, g_busy, g_test_pending;
static int64_t g_last_ev[NKIND];
static int g_have_last[NKIND];
static int64_t g_win_start;                /* 0 = no window open */
static int g_win_count;
static unsigned g_supp, g_supp_total, g_drop_queue, g_drop_cool, g_summaries;

static int enqueue_locked(const job_t *j)
{
    if (g_qn >= NQ) return -1;
    g_q[(g_qh + g_qn) % NQ] = *j;
    g_qn++;
    pthread_cond_signal(&g_qc);
    return 0;
}

/* The window ends notify.window_min after its first notification; what was
 * suppressed in it goes out as one summary. */
static void win_tick_locked(int64_t now)
{
    if (!s_en) {                           /* off: limits start over when turned on again */
        g_win_start = 0; g_win_count = 0; g_supp = 0;
        memset(g_have_last, 0, sizeof g_have_last);
        return;
    }
    if (!g_win_start || now - g_win_start < (int64_t)s_win * 60000) return;
    if (g_supp) {
        job_t j;
        memset(&j, 0, sizeof j);
        snprintf(j.kind, sizeof j.kind, "summary");
        j.summary = (int)g_supp; j.t = time(NULL); j.mono = now;
        if (enqueue_locked(&j) != 0) return;            /* queue full: next tick */
        g_summaries++;
        g_supp = 0;
    }
    g_win_start = 0;
    g_win_count = 0;
}

void notify_event(const char *kind, int level, time_t t)
{
    int k = -1;
    for (int i = 0; i < NKIND; i++) if (!strcmp(kind, k_kind[i])) k = i;
    if (!s_en || k < 0 || !(s_evmask & (1u << k))) return;
    int64_t now = mono_ms();
    const char *why = NULL;
    pthread_mutex_lock(&g_ql);
    win_tick_locked(now);
    if (s_cool && g_have_last[k] && now - g_last_ev[k] < (int64_t)s_cool * 1000) { g_drop_cool++; why = "cooldown"; }
    else if (g_win_start && g_win_count >= s_max) { g_supp++; g_supp_total++; why = "rate limit"; }
    else {
        job_t j;
        memset(&j, 0, sizeof j);
        snprintf(j.kind, sizeof j.kind, "%s", kind);
        j.level = level; j.t = t; j.mono = now;
        if (enqueue_locked(&j) != 0) { g_drop_queue++; why = "queue full"; }
        else {
            if (!g_win_start) { g_win_start = now ? now : 1; g_win_count = 0; }
            g_win_count++;
            g_last_ev[k] = now; g_have_last[k] = 1;
        }
    }
    pthread_mutex_unlock(&g_ql);
    if (why) printf("[notify] %s: not sent (%s)\n", kind, why);
}

int notify_test(char *err, size_t cap)
{
    cfg_t c;
    cfg_snapshot(&c);
    if (cfg_check(&c, err, cap) != 0) return -1;
    int rc = -1;
    pthread_mutex_lock(&g_ql);
    if (g_test_pending) snprintf(err, cap, "a test notification is already on its way");
    else {
        job_t j;
        memset(&j, 0, sizeof j);
        snprintf(j.kind, sizeof j.kind, "test");
        j.test = 1; j.t = time(NULL); j.mono = mono_ms();
        if (enqueue_locked(&j) != 0) snprintf(err, cap, "the notification queue is full, try again in a minute");
        else { g_test_pending = 1; rc = 0; }
    }
    pthread_mutex_unlock(&g_ql);
    return rc;
}

int notify_status_json(char *out, size_t cap)
{
    url_t u;
    char host[140] = "";
    static const char *const defhost[NOTIFY_NSERVICES] = { "", "api.telegram.org", "api.pushover.net", "discord.com", "" };
    cfg_t c;
    cfg_snapshot(&c);
    if (c.url[0] && url_parse(c.url, &u) == 0) snprintf(host, sizeof host, "%s", u.host);
    else snprintf(host, sizeof host, "%s", defhost[c.sv]);

    pthread_mutex_lock(&g_ql);
    int64_t now = mono_ms();
    win_tick_locked(now);
    int queued = g_qn + g_busy, wcount = g_win_start ? g_win_count : 0;
    long wleft = g_win_start ? (long)(((int64_t)s_win * 60000 - (now - g_win_start)) / 1000) : 0;
    if (wleft < 0) wleft = 0;
    unsigned supp = g_supp, supp_t = g_supp_total, dq = g_drop_queue, dc = g_drop_cool, sums = g_summaries;
    pthread_mutex_unlock(&g_ql);

    char ev[40];
    p_ev_get(ev, sizeof ev);
    pthread_mutex_lock(&g_st);
    char last[24] = "";
    if (g_last_ok) {
        struct tm tm;
        localtime_r(&g_last_ok, &tm);
        strftime(last, sizeof last, "%Y-%m-%d %H:%M:%S", &tm);
    }
    size_t o = (size_t)snprintf(out, cap,
        "{\"enabled\":%s,\"service\":\"%s\",\"server\":\"%s\",\"events\":\"%s\",\"state\":\"%s\",\"error\":\"%s\",\"last_ok\":\"%s\","
        "\"sent\":%u,\"failed\":%u,\"queued\":%d,\"dropped_queue\":%u,\"dropped_cooldown\":%u,"
        "\"suppressed\":%u,\"suppressed_total\":%u,\"summaries\":%u,\"window_sent\":%d,\"window_max\":%d,\"window_min\":%d,\"window_left_s\":%ld,"
        "\"clip\":%s,\"clips\":%u,\"clip_failed\":%u,\"last_clip_bytes\":%lu,\"last_clip_ms\":%d,\"clip_note\":\"%s\",\"log\":[",
        s_en ? "true" : "false", k_sv[c.sv], host, ev, g_state, g_err, last,
        g_sent, g_failed, queued, dq, dc, supp, supp_t, sums, wcount, s_max, s_win, wleft,
        s_clip ? "true" : "false", g_clips, g_clip_failed, (unsigned long)g_last_clip_bytes, g_last_clip_ms, g_clip_note);
    unsigned first = g_logn > LOG_N ? g_logn - LOG_N : 0;
    for (unsigned i = first; i < g_logn && o < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%s\"%s\"", i == first ? "" : ",", g_log[i % LOG_N]);
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]}");
    pthread_mutex_unlock(&g_st);
    return (int)(o < cap ? o : cap - 1);
}

/* ---- response parsing (fuzzed: tests/fuzz/fuzz_notify.c) ---------------------------- */
static const char *hdr_line_val(const char *l, const char *le, const char *name)
{
    size_t nl = strlen(name);
    if ((size_t)(le - l) <= nl || strncasecmp(l, name, nl) || l[nl] != ':') return NULL;
    l += nl + 1;
    while (l < le && (*l == ' ' || *l == '\t')) l++;
    return l;
}

int notify_parse_response(char *b, size_t len, notify_resp_t *r)
{
    memset(r, 0, sizeof *r);
    b[len] = 0;
    if (len < 12 || memcmp(b, "HTTP/1.", 7) || b[8] != ' ') return -1;
    if (!isdigit((unsigned char)b[9]) || !isdigit((unsigned char)b[10]) || !isdigit((unsigned char)b[11])) return -1;
    r->status = (b[9] - '0') * 100 + (b[10] - '0') * 10 + (b[11] - '0');
    char *he = memmem(b, len, "\r\n\r\n", 4);
    if (!he) { r->body = b + len; return 0; }          /* headers cut short: status only */
    long clen = -1;
    int chunked = 0;
    const char *l = memmem(b, (size_t)(he - b) + 2, "\r\n", 2);
    while (l && l < he) {
        l += 2;
        const char *le = memmem(l, (size_t)(he + 2 - l), "\r\n", 2);
        if (!le) break;
        const char *v;
        if ((v = hdr_line_val(l, le, "Content-Length"))) {
            long x = 0;
            int nd = 0;
            for (; v < le && isdigit((unsigned char)*v) && nd < 9; v++, nd++) x = x * 10 + (*v - '0');
            if (nd) clen = x;
        } else if ((v = hdr_line_val(l, le, "Transfer-Encoding"))) {
            if (memmem(v, (size_t)(le - v), "chunked", 7)) chunked = 1;
        } else if ((v = hdr_line_val(l, le, "Retry-After"))) {
            long x = 0;
            int nd = 0;
            for (; v < le && isdigit((unsigned char)*v) && nd < 6; v++, nd++) x = x * 10 + (*v - '0');
            if (nd) r->retry_after_ms = (int)((x > 3600 ? 3600 : x) * 1000);
        }
        l = le;
    }
    char *bd = he + 4;
    size_t bl = len - (size_t)(bd - b);
    if (chunked) {                                     /* de-chunk in place (out never passes in) */
        char *in = bd, *out = bd, *end = bd + bl;
        while (in < end) {
            unsigned long sz = 0;
            int nd = 0;
            for (; in < end && isxdigit((unsigned char)*in) && nd < 7; in++, nd++)
                sz = sz * 16 + (unsigned long)(isdigit((unsigned char)*in) ? *in - '0' : (tolower((unsigned char)*in) - 'a' + 10));
            if (!nd) break;
            char *le = memmem(in, (size_t)(end - in), "\r\n", 2);
            if (!le) break;
            in = le + 2;
            if (!sz) break;
            size_t avail = (size_t)(end - in);
            if (sz > avail) sz = avail;
            memmove(out, in, sz);
            out += sz; in += sz;
            if (end - in < 2) break;
            in += 2;
        }
        bl = (size_t)(out - bd);
    } else if (clen >= 0 && (size_t)clen < bl) bl = (size_t)clen;
    bd[bl] = 0;
    r->body = bd;
    r->body_len = bl;
    return 0;
}

/* Is a response in b[0..len) complete (headers + Content-Length / last chunk)? */
static int resp_complete(const char *b, size_t len)
{
    const char *he = memmem(b, len, "\r\n\r\n", 4);
    if (!he) return 0;
    size_t blen = len - (size_t)(he + 4 - b);
    const char *l = memmem(b, (size_t)(he - b) + 2, "\r\n", 2);
    while (l && l < he) {
        l += 2;
        const char *le = memmem(l, (size_t)(he + 2 - l), "\r\n", 2), *v;
        if (!le) break;
        if ((v = hdr_line_val(l, le, "Content-Length"))) return blen >= (size_t)strtoul(v, NULL, 10);
        if ((v = hdr_line_val(l, le, "Transfer-Encoding")) && memmem(v, (size_t)(le - v), "chunked", 7))
            return blen >= 5 && !memcmp(b + len - 5, "0\r\n\r\n", 5);
        l = le;
    }
    return 0;                                          /* until the server closes */
}

static const char *js_ws(const char *p, const char *e) { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; return p; }
static const char *js_str_end(const char *p, const char *e)   /* p at '"': after the closing quote, or NULL */
{
    for (p++; p < e; p++) {
        if (*p == '\\') { if (++p >= e) return NULL; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}
static const char *js_skip(const char *p, const char *e)      /* one value */
{
    if (p >= e) return NULL;
    if (*p == '"') return js_str_end(p, e);
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (p < e) {
            if (*p == '"') { if (!(p = js_str_end(p, e))) return NULL; continue; }
            if (*p == '{' || *p == '[') d++;
            else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p > s ? p : NULL;
}
static void js_copy(const char *p, const char *q, char *out, size_t cap)   /* string body p..q (q at the closing quote) */
{
    size_t n = 0;
    while (p < q && n + 1 < cap) {
        char ch = *p++;
        if (ch == '\\' && p < q) {
            char x = *p++;
            switch (x) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'b': case 'f': ch = ' '; break;
            case 'u': ch = '?'; for (int i = 0; i < 4 && p < q && isxdigit((unsigned char)*p); i++) p++; break;
            default: ch = x;
            }
        }
        out[n++] = ch;
    }
    out[n] = 0;
}

int notify_json_get(const char *js, size_t len, const char *key, char *out, size_t cap)
{
    const char *p = js, *e = js + len;
    if (cap) out[0] = 0;
    if (!cap) return 0;
    p = js_ws(p, e);
    if (p >= e || *p != '{') return 0;
    p++;
    size_t kl = strlen(key);
    for (;;) {
        p = js_ws(p, e);
        if (p >= e || *p != '"') return 0;
        const char *ks = p + 1, *ke = js_str_end(p, e);
        if (!ke) return 0;
        int match = (size_t)(ke - 1 - ks) == kl && !memcmp(ks, key, kl);
        p = js_ws(ke, e);
        if (p >= e || *p != ':') return 0;
        p = js_ws(p + 1, e);
        const char *ve = js_skip(p, e);
        if (!ve) return 0;
        if (match) {
            if (*p == '"') js_copy(p + 1, ve - 1, out, cap);
            else if (*p == '[') {
                const char *q = js_ws(p + 1, ve);
                const char *qe = q < ve && *q == '"' ? js_str_end(q, ve) : NULL;
                if (qe) js_copy(q + 1, qe - 1, out, cap);
            } else if (*p != '{') {
                size_t n = (size_t)(ve - p) < cap - 1 ? (size_t)(ve - p) : cap - 1;
                memcpy(out, p, n);
                out[n] = 0;
            }
            return 1;
        }
        p = js_ws(ve, e);
        if (p >= e || *p != ',') return 0;
        p++;
    }
}

/* 0 delivered; -1 refused (err); -2 try again (*retry_ms: delay, 0 = default); -3 too large */
int notify_result(int service, const notify_resp_t *r, int *retry_ms, char *err, size_t cap)
{
    char v[160] = "";
    const char *b = r->body ? r->body : "";
    size_t bl = r->body ? r->body_len : 0;
    int ok = r->status >= 200 && r->status < 300;
    *retry_ms = 0;
    switch (service) {
    case NOTIFY_TELEGRAM:
        if (ok && notify_json_get(b, bl, "ok", v, sizeof v) == 1 && !strcmp(v, "true")) return 0;
        notify_json_get(b, bl, "description", v, sizeof v);
        break;
    case NOTIFY_PUSHOVER:
        if (ok && notify_json_get(b, bl, "status", v, sizeof v) == 1 && !strcmp(v, "1")) return 0;
        notify_json_get(b, bl, "errors", v, sizeof v);
        break;
    case NOTIFY_DISCORD:
        if (ok) return 0;
        if (r->status == 429 && notify_json_get(b, bl, "retry_after", v, sizeof v) == 1) {
            double s = strtod(v, NULL);
            if (s > 0 && s < 3600) *retry_ms = (int)(s * 1000) + 100;
        }
        notify_json_get(b, bl, "message", v, sizeof v);
        break;
    case NOTIFY_GOTIFY:
        if (ok) return 0;
        if (notify_json_get(b, bl, "errorDescription", v, sizeof v) != 1) notify_json_get(b, bl, "error", v, sizeof v);
        break;
    default:
        if (ok) return 0;
        notify_json_get(b, bl, "error", v, sizeof v);
    }
    clean(v);
    if (v[0]) snprintf(err, cap, "HTTP %d: %s", r->status, v);
    else snprintf(err, cap, "HTTP %d", r->status);
    if (r->status == 413) return -3;
    if (r->status == 429 || r->status >= 500) {
        if (!*retry_ms) *retry_ms = r->retry_after_ms;
        return -2;
    }
    return -1;
}

/* ---- HTTP(S) client ----------------------------------------------------------------- */
typedef struct { const void *p; size_t n; } part_t;

static int tcp_connect(const char *host, int port, char *err, size_t cap)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(err, cap, "cannot resolve %s", host); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { freeaddrinfo(res); snprintf(err, cap, "socket: %s", strerror(errno)); return -1; }
    int rc = connect(fd, res->ai_addr, res->ai_addrlen), e = rc ? errno : 0;
    freeaddrinfo(res);
    if (rc && e == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        int pr = poll(&p, 1, CONNECT_MS);
        socklen_t sl = sizeof e;
        if (pr <= 0) e = ETIMEDOUT;
        else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &sl) != 0) e = errno;
    }
    if (e) {
        snprintf(err, cap, "cannot connect to %s:%d (%s)", host, port, strerror(e));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static void set_timeouts(int fd)
{
    struct timeval s = { 15, 0 }, r = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof s);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof r);
}

static int send_dl(conn_t *c, const void *p, size_t n, int64_t dl)
{
    const uint8_t *b = p;
    while (n) {
        if (mono_ms() > dl) return -1;
        size_t k = n > 8192 ? 8192 : n;
        if (conn_send_all(c, b, k) != 0) return -1;
        b += k; n -= k;
    }
    return 0;
}

/* One request on a new connection; buf holds RESP_CAP + 1 bytes.
 * 0: response parsed into r; -1: failed before the request was sent (may be
 * retried); -2: failed after it (no retry: the server may have it). */
static int http_request(const char *method, const char *url, const char *hdrs, const char *ctype,
                        const part_t *pt, int np, char *buf, notify_resp_t *r, char *err, size_t cap)
{
    url_t u;
    if (url_parse(url, &u) != 0) { snprintf(err, cap, "invalid URL"); return -2; }
    int fd = tcp_connect(u.host, u.port, err, cap);
    if (fd < 0) return -1;
    conn_t *c;
    if (u.tls) {
        c = tls_client_connect(fd, u.host, 3, 0, NULL, TLS_MS, err, cap);
        if (!c) return strstr(err, "certificate") ? -2 : -1;       /* not trusted: no point retrying */
    } else if (!(c = conn_plain(fd))) { close(fd); snprintf(err, cap, "out of memory"); return -1; }
    set_timeouts(fd);

    size_t total = 0;
    for (int i = 0; i < np; i++) total += pt[i].n;
    char hp[140], head[2600];
    if (u.port == (u.tls ? 443 : 80)) snprintf(hp, sizeof hp, "%s", u.host);
    else snprintf(hp, sizeof hp, "%s:%d", u.host, u.port);
    int hn = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ak_rtsp-notify/1\r\nAccept: */*\r\n"
                      "Connection: close\r\n%s%s%s%sContent-Length: %lu\r\n\r\n", method, u.path, hp, hdrs ? hdrs : "",
                      ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "", (unsigned long)total);
    if (hn < 0 || (size_t)hn >= sizeof head) { conn_close(c); snprintf(err, cap, "request too long"); return -2; }
    int64_t dl = mono_ms() + 30000 + (int64_t)(total / 16);       /* >= 16 KB/s after 30 s */
    int rc = send_dl(c, head, (size_t)hn, dl);
    for (int i = 0; !rc && i < np; i++) if (pt[i].n) rc = send_dl(c, pt[i].p, pt[i].n, dl);
    if (rc) { conn_close(c); snprintf(err, cap, "%s: sending failed or too slow", u.host); return -1; }

    size_t len = 0;
    int64_t rdl = mono_ms() + RESP_MS;
    int slow = 0;
    while (len < RESP_CAP) {
        int64_t left = rdl - mono_ms();
        if (left <= 0) { slow = 1; break; }
        if (!conn_readable(c, left > 500 ? 500 : (int)left)) continue;
        ssize_t n = conn_recv(c, buf + len, RESP_CAP - len);
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = 0;
        if (resp_complete(buf, len)) break;
    }
    conn_close(c);
    buf[len] = 0;
    if (notify_parse_response(buf, len, r) != 0) {
        snprintf(err, cap, "%s: %s", u.host, slow ? "no answer within 30 s" : len ? "invalid HTTP response" : "no response");
        return -2;
    }
    return 0;
}

/* ---- messages ------------------------------------------------------------------------ */
typedef struct { char *b; size_t n, cap; int err; } sb_t;
static void sb_printf(sb_t *s, const char *fmt, ...)
{
    if (s->err) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->b + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= s->cap - s->n) { s->err = 1; return; }
    s->n += (size_t)n;
}
static void json_esc(sb_t *s, const char *in)
{
    for (; *in; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '"' || ch == '\\') sb_printf(s, "\\%c", ch);
        else if (ch == '\n') sb_printf(s, "\\n");
        else if (ch < 0x20) sb_printf(s, "\\u%04x", ch);
        else sb_printf(s, "%c", ch);
    }
}
/* Header value: as is when printable ASCII, else an RFC 2047 encoded word (ntfy decodes it). */
static void hdr_enc(const char *in, char *out, size_t cap)
{
    int plain = 1;
    for (const char *p = in; *p; p++) if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7f) plain = 0;
    if (plain) { snprintf(out, cap, "%s", in); return; }
    char b64[600];
    size_t ol = 0;
    if (cap < 16 || mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &ol, (const unsigned char *)in, strlen(in)) != 0) { snprintf(out, cap, "?"); return; }
    b64[ol] = 0;
    snprintf(out, cap, "=?UTF-8?B?%s?=", b64);
}
static void mp_field(sb_t *s, const char *bnd, const char *name, const char *val)
{
    sb_printf(s, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", bnd, name, val);
}
static void mp_file(sb_t *s, const char *bnd, const char *name, const char *fname, const char *ctype)
{
    sb_printf(s, "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n", bnd, name, fname, ctype);
}

typedef struct {
    const cfg_t *c;
    const char *title, *msg, *tag;
    const uint8_t *data;     /* attachment, NULL = text only */
    size_t n;
    int clip;                /* data is the MP4 clip */
    int quiet;               /* follow-up message: no sound / low priority */
} msg_t;

static void api_base(const char *url, const char *def, char *out, size_t cap)
{
    snprintf(out, cap, "%s", url[0] ? url : def);
}

/* One attempt. 0 ok; -1 refused; -2 retry (*retry_ms); -3 too large. */
static int send_once(const msg_t *m, char *buf, int *retry_ms, char *err, size_t cap)
{
    const cfg_t *c = m->c;
    char url[768], hdrs[1600] = "", ctype[96] = "", pre[3072], bnd[40], post_[64] = "", base[260];
    const char *method = "POST";
    sb_t H = { hdrs, 0, sizeof hdrs, 0 }, P = { pre, 0, sizeof pre, 0 };
    part_t pt[3];
    int np = 0;
    const char *fname = m->clip ? "clip.mp4" : "snapshot.jpg", *fct = m->clip ? "video/mp4" : "image/jpeg";
    char tm[420];
    snprintf(tm, sizeof tm, "%s\n%s", m->title, m->msg);
    snprintf(bnd, sizeof bnd, "akrtsp%08x%08x", (unsigned)mono_ms(), (unsigned)rand());
    *retry_ms = 0;

    switch (c->sv) {
    case NOTIFY_NTFY: {
        char e1[400], e2[700];
        snprintf(url, sizeof url, "%s", c->url);
        hdr_enc(m->title, e1, sizeof e1);
        sb_printf(&H, "Title: %s\r\nTags: %s\r\n", e1, m->tag);
        if (m->quiet) sb_printf(&H, "Priority: 2\r\n");
        if (c->token[0]) {
            if (strchr(c->token, ':')) {               /* user:password */
                char b64[240];
                size_t ol = 0;
                if (mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &ol, (const unsigned char *)c->token, strlen(c->token)) != 0) ol = 0;
                b64[ol] = 0;
                sb_printf(&H, "Authorization: Basic %s\r\n", b64);
            } else sb_printf(&H, "Authorization: Bearer %s\r\n", c->token);
        }
        if (m->data) {
            hdr_enc(m->msg, e2, sizeof e2);
            sb_printf(&H, "Filename: %s\r\nMessage: %s\r\n", fname, e2);
            snprintf(ctype, sizeof ctype, "%s", fct);
            pt[np++] = (part_t){ m->data, m->n };
            method = "PUT";
        } else {
            snprintf(ctype, sizeof ctype, "text/plain; charset=utf-8");
            pt[np++] = (part_t){ m->msg, strlen(m->msg) };
        }
        break;
    }
    case NOTIFY_TELEGRAM:
        api_base(c->url, "https://api.telegram.org", base, sizeof base);
        snprintf(url, sizeof url, "%s/bot%s/%s", base, c->token, !m->data ? "sendMessage" : m->clip ? "sendVideo" : "sendPhoto");
        mp_field(&P, bnd, "chat_id", c->user);
        if (m->quiet) mp_field(&P, bnd, "disable_notification", "true");
        if (m->data) {
            mp_field(&P, bnd, "caption", tm);
            if (m->clip) mp_field(&P, bnd, "supports_streaming", "true");
            mp_file(&P, bnd, m->clip ? "video" : "photo", fname, fct);
        } else mp_field(&P, bnd, "text", tm);
        break;
    case NOTIFY_PUSHOVER:
        api_base(c->url, "https://api.pushover.net", base, sizeof base);
        snprintf(url, sizeof url, "%s/1/messages.json", base);
        mp_field(&P, bnd, "token", c->token);
        mp_field(&P, bnd, "user", c->user);
        mp_field(&P, bnd, "title", m->title);
        mp_field(&P, bnd, "message", m->msg);
        if (m->data) mp_file(&P, bnd, "attachment", fname, fct);
        break;
    case NOTIFY_DISCORD: {
        char pj[1200];
        sb_t J = { pj, 0, sizeof pj, 0 };
        if (!c->token[0]) snprintf(url, sizeof url, "%s%s", c->url, strchr(c->url, '?') ? "" : "?wait=true");   /* webhook */
        else {
            api_base(c->url, "https://discord.com", base, sizeof base);
            snprintf(url, sizeof url, "%s/api/v10/channels/%s/messages", base, c->user);
            sb_printf(&H, "Authorization: Bot %s\r\n", c->token);
        }
        sb_printf(&J, "{\"content\":\"**");
        json_esc(&J, m->title);
        sb_printf(&J, "**\\n");
        json_esc(&J, m->msg);
        sb_printf(&J, "\"");
        if (!c->token[0]) { sb_printf(&J, ",\"username\":\""); json_esc(&J, sys_device_name()); sb_printf(&J, "\""); }
        if (m->quiet) sb_printf(&J, ",\"flags\":4096");                  /* SUPPRESS_NOTIFICATIONS */
        sb_printf(&J, ",\"allowed_mentions\":{\"parse\":[]}}");
        if (J.err) { snprintf(err, cap, "message too long"); return -1; }
        mp_field(&P, bnd, "payload_json", pj);
        if (m->data) mp_file(&P, bnd, "files[0]", fname, fct);
        break;
    }
    case NOTIFY_GOTIFY: {
        char js[1400];
        sb_t J = { js, 0, sizeof js, 0 };
        snprintf(url, sizeof url, "%s/message", c->url);
        sb_printf(&H, "X-Gotify-Key: %s\r\n", c->token);
        sb_printf(&J, "{\"title\":\"");
        json_esc(&J, m->title);
        sb_printf(&J, "\",\"message\":\"");
        json_esc(&J, m->msg);
        if (c->img[0]) { sb_printf(&J, "\\n\\n![snapshot]("); json_esc(&J, c->img); sb_printf(&J, ")"); }
        sb_printf(&J, "\",\"priority\":%d", c->prio);
        if (c->img[0]) sb_printf(&J, ",\"extras\":{\"client::display\":{\"contentType\":\"text/markdown\"}}");
        sb_printf(&J, "}");
        if (J.err) { snprintf(err, cap, "message too long"); return -1; }
        snprintf(ctype, sizeof ctype, "application/json");
        /* the JSON lives in this frame: send now */
        pt[np++] = (part_t){ js, J.n };
        if (H.err) { snprintf(err, cap, "request too long"); return -1; }
        notify_resp_t r;
        int rc = http_request(method, url, hdrs, ctype, pt, np, buf, &r, err, cap);
        if (rc) return rc == -1 ? -2 : -1;
        return notify_result(c->sv, &r, retry_ms, err, cap);
    }
    }
    if (P.n) {                                         /* multipart: fields [+ file head], data, end */
        snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", bnd);
        snprintf(post_, sizeof post_, "%s--%s--\r\n", m->data ? "\r\n" : "", bnd);
        pt[np++] = (part_t){ pre, P.n };
        if (m->data) pt[np++] = (part_t){ m->data, m->n };
        pt[np++] = (part_t){ post_, strlen(post_) };
    }
    if (H.err || P.err) { snprintf(err, cap, "request too long"); return -1; }
    notify_resp_t r;
    int rc = http_request(method, url, hdrs, ctype[0] ? ctype : NULL, pt, np, buf, &r, err, cap);
    if (rc) return rc == -1 ? -2 : -1;
    return notify_result(c->sv, &r, retry_ms, err, cap);
}

/* Up to TRIES attempts. Same return values as send_once. */
static int deliver(const msg_t *m, char *err, size_t cap)
{
    char *buf = malloc(RESP_CAP + 1);
    if (!buf) { snprintf(err, cap, "out of memory"); return -1; }
    int rc = -1;
    for (int a = 0; a < TRIES; a++) {
        int retry_ms = 0;
        rc = send_once(m, buf, &retry_ms, err, cap);
        if (rc != -2 || a + 1 == TRIES) break;
        int d = retry_ms > 0 ? retry_ms : a == 0 ? 3000 : 10000;
        if (d > RETRY_MAX_MS) { nlog("%s: %s; server asks to wait %d s: giving up", k_sv[m->c->sv], err, d / 1000); break; }
        nlog("%s: %s; retry %d/%d in %d.%d s", k_sv[m->c->sv], err, a + 1, TRIES - 1, d / 1000, d % 1000 / 100);
        int64_t until = mono_ms() + d;
        while (mono_ms() < until) usleep(100000);
    }
    free(buf);
    return rc;
}

/* ---- clip --------------------------------------------------------------------------- */
/* MP4 (fragmented, H.264 only) of secs seconds from the live hub, starting
 * at the keyframe nearest before the event if it is still in the hub's
 * buffer, else at the next one. The caller has subscribed. */
static uint8_t *build_clip(int64_t ev_mono, int secs, size_t *outlen, int *dur_ms, int *truncated, char *err, size_t cap)
{
    uint8_t *clip = malloc(NOTIFY_CLIP_MAX), *frame = malloc(LIVE_MAX_FRAME);
    live_item_t *it = malloc(sizeof *it);
    FILE *f = NULL;
    fmp4_t *m = NULL;
    *truncated = 0; *dur_ms = 0; *outlen = 0;
    if (!clip || !frame || !it || !(f = fmemopen(clip, NOTIFY_CLIP_MAX, "w+"))) { snprintf(err, cap, "out of memory"); goto fail; }
    setvbuf(f, NULL, _IONBF, 0);
    live_cursor_t cur = { 0, 0 };
    int64_t now = mono_ms(), back = now - ev_mono + 1000;
    if (back < 0) back = 0;
    if (back > 30000) back = 30000;
    if (live_rewind(&cur, (uint32_t)back) != 0) venc_request_idr();
    int64_t dl = now + (int64_t)secs * 1000 + 8000;
    uint64_t t0 = 0, tlast = 0;
    size_t pending = 0;
    int since = 0, nfr = 0;
    while (mono_ms() < dl) {
        if (live_next(&cur, frame, LIVE_MAX_FRAME, it, 500) != LIVE_VIDEO) continue;
        if (!m) {
            if (!it->key || !(m = fmp4_open(f, frame, it->len, 0))) continue;
            t0 = it->ts;
        }
        if (it->ts < t0) continue;
        if (it->ts - t0 >= (uint64_t)secs * 1000) break;
        long pos = ftell(f);
        if (pos < 0 || (size_t)pos + pending + it->len + 32 * (size_t)(since + 1) + 4096 > NOTIFY_CLIP_MAX) { *truncated = 1; break; }
        if ((it->key && pending) || pending > 512 * 1024) {
            if (fmp4_flush(m) != 0) { snprintf(err, cap, "clip buffer full"); goto fail; }
            pending = 0; since = 0;
        }
        if (fmp4_video(m, frame, it->len, it->key, it->ts) != 0) { snprintf(err, cap, "out of memory"); goto fail; }
        pending += it->len; since++; nfr++;
        tlast = it->ts;
    }
    if (!m || !nfr) { snprintf(err, cap, "no video from the camera"); goto fail; }
    fmp4_close(m);
    m = NULL;
    long end = ftell(f);
    fclose(f);
    f = NULL;
    if (end <= 0) { snprintf(err, cap, "clip buffer full"); goto fail; }
    free(frame);
    free(it);
    *dur_ms = (int)(tlast - t0) + (nfr > 1 ? (int)((tlast - t0) / (uint64_t)(nfr - 1)) : 66);
    *outlen = (size_t)end;
    uint8_t *shr = realloc(clip, (size_t)end);           /* give back the unused part of the cap */
    return shr ? shr : clip;
fail:
    if (m) fmp4_close(m);
    if (f) fclose(f);
    free(clip); free(frame); free(it);
    return NULL;
}

/* ---- sender ------------------------------------------------------------------------- */
static void compose(const cfg_t *c, const job_t *j, char *title, size_t tc, char *msg, size_t mc, const char **tag)
{
    char ts[32];
    struct tm tm;
    localtime_r(&j->t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    const char *name = c->title[0] ? c->title : sys_device_name();
    if (j->summary) {
        snprintf(title, tc, "%s: notifications paused", name);
        snprintf(msg, mc, "%d notification%s suppressed (limit %d per %d min)", j->summary, j->summary == 1 ? "" : "s", s_max, s_win);
        *tag = "hourglass";
    } else if (j->test) {
        snprintf(title, tc, "%s: test", name);
        snprintf(msg, mc, "Test notification from %s at %s", sys_device_name(), ts);
        *tag = "white_check_mark";
    } else if (!strcmp(j->kind, "sound")) {
        snprintf(title, tc, "%s: sound", name);
        snprintf(msg, mc, "Sound detected (%d dBFS) at %s", j->level, ts);
        *tag = "loud_sound";
    } else if (!strcmp(j->kind, "tamper")) {
        snprintf(title, tc, "%s: tamper alert", name);
        snprintf(msg, mc, "%s at %s", j->level == 2 ? "Camera moved" : "Lens covered", ts);
        *tag = "warning";
    } else if (!strcmp(j->kind, "line")) {                 /* events_line: level 1 = toward the left, 2 = right */
        snprintf(title, tc, "%s: line crossed", name);
        snprintf(msg, mc, "Line crossed%s at %s", j->level == 1 ? " (toward the left)" : j->level == 2 ? " (toward the right)" : "", ts);
        *tag = "arrow_right";
    } else if (!strcmp(j->kind, "loiter")) {               /* events_loiter: level = zone */
        snprintf(title, tc, "%s: loitering", name);
        snprintf(msg, mc, "Someone stayed in zone %d at %s", j->level, ts);
        *tag = "hourglass_flowing_sand";
    } else {
        snprintf(title, tc, "%s: motion", name);
        snprintf(msg, mc, "Motion detected at %s", ts);
        *tag = "rotating_light";
    }
}

static void send_clip(const cfg_t *c, const job_t *j, const char *title, const char *msg)
{
    char err[200] = "", t2[220];
    size_t n = 0;
    int dur = 0, trunc = 0;
    int64_t t_start = mono_ms();
    uint8_t *clip = build_clip(j->mono, c->clip_s, &n, &dur, &trunc, err, sizeof err);
    if (!clip) {
        nlog("clip: %s", err);
        pthread_mutex_lock(&g_st); g_clip_failed++; snprintf(g_clip_note, sizeof g_clip_note, "%s", err); pthread_mutex_unlock(&g_st);
        return;
    }
    pthread_mutex_lock(&g_st);
    if (g_clip_limit_gen != g_gen) { g_clip_limit = 0; g_clip_limit_gen = g_gen; }
    size_t limit = g_clip_limit;
    pthread_mutex_unlock(&g_st);
    if (c->sv == NOTIFY_DISCORD && (!limit || limit > DISCORD_MAX)) limit = DISCORD_MAX;
    char note[160];
    int rc = 0;
    if (limit && n >= limit) {
        rc = -3;
        snprintf(err, sizeof err, "larger than the server accepts");
    } else {
        snprintf(t2, sizeof t2, "%s (video)", title);
        msg_t mm = { c, t2, msg, "movie_camera", clip, n, 1, 1 };
        rc = deliver(&mm, err, sizeof err);
    }
    free(clip);
    if (rc == 0) snprintf(note, sizeof note, "%lu KB, %d.%d s%s, built in %d ms", (unsigned long)(n / 1024), dur / 1000, dur % 1000 / 100,
                          trunc ? " (size cap reached)" : "", (int)(mono_ms() - t_start));
    else if (rc == -3) snprintf(note, sizeof note, "clip of %lu KB refused as too large (%s); the picture was sent", (unsigned long)(n / 1024), err);
    else snprintf(note, sizeof note, "clip not sent: %s", err);
    nlog("clip: %s", note);
    pthread_mutex_lock(&g_st);
    if (rc == 0) { g_clips++; g_last_clip_bytes = n; g_last_clip_ms = dur; }
    else g_clip_failed++;
    /* refused as too large: clips of 3/4 of that size or more (sizes vary from
     * event to event) are not uploaded again until the settings change */
    if (rc == -3 && (!g_clip_limit || n - n / 4 < g_clip_limit)) g_clip_limit = n - n / 4;
    snprintf(g_clip_note, sizeof g_clip_note, "%s", note);
    clean(g_clip_note);
    pthread_mutex_unlock(&g_st);
}

static void process(const job_t *j)
{
    cfg_t c;
    cfg_snapshot(&c);
    char err[200] = "", title[200], msg[300];
    const char *tag = "";
    if (cfg_check(&c, err, sizeof err) != 0) {
        nlog("%s not sent: %s", j->kind, err);
        pthread_mutex_lock(&g_st); g_failed++; pthread_mutex_unlock(&g_st);
        set_state("error", err);
        return;
    }
    compose(&c, j, title, sizeof title, msg, sizeof msg, &tag);
    set_state("sending", NULL);
    int att = !j->summary && c.snap && c.sv != NOTIFY_GOTIFY;
    int want_clip = !j->summary && c.clip && (c.sv == NOTIFY_NTFY || c.sv == NOTIFY_TELEGRAM || c.sv == NOTIFY_DISCORD);
    if (want_clip) live_subscribe();                   /* the hub keeps frames from now on */
    uint8_t *jpg = NULL;
    int jn = -1;
    if (att) {
        jpg = malloc(JPG_MAX);
        if (jpg) jn = snapshot_jpeg(jpg, JPG_MAX, 0);
        if (jn <= 0) nlog("no picture available: sending text only");
    }
    msg_t mm = { &c, title, msg, tag, jn > 0 ? jpg : NULL, jn > 0 ? (size_t)jn : 0, 0, 0 };
    int64_t t0 = mono_ms();
    int rc = deliver(&mm, err, sizeof err);
    if (rc == -3 && mm.data) {
        nlog("%s: picture refused as too large, sending text", k_sv[c.sv]);
        mm.data = NULL; mm.n = 0;
        rc = deliver(&mm, err, sizeof err);
    }
    free(jpg);
    if (rc == 0) {
        nlog("%s sent to %s (%s%s, %d ms)", j->summary ? "summary" : j->kind, k_sv[c.sv], mm.data ? "picture" : "text",
             want_clip ? ", clip follows" : "", (int)(mono_ms() - t0));
        pthread_mutex_lock(&g_st); g_sent++; g_last_ok = time(NULL); pthread_mutex_unlock(&g_st);
        set_state("ok", NULL);
        if (want_clip) send_clip(&c, j, title, msg);
    } else {
        nlog("%s to %s failed: %s", j->summary ? "summary" : j->kind, k_sv[c.sv], err);
        pthread_mutex_lock(&g_st); g_failed++; pthread_mutex_unlock(&g_st);
        set_state("error", err);
    }
    if (want_clip) live_unsubscribe();
}

static void *notify_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_ql);
        while (!g_qn) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_qc, &g_ql, &ts);
            win_tick_locked(mono_ms());
        }
        job_t j = g_q[g_qh];
        g_qh = (g_qh + 1) % NQ;
        g_qn--;
        g_busy = 1;
        pthread_mutex_unlock(&g_ql);

        process(&j);

        pthread_mutex_lock(&g_ql);
        g_busy = 0;
        if (j.test) g_test_pending = 0;
        pthread_mutex_unlock(&g_ql);
    }
    return NULL;
}

void notify_init(void)
{
#define REGN(name, fn, fl) do { ctrl_param_t p_ = { name, fn##_get, fn##_setg, fl }; control_register(&p_); } while (0)
    REGN("notify.enabled", p_en, CTRL_PERSIST);
    REGN("notify.service", p_sv, CTRL_PERSIST);
    REGN("notify.url", p_url, CTRL_PERSIST | CTRL_SECRET);      /* ntfy topic / Discord webhook token live in it */
    REGN("notify.token", p_token, CTRL_PERSIST | CTRL_SECRET);
    REGN("notify.user", p_user, CTRL_PERSIST);
    REGN("notify.events", p_ev, CTRL_PERSIST);
    REGN("notify.cooldown", p_cool, CTRL_PERSIST);
    REGN("notify.max", p_max, CTRL_PERSIST);
    REGN("notify.window_min", p_win, CTRL_PERSIST);
    REGN("notify.snapshot", p_snap, CTRL_PERSIST);
    REGN("notify.clip", p_clip, CTRL_PERSIST);
    REGN("notify.clip_s", p_clip_s, CTRL_PERSIST);
    REGN("notify.title", p_title, CTRL_PERSIST);
    REGN("notify.priority", p_prio, CTRL_PERSIST);
    REGN("notify.image_url", p_img, CTRL_PERSIST);
#undef REGN
}

void notify_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 160 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, notify_thread, NULL);
    pthread_attr_destroy(&a);
}
