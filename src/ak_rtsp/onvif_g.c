#include "onvif_g.h"
#include "recidx.h"
#include "record.h"
#include "fmp4r.h"
#include "replay.h"
#include "system.h"
#include "control.h"
#include "monotime.h"
#include "ctrl_params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#include <sys/random.h>

static int s_enabled;                            /* onvif.replay, off by default */
CTRL_BOOL(p_replay, s_enabled)

void onvif_g_init(void) { CTRL_REG("onvif.replay", p_replay, CTRL_PERSIST); }
int  onvif_g_enabled(void) { return s_enabled; }

#define NS_TSE "xmlns:tse=\"http://www.onvif.org/ver10/search/wsdl\""
#define NS_TRC "xmlns:trc=\"http://www.onvif.org/ver10/recording/wsdl\""
#define NS_TRP "xmlns:trp=\"http://www.onvif.org/ver10/replay/wsdl\""
#define SB_MAX (2u * 1024 * 1024)                /* largest answer (GetRecordings of 400 days is ~0.5 MB) */
#define PAGE_DEFAULT 50
#define PAGE_MAX 100
#define MAX_SEARCHES 4
#define EV_MAX 1000                              /* results kept per event search */
#define EV_HARD 20000                            /* events looked at per search */
#define REC_MAX (RECIDX_MAX_DAYS + 64)

/* ---- output buffer --------------------------------------------------------------- */
typedef struct { char *p; size_t n, cap; int err; } sb_t;
static void sbf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sbf(sb_t *b, const char *fmt, ...)
{
    if (b->err) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int k = b->cap > b->n ? vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap) : -2;
        va_end(ap);
        if (k == -1) { b->err = 1; return; }
        if (k >= 0 && (size_t)k < b->cap - b->n) { b->n += (size_t)k; return; }
        size_t want = b->cap ? b->cap * 2 : 4096;
        while (k >= 0 && want < b->n + (size_t)k + 1) want *= 2;
        if (want > SB_MAX) { b->err = 1; return; }
        char *np = realloc(b->p, want);
        if (!np) { b->err = 1; return; }
        b->p = np;
        b->cap = want;
    }
}

/* XML-escaped copy (device name) */
static void esc(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        const char *e = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (e) { size_t l = strlen(e); memcpy(out + o, e, l); o += l; }
        else if ((unsigned char)*s >= 0x20) out[o++] = *s;
    }
    out[o] = 0;
}

static void fault(sb_t *b, const char *sub, const char *sub2, const char *reason)
{
    sbf(b, "<env:Fault xmlns:ter=\"http://www.onvif.org/ver10/error\"><env:Code><env:Value>env:Sender</env:Value>"
           "<env:Subcode><env:Value>ter:%s</env:Value>", sub);
    if (sub2) sbf(b, "<env:Subcode><env:Value>ter:%s</env:Value></env:Subcode>", sub2);
    sbf(b, "</env:Subcode></env:Code><env:Reason><env:Text xml:lang=\"en\">%s</env:Text></env:Reason></env:Fault>", reason);
}

/* ---- request parsing (bounded; the body is NUL-terminated by web.c) --------------------- */
/* next start tag <[prefix:]name ...> at or after from: pointer after its '>' (NULL none); *empty if <.../> */
static const char *xel(const char *from, const char *name, int *empty)
{
    size_t nl = strlen(name);
    for (const char *p = from ? strchr(from, '<') : NULL; p; p = strchr(p + 1, '<')) {
        const char *q = p + 1;
        if (*q == '/' || *q == '?' || *q == '!') continue;
        const char *end = q, *colon = q;
        while (*end && !isspace((unsigned char)*end) && *end != '>' && *end != '/') end++;
        for (const char *c = q; c < end; c++) if (*c == ':') colon = c + 1;
        if ((size_t)(end - colon) != nl || strncmp(colon, name, nl)) continue;
        const char *gt = strchr(end, '>');
        if (!gt) return NULL;
        if (empty) *empty = gt > end && gt[-1] == '/';
        return gt + 1;
    }
    return NULL;
}

/* trimmed text of an element found by xel; "" for <x/> */
static void xtext(const char *s, int empty, char *out, size_t cap)
{
    out[0] = 0;
    if (!s || empty) return;
    const char *e = strchr(s, '<');
    if (!e) return;
    while (s < e && isspace((unsigned char)*s)) s++;
    size_t n = (size_t)(e - s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static int get(const char *body, const char *name, char *out, size_t cap)
{
    int empty = 0;
    const char *s = xel(body, name, &empty);
    xtext(s, empty, out, cap);
    return s ? 0 : -1;
}

static long dur_s(const char *iso, long def)    /* xs:duration PT..H..M..S, capped */
{
    const char *p = strstr(iso, "PT");
    if (!p) return def;
    long total = 0, v = 0;
    for (p += 2; *p; p++) {
        if (isdigit((unsigned char)*p)) { if (v < 1000000) v = v * 10 + (*p - '0'); }
        else if (*p == 'H') { total += v * 3600; v = 0; }
        else if (*p == 'M') { total += v * 60; v = 0; }
        else if (*p == 'S' || *p == '.') { total += v; v = 0; if (*p == '.') break; }
        if (total > 100000000L) total = 100000000L;
    }
    return total > 0 ? total : def;
}

static int get_int(const char *body, const char *name, int def)
{
    char t[24];
    if (get(body, name, t, sizeof t) || !t[0]) return def;
    char *e;
    long v = strtol(t, &e, 10);
    if (e == t || v < 0) return def;
    return v > 1000000 ? 1000000 : (int)v;
}

/* ---- recordings ------------------------------------------------------------------------- */
typedef struct {
    int n, live, kinds[4];
    time_t from, until;
    char last[44];
} rsum_t;

/* token -> its files (into buf, RECIDX_MAX_FILES entries); count, 0 = no such recording */
static int rec_files(const char *token, char day[11], recfile_t *buf)
{
    char name[44];
    if (recidx_parse_token(token, day, name)) return 0;
    if (name[0]) return recidx_file(day, name, buf) == 0 ? 1 : 0;
    return recidx_files(day, buf, RECIDX_MAX_FILES);
}

static void summarize(const recfile_t *f, int n, rsum_t *s)
{
    memset(s, 0, sizeof *s);
    s->n = n;
    for (int i = 0; i < n; i++) {
        if (!i || f[i].start < s->from) s->from = f[i].start;
        if (!i || f[i].end > s->until) s->until = f[i].end;
        if (f[i].live) s->live = 1;
        if (f[i].kind < 4) s->kinds[f[i].kind]++;
    }
    if (n) snprintf(s->last, sizeof s->last, "%s", f[n - 1].name);
}

typedef struct { int ok, audio, w, h; double fps; int kbps; } media_t;

/* stream properties from one file's header and first fragment */
static void media_of(const char *day, const recfile_t *f, media_t *m)
{
    memset(m, 0, sizeof *m);
    char path[300];
    fmp4r_t r;
    if (record_path(day, f->name, path, sizeof path) || fmp4r_open(&r, path)) return;
    m->ok = 1;
    m->audio = r.aid && r.arate == 8000;
    m->w = r.width; m->h = r.height;
    if (fmp4r_next(&r) == 1) {
        int64_t d = 0;
        int nv = 0;
        for (size_t i = 0; i < r.ns; i++) if (!r.s[i].audio) { nv++; d += r.s[i].dur_us; }
        if (nv && d > 0) m->fps = (double)nv * 1000000.0 / (double)d;
    }
    int64_t dur = r.duration_us;
    if (dur > 1000000) m->kbps = (int)(f->size * 8 / (dur / 1000));
    fmp4r_close(&r);
}

/* the newest file of a recording that opens (a broken newest file must not hide the tracks) */
static void media_last(const char *day, const recfile_t *f, int n, media_t *m)
{
    memset(m, 0, sizeof *m);
    for (int i = n - 1, tries = 0; i >= 0 && tries < 8 && !m->ok; i--, tries++) media_of(day, &f[i], m);
}

static void source_xml(sb_t *b, const char *base)
{
    char name[100];
    esc(sys_device_name_cn(), name, sizeof name);
    sbf(b, "<tt:Source><tt:SourceId>%s/onvif/media_service#vs</tt:SourceId><tt:Name>%s</tt:Name><tt:Location></tt:Location>"
           "<tt:Description>SD card</tt:Description><tt:Address>%s/onvif/device_service</tt:Address></tt:Source>", base, name, base);
}

static void content_str(const rsum_t *s, char *out, size_t cap)
{
    snprintf(out, cap, "%d file(s): %d motion, %d sound, %d motion+sound, %d continuous",
             s->n, s->kinds[RK_MOTION], s->kinds[RK_SOUND], s->kinds[RK_EVENT], s->kinds[RK_CONT]);
}

static void track_info(sb_t *b, const char *el, const char *tok, const char *type, const char *desc, const rsum_t *s)
{
    char a[24], z[24];
    recidx_iso(s->from, a);
    recidx_iso(s->until, z);
    sbf(b, "<tt:%s><tt:TrackToken>%s</tt:TrackToken><tt:TrackType>%s</tt:TrackType><tt:Description>%s</tt:Description>"
           "<tt:DataFrom>%s</tt:DataFrom><tt:DataTo>%s</tt:DataTo></tt:%s>", el, tok, type, desc, a, z, el);
}

/* RecordingInformation content (no wrapper); -1 unknown token */
static int rec_info(sb_t *b, const char *token, const char *base, recfile_t *buf)
{
    char day[11];
    int n = rec_files(token, day, buf);
    if (n <= 0) return -1;
    rsum_t s;
    summarize(buf, n, &s);
    media_t m;
    media_last(day, buf, n, &m);
    char a[24], z[24], c[160];
    recidx_iso(s.from, a);
    recidx_iso(s.until, z);
    content_str(&s, c, sizeof c);
    sbf(b, "<tt:RecordingToken>%s</tt:RecordingToken>", token);
    source_xml(b, base);
    sbf(b, "<tt:EarliestRecording>%s</tt:EarliestRecording><tt:LatestRecording>%s</tt:LatestRecording><tt:Content>%s</tt:Content>", a, z, c);
    track_info(b, "Track", "VIDEO001", "Video", "H.264", &s);
    if (m.audio) track_info(b, "Track", "AUDIO001", "Audio", "G.711 u-law", &s);
    sbf(b, "<tt:RecordingStatus>%s</tt:RecordingStatus>", s.live ? "Recording" : "Stopped");
    return 0;
}

/* every recording (day with files), oldest first */
static int all_recordings(char (*toks)[RECIDX_TOKEN_MAX], int max, recfile_t *buf)
{
    char (*days)[11] = malloc(RECIDX_MAX_DAYS * sizeof *days);
    if (!days) return 0;
    int nd = recidx_days(days, RECIDX_MAX_DAYS), n = 0;
    for (int i = 0; i < nd && n < max; i++)
        if (recidx_files(days[i], buf, RECIDX_MAX_FILES) > 0) snprintf(toks[n++], RECIDX_TOKEN_MAX, "%s", days[i]);
    free(days);
    return n;
}

static int keep_days(void)
{
    char out[64] = "";
    int v = 0;
    control_exec("GET record.keep_days", out, sizeof out);
    if (sscanf(out, "%*[^=]=%d", &v) != 1) v = 0;
    return v;
}

/* ---- searches ----------------------------------------------------------------------------- */
enum { T_REC, T_TRACK, T_MOTION, T_SOUND, T_COUNT };
static const char *const k_topic[T_COUNT] = {
    "tns1:RecordingHistory/Recording/State", "tns1:RecordingHistory/Track/State",
    "tns1:RuleEngine/CellMotionDetector/Motion", "tns1:AudioAnalytics/Audio/DetectedSound",
};
typedef struct { int64_t t; uint32_t seq; uint16_t rec; uint8_t topic, val, ss; } gev_t;
typedef struct {
    int used, events;
    char token[24];
    int64_t expires, endpoint;       /* mono ms; endpoint: UTC ms */
    long keepalive;
    char (*recs)[RECIDX_TOKEN_MAX];
    int nrec;
    gev_t *ev;
    int nev, pos, backward;
} search_t;
static search_t g_s[MAX_SEARCHES];
static pthread_mutex_t g_slock = PTHREAD_MUTEX_INITIALIZER;

static void search_free(search_t *s)
{
    free(s->recs);
    free(s->ev);
    memset(s, 0, sizeof *s);
}

/* stores a finished search; returns its token in tok */
static void search_put(search_t *n, char tok[24])
{
    uint32_t r = 0;
    if (getrandom(&r, sizeof r, 0) != (ssize_t)sizeof r) r = (uint32_t)mono_us();
    snprintf(n->token, sizeof n->token, "search-%08x", r);
    snprintf(tok, 24, "%s", n->token);
    n->used = 1;
    n->expires = mono_ms() + n->keepalive * 1000;
    pthread_mutex_lock(&g_slock);
    search_t *slot = NULL;
    int64_t now = mono_ms();
    for (int i = 0; i < MAX_SEARCHES; i++) if (g_s[i].used && g_s[i].expires < now) search_free(&g_s[i]);
    for (int i = 0; i < MAX_SEARCHES && !slot; i++) if (!g_s[i].used) slot = &g_s[i];
    if (!slot) {                                  /* all busy: the one expiring first goes */
        slot = &g_s[0];
        for (int i = 1; i < MAX_SEARCHES; i++) if (g_s[i].expires < slot->expires) slot = &g_s[i];
        search_free(slot);
    }
    *slot = *n;
    pthread_mutex_unlock(&g_slock);
}

static search_t *search_get(const char *tok)    /* with g_slock held */
{
    int64_t now = mono_ms();
    for (int i = 0; i < MAX_SEARCHES; i++)
        if (g_s[i].used && !strcmp(g_s[i].token, tok)) {
            if (g_s[i].expires < now) { search_free(&g_s[i]); return NULL; }
            g_s[i].expires = now + g_s[i].keepalive * 1000;
            return &g_s[i];
        }
    return NULL;
}

/* scope: IncludedRecordings tokens (validated), count */
static int scope_tokens(const char *body, char (*toks)[RECIDX_TOKEN_MAX], int max)
{
    int n = 0, empty;
    const char *sc = xel(body, "Scope", &empty);
    if (!sc || empty) return 0;
    for (const char *q = sc; n < max && (q = xel(q, "IncludedRecordings", &empty));) {
        char t[RECIDX_TOKEN_MAX], d[11], nm[44];
        xtext(q, empty, t, sizeof t);
        if (!recidx_parse_token(t, d, nm)) {
            int dup = 0;
            for (int i = 0; i < n; i++) if (!strcmp(toks[i], t)) dup = 1;
            if (!dup) snprintf(toks[n++], RECIDX_TOKEN_MAX, "%s", t);
        }
    }
    return n;
}

/* topic filter: "a|b", each "tns1:X/Y" or a prefix ending in "//." or "/ *" (any below) */
static int topic_ok(const char *expr, int topic)
{
    if (!expr[0]) return 1;
    const char *t = k_topic[topic];
    char alt[128];
    for (const char *p = expr; *p;) {
        size_t l = strcspn(p, "|");
        size_t k = l < sizeof alt ? l : sizeof alt - 1;
        memcpy(alt, p, k);
        alt[k] = 0;
        char *a = alt;
        while (isspace((unsigned char)*a)) a++;
        size_t al = strlen(a);
        while (al && isspace((unsigned char)a[al - 1])) a[--al] = 0;
        while (al && (a[al - 1] == '.' || a[al - 1] == '*' || a[al - 1] == '/')) a[--al] = 0;
        const char *tn = strchr(t, ':') + 1, *an = strchr(a, ':') ? strchr(a, ':') + 1 : a;
        size_t anl = strlen(an);
        if (!anl || (!strncmp(tn, an, anl) && (tn[anl] == 0 || tn[anl] == '/'))) return 1;
        p += l;
        if (*p == '|') p++;
    }
    return 0;
}

typedef struct { search_t *s; int64_t lo, hi; const char *topics; int cap_hit; uint32_t seq; } evctx_t;
static void add_ev(evctx_t *c, int64_t t, int rec, int topic, int val, int ss)
{
    search_t *s = c->s;
    if (!ss && (t < c->lo || t > c->hi)) return;
    if (!topic_ok(c->topics, topic)) return;
    if (s->nev >= EV_HARD) { c->cap_hit = 1; return; }
    if (s->nev % 256 == 0) {
        gev_t *n = realloc(s->ev, (size_t)(s->nev + 256) * sizeof *n);
        if (!n) { c->cap_hit = 1; return; }
        s->ev = n;
    }
    gev_t *e = &s->ev[s->nev++];
    e->t = t; e->rec = (uint16_t)rec; e->topic = (uint8_t)topic; e->val = (uint8_t)val; e->ss = (uint8_t)ss; e->seq = c->seq++;
}

static int g_backward_sort;                       /* qsort has no context argument; under g_sortlock */
static pthread_mutex_t g_sortlock = PTHREAD_MUTEX_INITIALIZER;
static int cmp_ev(const void *a, const void *b)
{
    const gev_t *x = a, *y = b;
    if (x->ss != y->ss) return x->ss ? -1 : 1;    /* start state first */
    if (x->t != y->t) return (x->t < y->t) ^ g_backward_sort ? -1 : 1;
    return x->seq < y->seq ? -1 : 1;
}

static void day_of(int64_t ms, char out[11])
{
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    if (!gmtime_r(&t, &tm)) { snprintf(out, 11, "1970-01-01"); return; }
    snprintf(out, 11, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static void find_events(const char *body, sb_t *b, recfile_t *buf)
{
    char sp_s[48], ep_s[48] = "", inc[16] = "", ka[32] = "", topics[256] = "";
    if (get(body, "StartPoint", sp_s, sizeof sp_s)) { fault(b, "InvalidArgVal", NULL, "StartPoint missing"); return; }
    int64_t sp = recidx_parse_time_ms(sp_s), ep = -1;
    if (sp < 0) { fault(b, "InvalidArgVal", NULL, "bad StartPoint"); return; }
    if (!get(body, "EndPoint", ep_s, sizeof ep_s) && ep_s[0] && (ep = recidx_parse_time_ms(ep_s)) < 0) {
        fault(b, "InvalidArgVal", NULL, "bad EndPoint"); return;
    }
    get(body, "IncludeStartState", inc, sizeof inc);
    get(body, "KeepAliveTime", ka, sizeof ka);
    get(body, "TopicExpression", topics, sizeof topics);
    int max = get_int(body, "MaxMatches", EV_MAX);
    if (max <= 0 || max > EV_MAX) max = EV_MAX;
    int include_start = !strcmp(inc, "true") || !strcmp(inc, "1");

    search_t s;
    memset(&s, 0, sizeof s);
    s.events = 1;
    s.backward = ep >= 0 && ep < sp;
    s.keepalive = dur_s(ka, 60);
    if (s.keepalive > 600) s.keepalive = 600;
    s.recs = malloc(REC_MAX * sizeof *s.recs);
    if (!s.recs) { fault(b, "Action", NULL, "out of memory"); return; }
    evctx_t c = { &s, s.backward ? ep : sp, s.backward ? sp : (ep >= 0 ? ep : INT64_MAX), topics, 0, 0 };
    s.endpoint = ep >= 0 ? ep : sp;

    char (*toks)[RECIDX_TOKEN_MAX] = malloc(REC_MAX * sizeof *toks);
    int nt = toks ? scope_tokens(body, toks, 64) : 0;
    if (toks && !nt) {                            /* no scope: the days around the time window */
        char lo_d[11], hi_d[11];
        day_of(c.lo - 86400000, lo_d);
        if (c.hi == INT64_MAX) snprintf(hi_d, sizeof hi_d, "9999-12-31"); else day_of(c.hi + 86400000, hi_d);
        char (*days)[11] = malloc(RECIDX_MAX_DAYS * sizeof *days);
        int nd = days ? recidx_days(days, RECIDX_MAX_DAYS) : 0;
        for (int i = 0; i < nd && nt < REC_MAX; i++)
            if (strcmp(days[i], lo_d) >= 0 && strcmp(days[i], hi_d) <= 0) snprintf(toks[nt++], RECIDX_TOKEN_MAX, "%s", days[i]);
        free(days);
    }
    for (int i = 0; i < nt && !c.cap_hit; i++) {
        char day[11];
        int n = rec_files(toks[i], day, buf);
        if (n <= 0) continue;
        int rec = s.nrec++;
        snprintf(s.recs[rec], RECIDX_TOKEN_MAX, "%s", toks[i]);
        int covered = 0;
        for (int j = 0; j < n; j++) {
            const recfile_t *f = &buf[j];
            int64_t a = (int64_t)f->start * 1000, z = (int64_t)f->end * 1000;
            if (include_start && !covered && a <= sp && sp <= z) {
                covered = 1;
                add_ev(&c, sp, rec, T_REC, 1, 1);
                add_ev(&c, sp, rec, T_TRACK, 1, 1);
            }
            if (z < c.lo || a > c.hi) continue;
            int motion = f->kind == RK_MOTION || f->kind == RK_EVENT, sound = f->kind == RK_SOUND || f->kind == RK_EVENT;
            add_ev(&c, a, rec, T_REC, 1, 0);
            add_ev(&c, a, rec, T_TRACK, 1, 0);
            if (motion) add_ev(&c, a, rec, T_MOTION, 1, 0);
            if (sound) add_ev(&c, a, rec, T_SOUND, 1, 0);
            if (f->live) continue;                /* still recording: no end yet */
            if (motion) add_ev(&c, z, rec, T_MOTION, 0, 0);
            if (sound) add_ev(&c, z, rec, T_SOUND, 0, 0);
            add_ev(&c, z, rec, T_TRACK, 0, 0);
            add_ev(&c, z, rec, T_REC, 0, 0);
        }
    }
    free(toks);
    if (s.nev > 1) {
        pthread_mutex_lock(&g_sortlock);
        g_backward_sort = s.backward;
        qsort(s.ev, (size_t)s.nev, sizeof *s.ev, cmp_ev);
        pthread_mutex_unlock(&g_sortlock);
    }
    if (s.nev > max) s.nev = max;
    if (ep < 0 && s.nev) s.endpoint = s.ev[s.nev - 1].t;
    char tok[24];
    search_put(&s, tok);
    sbf(b, "<tse:FindEventsResponse " NS_TSE "><tse:SearchToken>%s</tse:SearchToken></tse:FindEventsResponse>", tok);
}

static void event_xml(sb_t *b, const gev_t *e, const char *rec)
{
    char t[24];
    recidx_iso((time_t)(e->t / 1000), t);
    static const char *const item[T_COUNT] = { "IsRecording", "IsDataPresent", "IsMotion", "IsSoundDetected" };
    sbf(b, "<tt:Result><tt:RecordingToken>%s</tt:RecordingToken><tt:TrackToken>%s</tt:TrackToken><tt:Time>%s</tt:Time>"
           "<tt:Event><wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">%s</wsnt:Topic>"
           "<wsnt:Message><tt:Message UtcTime=\"%s\" PropertyOperation=\"%s\"><tt:Source>",
        rec, e->topic == T_SOUND ? "AUDIO001" : "VIDEO001", t, k_topic[e->topic], t, e->ss ? "Initialized" : "Changed");
    if (e->topic == T_REC) sbf(b, "<tt:SimpleItem Name=\"RecordingToken\" Value=\"%s\"/>", rec);
    else if (e->topic == T_TRACK) sbf(b, "<tt:SimpleItem Name=\"RecordingToken\" Value=\"%s\"/><tt:SimpleItem Name=\"Track\" Value=\"VIDEO001\"/>", rec);
    else if (e->topic == T_MOTION)
        sbf(b, "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"vsc\"/><tt:SimpleItem Name=\"VideoAnalyticsConfigurationToken\" Value=\"vac\"/>"
               "<tt:SimpleItem Name=\"Rule\" Value=\"MotionRule\"/>");
    else sbf(b, "<tt:SimpleItem Name=\"AudioSourceConfigurationToken\" Value=\"asc\"/>");
    sbf(b, "</tt:Source><tt:Data><tt:SimpleItem Name=\"%s\" Value=\"%s\"/></tt:Data></tt:Message></wsnt:Message></tt:Event>"
           "<tt:StartStateEvent>%s</tt:StartStateEvent></tt:Result>", item[e->topic], e->val ? "true" : "false", e->ss ? "true" : "false");
}

/* GetRecordingSearchResults / GetEventSearchResults */
static void search_results(const char *body, sb_t *b, int events, const char *base, recfile_t *buf)
{
    char tok[32];
    get(body, "SearchToken", tok, sizeof tok);
    int max = get_int(body, "MaxResults", PAGE_DEFAULT);
    if (max <= 0 || max > PAGE_MAX) max = max <= 0 ? PAGE_DEFAULT : PAGE_MAX;
    /* copy this page out under the lock, render without it (file access) */
    char (*page)[RECIDX_TOKEN_MAX] = malloc((size_t)max * sizeof *page);
    gev_t *pev = malloc((size_t)max * sizeof *pev);
    if (!page || !pev) { free(page); free(pev); fault(b, "Action", NULL, "out of memory"); return; }
    int n = 0, done = 0;
    pthread_mutex_lock(&g_slock);
    search_t *s = search_get(tok);
    if (!s || s->events != events) {
        pthread_mutex_unlock(&g_slock);
        free(page); free(pev);
        fault(b, "InvalidArgVal", "InvalidToken", "unknown search token");
        return;
    }
    if (events) {
        for (; n < max && s->pos < s->nev; n++, s->pos++) {
            pev[n] = s->ev[s->pos];
            snprintf(page[n], RECIDX_TOKEN_MAX, "%s", s->recs[pev[n].rec]);
        }
        done = s->pos >= s->nev;
    } else {
        for (; n < max && s->pos < s->nrec; n++, s->pos++) snprintf(page[n], RECIDX_TOKEN_MAX, "%s", s->recs[s->pos]);
        done = s->pos >= s->nrec;
    }
    pthread_mutex_unlock(&g_slock);
    const char *st = done ? "Completed" : "Searching";
    if (events) {
        sbf(b, "<tse:GetEventSearchResultsResponse " NS_TSE "><tse:ResultList><tt:SearchState>%s</tt:SearchState>", st);
        for (int i = 0; i < n; i++) event_xml(b, &pev[i], page[i]);
        sbf(b, "</tse:ResultList></tse:GetEventSearchResultsResponse>");
    } else {
        sbf(b, "<tse:GetRecordingSearchResultsResponse " NS_TSE "><tse:ResultList><tt:SearchState>%s</tt:SearchState>", st);
        for (int i = 0; i < n; i++) {
            size_t mark = b->n;
            sbf(b, "<tt:RecordingInformation>");
            if (rec_info(b, page[i], base, buf)) { b->n = mark; if (b->p) b->p[mark] = 0; continue; }   /* deleted meanwhile */
            sbf(b, "</tt:RecordingInformation>");
        }
        sbf(b, "</tse:ResultList></tse:GetRecordingSearchResultsResponse>");
    }
    free(page);
    free(pev);
}

/* ---- media attributes --------------------------------------------------------------------- */
static void media_attributes(const char *body, sb_t *b, recfile_t *buf)
{
    char ts[48] = "";
    get(body, "Time", ts, sizeof ts);
    int64_t t = ts[0] ? recidx_parse_time_ms(ts) : -1;
    if (ts[0] && t < 0) { fault(b, "InvalidArgVal", NULL, "bad Time"); return; }
    sb_t items = { 0 };
    int empty;
    for (const char *q = body; (q = xel(q, "RecordingTokens", &empty));) {
        char tok[RECIDX_TOKEN_MAX], day[11];
        xtext(q, empty, tok, sizeof tok);
        int n = rec_files(tok, day, buf);
        if (n <= 0) { free(items.p); fault(b, "InvalidArgVal", "NoRecording", "unknown recording token"); return; }
        rsum_t s;
        summarize(buf, n, &s);
        int at = n - 1;                           /* the file at Time, else the newest */
        for (int i = 0; t >= 0 && i < n; i++) if ((int64_t)buf[i].start * 1000 <= t && t <= (int64_t)buf[i].end * 1000) at = i;
        media_t m;
        media_of(day, &buf[at], &m);
        if (!m.ok) media_last(day, buf, n, &m);
        char a[24], z[24];
        recidx_iso(s.from, a);
        recidx_iso(s.until, z);
        sbf(&items, "<tse:MediaAttributes><tt:RecordingToken>%s</tt:RecordingToken>", tok);
        if (m.ok) {
            sbf(&items, "<tt:TrackAttributes>");
            track_info(&items, "TrackInformation", "VIDEO001", "Video", "H.264", &s);
            sbf(&items, "<tt:VideoAttributes>");
            if (m.kbps > 0) sbf(&items, "<tt:Bitrate>%d</tt:Bitrate>", m.kbps);
            sbf(&items, "<tt:Width>%d</tt:Width><tt:Height>%d</tt:Height><tt:Encoding>H264</tt:Encoding><tt:Framerate>%.1f</tt:Framerate>"
                        "</tt:VideoAttributes></tt:TrackAttributes>", m.w, m.h, m.fps > 0 ? m.fps : 15.0);
            if (m.audio) {
                sbf(&items, "<tt:TrackAttributes>");
                track_info(&items, "TrackInformation", "AUDIO001", "Audio", "G.711 u-law", &s);
                sbf(&items, "<tt:AudioAttributes><tt:Bitrate>64</tt:Bitrate><tt:Encoding>G711</tt:Encoding><tt:Samplerate>8</tt:Samplerate>"
                            "</tt:AudioAttributes></tt:TrackAttributes>");
            }
        }
        sbf(&items, "<tt:From>%s</tt:From><tt:Until>%s</tt:Until></tse:MediaAttributes>", a, z);
    }
    if (items.err) { free(items.p); fault(b, "Action", NULL, "answer too large"); return; }
    sbf(b, "<tse:GetMediaAttributesResponse " NS_TSE ">%s</tse:GetMediaAttributesResponse>", items.p ? items.p : "");
    free(items.p);
}

/* ---- dispatch ------------------------------------------------------------------------------- */
static int is_search_action(const char *a)
{
    static const char *const k[] = {
        "GetRecordingSummary", "GetRecordingInformation", "GetMediaAttributes", "FindRecordings", "GetRecordingSearchResults",
        "FindEvents", "GetEventSearchResults", "GetSearchState", "EndSearch", "FindPTZPosition", "GetPTZPositionSearchResults",
        "FindMetadata", "GetMetadataSearchResults", NULL };
    for (int i = 0; k[i]; i++) if (!strcmp(a, k[i])) return 1;
    return 0;
}
static int is_recording_action(const char *a)
{
    static const char *const k[] = {
        "GetRecordings", "GetRecordingConfiguration", "GetTrackConfiguration", "GetRecordingJobs", "GetRecordingJobConfiguration",
        "GetRecordingJobState", "GetRecordingOptions", "CreateRecording", "DeleteRecording", "SetRecordingConfiguration",
        "CreateTrack", "DeleteTrack", "SetTrackConfiguration", "CreateRecordingJob", "DeleteRecordingJob",
        "SetRecordingJobConfiguration", "SetRecordingJobMode", "ExportRecordedData", "StopExportRecordedData",
        "GetExportRecordedDataState", NULL };
    for (int i = 0; k[i]; i++) if (!strcmp(a, k[i])) return 1;
    return 0;
}
static int is_replay_action(const char *a)
{
    return !strcmp(a, "GetReplayUri") || !strcmp(a, "GetReplayConfiguration") || !strcmp(a, "SetReplayConfiguration");
}

static void search_service(const char *action, const char *body, const char *base, sb_t *b, recfile_t *buf)
{
    if (!strcmp(action, "GetServiceCapabilities")) {
        sbf(b, "<tse:GetServiceCapabilitiesResponse " NS_TSE "><tse:Capabilities MetadataSearch=\"false\"/></tse:GetServiceCapabilitiesResponse>");
    } else if (!strcmp(action, "GetRecordingSummary")) {
        char (*toks)[RECIDX_TOKEN_MAX] = malloc(RECIDX_MAX_DAYS * sizeof *toks);
        int n = toks ? all_recordings(toks, RECIDX_MAX_DAYS, buf) : 0;
        time_t from = 0, until = 0;
        for (int i = 0; i < n; i++) {
            char day[11];
            int k = rec_files(toks[i], day, buf);
            rsum_t s;
            summarize(buf, k, &s);
            if (!k) continue;
            if (!from || s.from < from) from = s.from;
            if (s.until > until) until = s.until;
        }
        free(toks);
        if (!n) from = until = time(NULL);       /* nothing recorded: an empty span */
        char a[24], z[24];
        recidx_iso(from, a);
        recidx_iso(until, z);
        sbf(b, "<tse:GetRecordingSummaryResponse " NS_TSE "><tse:Summary><tt:DataFrom>%s</tt:DataFrom><tt:DataUntil>%s</tt:DataUntil>"
               "<tt:NumberRecordings>%d</tt:NumberRecordings></tse:Summary></tse:GetRecordingSummaryResponse>", a, z, n);
    } else if (!strcmp(action, "GetRecordingInformation")) {
        char tok[RECIDX_TOKEN_MAX];
        get(body, "RecordingToken", tok, sizeof tok);
        sb_t x = { 0 };
        if (rec_info(&x, tok, base, buf) || x.err) { free(x.p); fault(b, "InvalidArgVal", "NoRecording", "unknown recording token"); return; }
        sbf(b, "<tse:GetRecordingInformationResponse " NS_TSE "><tse:RecordingInformation>%s</tse:RecordingInformation>"
               "</tse:GetRecordingInformationResponse>", x.p);
        free(x.p);
    } else if (!strcmp(action, "GetMediaAttributes")) {
        media_attributes(body, b, buf);
    } else if (!strcmp(action, "FindRecordings")) {
        char ka[32] = "";
        get(body, "KeepAliveTime", ka, sizeof ka);
        search_t s;
        memset(&s, 0, sizeof s);
        s.keepalive = dur_s(ka, 60);
        if (s.keepalive > 600) s.keepalive = 600;
        s.recs = malloc(REC_MAX * sizeof *s.recs);
        if (!s.recs) { fault(b, "Action", NULL, "out of memory"); return; }
        s.nrec = scope_tokens(body, s.recs, 64);
        if (s.nrec) {                             /* only those that exist */
            int k = 0;
            for (int i = 0; i < s.nrec; i++) {
                char day[11];
                if (rec_files(s.recs[i], day, buf) > 0) memmove(s.recs[k++], s.recs[i], RECIDX_TOKEN_MAX);
            }
            s.nrec = k;
        } else {
            s.nrec = all_recordings(s.recs, RECIDX_MAX_DAYS, buf);
        }
        int max = get_int(body, "MaxMatches", 0);
        if (max > 0 && s.nrec > max) s.nrec = max;
        s.endpoint = (int64_t)time(NULL) * 1000;
        char tok[24];
        search_put(&s, tok);
        sbf(b, "<tse:FindRecordingsResponse " NS_TSE "><tse:SearchToken>%s</tse:SearchToken></tse:FindRecordingsResponse>", tok);
    } else if (!strcmp(action, "GetRecordingSearchResults")) {
        search_results(body, b, 0, base, buf);
    } else if (!strcmp(action, "FindEvents")) {
        find_events(body, b, buf);
    } else if (!strcmp(action, "GetEventSearchResults")) {
        search_results(body, b, 1, base, buf);
    } else if (!strcmp(action, "GetSearchState") || !strcmp(action, "EndSearch")) {
        char tok[32];
        get(body, "SearchToken", tok, sizeof tok);
        pthread_mutex_lock(&g_slock);
        search_t *s = search_get(tok);
        if (!s) { pthread_mutex_unlock(&g_slock); fault(b, "InvalidArgVal", "InvalidToken", "unknown search token"); return; }
        int done = s->pos >= (s->events ? s->nev : s->nrec);
        int64_t ep = s->endpoint;
        if (!strcmp(action, "EndSearch")) search_free(s);
        pthread_mutex_unlock(&g_slock);
        if (!strcmp(action, "EndSearch")) {
            char t[24];
            recidx_iso((time_t)(ep / 1000), t);
            sbf(b, "<tse:EndSearchResponse " NS_TSE "><tse:Endpoint>%s</tse:Endpoint></tse:EndSearchResponse>", t);
        } else {
            sbf(b, "<tse:GetSearchStateResponse " NS_TSE "><tse:State>%s</tse:State></tse:GetSearchStateResponse>", done ? "Completed" : "Searching");
        }
    } else {
        fault(b, "ActionNotSupported", NULL, "not supported (no PTZ or metadata search)");
    }
}

static void recording_config(sb_t *b, const char *base, const rsum_t *s)
{
    char c[160];
    content_str(s, c, sizeof c);
    source_xml(b, base);
    int kd = keep_days();
    sbf(b, "<tt:Content>%s</tt:Content><tt:MaximumRetentionTime>P%dD</tt:MaximumRetentionTime>", c, kd > 0 ? kd : 0);
}

static void recording_service(const char *action, const char *body, const char *base, sb_t *b, recfile_t *buf)
{
    if (!strcmp(action, "GetServiceCapabilities")) {
        sbf(b, "<trc:GetServiceCapabilitiesResponse " NS_TRC "><trc:Capabilities DynamicRecordings=\"false\" DynamicTracks=\"false\" "
               "Encoding=\"H264 G711\" MaxRecordings=\"%d\" MaxRecordingJobs=\"0\" Options=\"false\" MetadataRecording=\"false\"/>"
               "</trc:GetServiceCapabilitiesResponse>", RECIDX_MAX_DAYS);
    } else if (!strcmp(action, "GetRecordings")) {
        char (*toks)[RECIDX_TOKEN_MAX] = malloc(RECIDX_MAX_DAYS * sizeof *toks);
        int n = toks ? all_recordings(toks, RECIDX_MAX_DAYS, buf) : 0;
        sbf(b, "<trc:GetRecordingsResponse " NS_TRC ">");
        for (int i = 0; i < n; i++) {
            char day[11];
            int k = rec_files(toks[i], day, buf);
            if (k <= 0) continue;
            rsum_t s;
            summarize(buf, k, &s);
            media_t m;
            media_last(day, buf, k, &m);
            sbf(b, "<trc:RecordingItem><tt:RecordingToken>%s</tt:RecordingToken><tt:Configuration>", toks[i]);
            recording_config(b, base, &s);
            sbf(b, "</tt:Configuration><tt:Tracks><tt:Track><tt:TrackToken>VIDEO001</tt:TrackToken><tt:Configuration>"
                   "<tt:TrackType>Video</tt:TrackType><tt:Description>H.264</tt:Description></tt:Configuration></tt:Track>");
            if (m.audio)
                sbf(b, "<tt:Track><tt:TrackToken>AUDIO001</tt:TrackToken><tt:Configuration><tt:TrackType>Audio</tt:TrackType>"
                       "<tt:Description>G.711 u-law</tt:Description></tt:Configuration></tt:Track>");
            sbf(b, "</tt:Tracks></trc:RecordingItem>");
        }
        free(toks);
        sbf(b, "</trc:GetRecordingsResponse>");
    } else if (!strcmp(action, "GetRecordingConfiguration") || !strcmp(action, "GetTrackConfiguration")) {
        char tok[RECIDX_TOKEN_MAX], day[11], trk[24] = "";
        get(body, "RecordingToken", tok, sizeof tok);
        int n = rec_files(tok, day, buf);
        if (n <= 0) { fault(b, "InvalidArgVal", "NoRecording", "unknown recording token"); return; }
        rsum_t s;
        summarize(buf, n, &s);
        if (!strcmp(action, "GetRecordingConfiguration")) {
            sbf(b, "<trc:GetRecordingConfigurationResponse " NS_TRC "><trc:RecordingConfiguration>");
            recording_config(b, base, &s);
            sbf(b, "</trc:RecordingConfiguration></trc:GetRecordingConfigurationResponse>");
            return;
        }
        get(body, "TrackToken", trk, sizeof trk);
        media_t m;
        media_last(day, buf, n, &m);
        int video = !strcmp(trk, "VIDEO001"), audio = !strcmp(trk, "AUDIO001") && m.audio;
        if (!video && !audio) { fault(b, "InvalidArgVal", "NoTrack", "unknown track token"); return; }
        sbf(b, "<trc:GetTrackConfigurationResponse " NS_TRC "><trc:TrackConfiguration><tt:TrackType>%s</tt:TrackType>"
               "<tt:Description>%s</tt:Description></trc:TrackConfiguration></trc:GetTrackConfigurationResponse>",
            video ? "Video" : "Audio", video ? "H.264" : "G.711 u-law");
    } else if (!strcmp(action, "GetRecordingJobs")) {
        sbf(b, "<trc:GetRecordingJobsResponse " NS_TRC "/>");   /* the recorder runs on its own (record.mode) */
    } else {
        fault(b, "ActionNotSupported", NULL, "recordings are read-only over ONVIF (set record.mode on the camera)");
    }
}

static void replay_service(const char *action, const char *body, const char *host, sb_t *b, recfile_t *buf)
{
    if (!strcmp(action, "GetServiceCapabilities")) {
        sbf(b, "<trp:GetServiceCapabilitiesResponse " NS_TRP "><trp:Capabilities ReversePlayback=\"false\" SessionTimeoutRange=\"60 60\" "
               "RTP_RTSP_TCP=\"true\"/></trp:GetServiceCapabilitiesResponse>");
    } else if (!strcmp(action, "GetReplayUri")) {
        char tok[RECIDX_TOKEN_MAX], day[11];
        get(body, "RecordingToken", tok, sizeof tok);
        if (rec_files(tok, day, buf) <= 0) { fault(b, "InvalidArgVal", "NoRecording", "unknown recording token"); return; }
        sbf(b, "<trp:GetReplayUriResponse " NS_TRP "><trp:Uri>rtsp://%s:554/replay/%s</trp:Uri></trp:GetReplayUriResponse>", host, tok);
    } else if (!strcmp(action, "GetReplayConfiguration")) {
        sbf(b, "<trp:GetReplayConfigurationResponse " NS_TRP "><trp:Configuration><tt:SessionTimeout>PT60S</tt:SessionTimeout>"
               "</trp:Configuration></trp:GetReplayConfigurationResponse>");
    } else if (!strcmp(action, "SetReplayConfiguration")) {
        sbf(b, "<trp:SetReplayConfigurationResponse " NS_TRP "/>");      /* fixed 60 s, accepted as is */
    } else {
        fault(b, "ActionNotSupported", NULL, "action not supported");
    }
}

int onvif_g_handle(const char *path, const char *action, const char *body, const char *base, const char *host,
                   char **out, int *code)
{
    int svc = !strncmp(path, "/onvif/search_service", 21) ? 1 : !strncmp(path, "/onvif/recording_service", 24) ? 2 :
              !strncmp(path, "/onvif/replay_service", 21) ? 3 : 0;
    if (!svc) svc = is_search_action(action) ? 1 : is_recording_action(action) ? 2 : is_replay_action(action) ? 3 : 0;
    if (!svc) return 0;
    *out = NULL;
    *code = 200;
    sb_t b = { 0 };
    if (!s_enabled) {
        fault(&b, "ActionNotSupported", NULL, "recording search and replay are off (onvif.replay)");
    } else {
        recfile_t *buf = malloc(RECIDX_MAX_FILES * sizeof *buf);
        if (!buf) fault(&b, "Action", NULL, "out of memory");
        else if (svc == 1) search_service(action, body ? body : "", base, &b, buf);
        else if (svc == 2) recording_service(action, body ? body : "", base, &b, buf);
        else replay_service(action, body ? body : "", host, &b, buf);
        free(buf);
    }
    if (b.err || !b.p) {
        free(b.p);
        b.p = NULL; b.n = b.cap = 0; b.err = 0;
        fault(&b, "Action", NULL, "answer too large");
    }
    if (b.p && !strncmp(b.p, "<env:Fault", 10)) *code = 400;
    *out = b.p;
    return 1;
}

void onvif_g_caps(const char *base, char *out, size_t cap)
{
    if (!s_enabled) { if (cap) out[0] = 0; return; }
    snprintf(out, cap, "<tt:Extension><tt:Recording><tt:XAddr>%s/onvif/recording_service</tt:XAddr><tt:ReceiverSource>false</tt:ReceiverSource>"
                       "<tt:MediaProfileSource>false</tt:MediaProfileSource><tt:DynamicRecordings>false</tt:DynamicRecordings>"
                       "<tt:DynamicTracks>false</tt:DynamicTracks><tt:MaxStringLength>%d</tt:MaxStringLength></tt:Recording>"
                       "<tt:Search><tt:XAddr>%s/onvif/search_service</tt:XAddr><tt:MetadataSearch>false</tt:MetadataSearch></tt:Search>"
                       "<tt:Replay><tt:XAddr>%s/onvif/replay_service</tt:XAddr></tt:Replay></tt:Extension>",
             base, RECIDX_TOKEN_MAX - 1, base, base);
}

void onvif_g_services(const char *base, char *out, size_t cap)
{
    if (!s_enabled) { if (cap) out[0] = 0; return; }
    static const char *const svc[3][2] = {
        { "http://www.onvif.org/ver10/recording/wsdl", "recording_service" },
        { "http://www.onvif.org/ver10/search/wsdl", "search_service" },
        { "http://www.onvif.org/ver10/replay/wsdl", "replay_service" },
    };
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < 3 && o < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "<tds:Service><tds:Namespace>%s</tds:Namespace><tds:XAddr>%s/onvif/%s</tds:XAddr>"
                                                "<tds:Version><tt:Major>2</tt:Major><tt:Minor>40</tt:Minor></tds:Version></tds:Service>",
                              svc[i][0], base, svc[i][1]);
}

const char *onvif_g_scope(void) { return s_enabled ? " onvif://www.onvif.org/Profile/G" : ""; }
