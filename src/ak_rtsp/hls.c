/* HLS output of the main stream: see hls.h and docs/hls.md.
 *
 * Producer: one thread, subscribed to the live hub only while requests keep
 * coming (stops HLS_IDLE_S after the last one). It feeds the frames to the
 * fragmented-MP4 muxer (fmp4.c, video only) through a stdio cookie stream
 * that appends to the segment being built; a segment is cut at the first
 * keyframe after HLS_SEG_MIN_MS. Completed segments sit in a small RAM ring
 * bounded by count and bytes.
 *
 * Requests: answered from the connection threads without the web lock.
 * g_lock is only held to look things up or copy at most HLS_SEND_CHUNK bytes,
 * never while sending; the live hub's lock is never touched here (the
 * producer only calls live_next()). */
#define _GNU_SOURCE                     /* fopencookie */
#include "hls.h"
#include "fmp4.h"
#include "live.h"
#include "venc.h"
#include "monotime.h"
#include "ctrl_params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* ---- limits (memory budget: docs/hls.md) ---------------------------------- */
#define HLS_IDLE_S         30            /* producer stops this long after the last request */
#define HLS_LIST           4             /* segments listed in the playlist */
#define HLS_RING           5             /* segments kept: one more for clients holding an older playlist */
#define HLS_MIN_KEEP       3             /* kept even over the byte budget */
#define HLS_MEM_MAX        (4u << 20)    /* completed segments, bytes */
#define HLS_SEG_MIN_MS     1000          /* cut at the first keyframe after this */
#define HLS_SEG_MAX_MS     10000         /* ... or at any frame after this (very long GOPs) */
#define HLS_SEG_MAX_BYTES  (1536u << 10) /* ... or at this size */
#define HLS_IDR_ASK_MS     3000          /* smart encoding (long GOPs): ask for a keyframe */
#define HLS_FRAG_BYTES     (256u << 10)  /* moof+mdat fragments inside a segment (muxer buffer) */
#define HLS_GROW           (128u << 10)  /* segment buffer growth step */
#define HLS_SEND_CHUNK     (32u << 10)   /* bytes copied per lock while sending */
#define HLS_WAIT_MS        9000          /* first playlist / init: wait for the producer */
#define HLS_ONE_SEG_MS     5000          /* ... answer with a single segment after this */
#define HLS_TOKENS         8
#define HLS_TOK_UNUSED_S   600           /* a new token must be used within 10 min */
#define HLS_TOK_IDLE_S     120           /* then stays valid while used at least every 2 min */
#define HLS_TOK_MAX_S      86400         /* and at most 24 h */

typedef struct { uint8_t *d; size_t n, cap; int err; } wbuf_t;
typedef struct { uint8_t *d; size_t n; uint32_t seq, dur90; int key; } seg_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static int      s_enabled;              /* hls.enabled */
static struct { char tok[33]; uint32_t made, last; int used; } g_tok[HLS_TOKENS];
static int      g_thread;               /* producer thread exists (until it has cleaned up) */
static int      g_producing;            /* its session is live (segments may be served) */
static int      g_stop_req;
static uint32_t g_session;
static uint32_t g_last_req;             /* mono_s of the last valid request */
static seg_t    g_seg[HLS_RING];        /* oldest first */
static int      g_nseg;
static size_t   g_seg_bytes, g_building, g_peak;
static uint8_t *g_init;
static size_t   g_init_len;
static uint32_t g_next_seq = 1;         /* never reset: segment names stay unique */
static uint32_t g_target;               /* EXT-X-TARGETDURATION of this session (never lowered) */
static uint64_t g_t90_next;             /* timeline: next session starts after the last one */
static int64_t  g_ts_epoch = -1;        /* encoder ts of the first frame (timeline origin) */
static uint32_t g_dropped;              /* segments lost to allocation failures */

/* ---- setting ------------------------------------------------------------------ */
int hls_enabled(void) { return s_enabled; }

static int hls_en_get(char *o, size_t c) { snprintf(o, c, "%s", s_enabled ? "on" : "off"); return 0; }
static int hls_en_set(const char *v, const char **e)
{
    int on;
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true")) on = 1;
    else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false")) on = 0;
    else { *e = "must be on or off"; return -1; }
    pthread_mutex_lock(&g_lock);
    s_enabled = on;
    if (!on) memset(g_tok, 0, sizeof g_tok);      /* every URL handed out stops working */
    pthread_cond_broadcast(&g_cond);              /* the producer stops at its next frame */
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void hls_register_params(void)
{
    CTRL_REG("hls.enabled", hls_en, CTRL_PERSIST);
}

/* ---- tokens --------------------------------------------------------------------- */
static int tok_alive(int i, uint32_t now)       /* g_lock held */
{
    if (!g_tok[i].tok[0]) return 0;
    if (now - g_tok[i].made > HLS_TOK_MAX_S) return 0;
    return now - g_tok[i].last <= (uint32_t)(g_tok[i].used ? HLS_TOK_IDLE_S : HLS_TOK_UNUSED_S);
}

int hls_token_new(char out[33])
{
    uint8_t r[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, r, sizeof r);
    close(fd);
    if (n != (ssize_t)sizeof r) return -1;
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", r[i]);
    uint32_t now = mono_s();
    pthread_mutex_lock(&g_lock);
    if (!s_enabled) { pthread_mutex_unlock(&g_lock); return -1; }
    int slot = -1;
    for (int i = 0; i < HLS_TOKENS && slot < 0; i++) if (!tok_alive(i, now)) slot = i;
    if (slot < 0) {                                 /* all in use: replace the least recently used */
        slot = 0;
        for (int i = 1; i < HLS_TOKENS; i++) if (now - g_tok[i].last > now - g_tok[slot].last) slot = i;
    }
    memcpy(g_tok[slot].tok, out, 33);
    g_tok[slot].made = g_tok[slot].last = now;
    g_tok[slot].used = 0;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* Constant time over the token's bytes and over all slots. g_lock held. */
static int tok_check(const char *tok, uint32_t now)
{
    int found = -1;
    for (int i = 0; i < HLS_TOKENS; i++) {
        unsigned d = 0;
        for (int k = 0; k < 32; k++) d |= (unsigned char)(g_tok[i].tok[k] ^ tok[k]);
        int hit = (d == 0) & tok_alive(i, now);
        found = hit ? i : found;
    }
    if (found < 0) return 0;
    g_tok[found].last = now;
    g_tok[found].used = 1;
    return 1;
}

/* ---- paths ---------------------------------------------------------------------- */
int hls_parse_path(const char *p, char tok[33], uint32_t *seq)
{
    if (!p || strncmp(p, "/hls/", 5) != 0) return HLS_BAD;
    p += 5;
    for (int i = 0; i < 32; i++)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return HLS_BAD;
    if (p[32] != '/') return HLS_BAD;
    const char *f = p + 33;
    size_t n = strcspn(f, "?#");
    int kind = HLS_BAD;
    if (n == 10 && !strncmp(f, "index.m3u8", 10)) kind = HLS_PLAYLIST;
    else if (n == 8 && !strncmp(f, "init.mp4", 8)) kind = HLS_INIT;
    else if (n >= 8 && n <= 17 && !strncmp(f, "seg", 3) && !strncmp(f + n - 4, ".m4s", 4)) {
        uint64_t v = 0;
        size_t i;
        for (i = 3; i < n - 4; i++) {
            if (f[i] < '0' || f[i] > '9') return HLS_BAD;
            v = v * 10 + (uint64_t)(f[i] - '0');
        }
        if ((f[3] == '0' && n - 7 > 1) || v > 0xffffffffu) return HLS_BAD;
        *seq = (uint32_t)v;
        kind = HLS_SEGMENT;
    }
    if (kind != HLS_BAD) { memcpy(tok, p, 32); tok[32] = 0; }
    return kind;
}

/* ---- producer ------------------------------------------------------------------- */
/* stdio cookie: fmp4.c writes here; *target is where the bytes go (NULL: dropped). */
static ssize_t ck_write(void *c, const char *p, size_t n)
{
    wbuf_t *b = *(wbuf_t **)c;
    if (!b || b->err || !n) return (ssize_t)n;
    if (n > b->cap - b->n) {
        size_t want = b->n + n;
        if (want < b->n || want > (64u << 20)) { b->err = 1; return (ssize_t)n; }
        size_t cap = (want + HLS_GROW - 1) / HLS_GROW * HLS_GROW;
        uint8_t *np = realloc(b->d, cap);
        if (!np) { b->err = 1; return (ssize_t)n; }
        b->d = np; b->cap = cap;
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
    return (ssize_t)n;
}

static void free_ring_locked(void)
{
    for (int i = 0; i < g_nseg; i++) free(g_seg[i].d);
    memset(g_seg, 0, sizeof g_seg);
    g_nseg = 0; g_seg_bytes = 0; g_building = 0;
    free(g_init); g_init = NULL; g_init_len = 0;
}

static void note_peak_locked(void)
{
    size_t t = g_seg_bytes + g_building + g_init_len;
    if (t > g_peak) g_peak = t;
}

/* Is this producer still wanted? If not, ends the session (frees the ring). */
static int alive(uint32_t sess)
{
    pthread_mutex_lock(&g_lock);
    int ok = sess == g_session && g_producing && s_enabled && !g_stop_req &&
             mono_s() - g_last_req <= HLS_IDLE_S;
    if (!ok && sess == g_session && g_producing) {
        g_producing = 0;
        free_ring_locked();
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static void publish_init(uint32_t sess, wbuf_t *b)
{
    pthread_mutex_lock(&g_lock);
    if (sess == g_session && g_producing) {
        free(g_init);
        g_init = b->d; g_init_len = b->n;
        b->d = NULL;
        note_peak_locked();
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
    free(b->d);
    memset(b, 0, sizeof *b);
}

static void publish_seg(uint32_t sess, wbuf_t *b, uint64_t dur90, int key)
{
    if (b->err || !b->n) {                          /* out of memory: lose this one (a gap) */
        free(b->d); memset(b, 0, sizeof *b);
        pthread_mutex_lock(&g_lock); g_dropped++; pthread_mutex_unlock(&g_lock);   /* numbers stay consecutive */
        return;
    }
    uint8_t *d = realloc(b->d, b->n);               /* drop the growth slack */
    if (d) b->d = d;
    pthread_mutex_lock(&g_lock);
    if (sess == g_session && g_producing) {
        while (g_nseg > 0 && (g_nseg + 1 > HLS_RING ||
                              (g_nseg + 1 > HLS_MIN_KEEP && g_seg_bytes + b->n > HLS_MEM_MAX))) {
            g_seg_bytes -= g_seg[0].n;
            free(g_seg[0].d);
            memmove(g_seg, g_seg + 1, (size_t)(g_nseg - 1) * sizeof g_seg[0]);
            g_nseg--;
        }
        seg_t *s = &g_seg[g_nseg++];
        s->d = b->d; s->n = b->n; s->seq = g_next_seq++; s->key = key;
        s->dur90 = dur90 > 0xffffffffu ? 0xffffffffu : (uint32_t)dur90;
        g_seg_bytes += b->n;
        b->d = NULL;
        uint32_t t = (uint32_t)((dur90 + 45000) / 90000);   /* EXTINF rounded to the nearest second */
        if (t < 1) t = 1;
        if (t > g_target) g_target = t;
        g_building = 0;
        note_peak_locked();
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
    free(b->d);
    memset(b, 0, sizeof *b);
}

static void *producer(void *arg)
{
    uint32_t sess = (uint32_t)(uintptr_t)arg;
    uint8_t *frame = malloc(LIVE_MAX_FRAME);
    live_item_t *it = malloc(sizeof *it);
    wbuf_t *target = NULL, initb = { 0 }, seg = { 0 };
    cookie_io_functions_t io;
    memset(&io, 0, sizeof io);
    io.write = ck_write;
    FILE *f = frame && it ? fopencookie(&target, "w", io) : NULL;
    if (f) setvbuf(f, NULL, _IONBF, 0);
    fmp4_t *mux = NULL;
    live_cursor_t cur = { 0, 0 };
    uint64_t seg_ts = 0, seg_v0 = 0;
    int seg_key = 0;
    size_t pending = 0;
    live_subscribe();
    if (f) live_rewind(&cur, 6000);          /* frames another viewer kept in the hub: faster start */
    printf("[hls] producer started (session %u)\n", sess);
    while (f && alive(sess)) {
        if (live_next(&cur, frame, LIVE_MAX_FRAME, it, 200) != LIVE_VIDEO) continue;
        if (!mux) {
            if (!it->key) continue;
            target = &initb;
            mux = fmp4_open(f, frame, it->len, 0);
            target = NULL;
            if (!mux || initb.err) {
                if (mux) { fmp4_close(mux); mux = NULL; }
                free(initb.d); memset(&initb, 0, sizeof initb);
                continue;
            }
            pthread_mutex_lock(&g_lock);
            if (g_ts_epoch < 0) g_ts_epoch = (int64_t)it->ts;
            uint64_t t90 = (uint64_t)((int64_t)it->ts - g_ts_epoch) * 90;   /* keeps time moving forward */
            if ((int64_t)it->ts < g_ts_epoch || t90 < g_t90_next) t90 = g_t90_next;
            pthread_mutex_unlock(&g_lock);
            fmp4_set_video_time(mux, t90);
            publish_init(sess, &initb);
            seg_ts = it->ts; seg_v0 = t90; seg_key = 1; pending = 0;
        } else {
            int64_t age = (int64_t)(it->ts - seg_ts);
            if ((it->key && age >= HLS_SEG_MIN_MS) || age >= HLS_SEG_MAX_MS || seg.n + pending >= HLS_SEG_MAX_BYTES) {
                target = &seg; fmp4_flush(mux); target = NULL;
                uint64_t v = fmp4_video_time(mux);
                publish_seg(sess, &seg, v - seg_v0, seg_key);
                seg_ts = it->ts; seg_v0 = v; seg_key = it->key; pending = 0;
            } else if (pending >= HLS_FRAG_BYTES) {
                target = &seg; fmp4_flush(mux); target = NULL;
                pending = 0;
            }
            if (!it->key && age >= HLS_IDR_ASK_MS) venc_request_idr();   /* no-op unless smart encoding (max 1/s) */
        }
        if (fmp4_video(mux, frame, it->len, it->key, it->ts) != 0) {
            /* out of memory: drop the segment, start again at the next keyframe */
            pthread_mutex_lock(&g_lock); g_t90_next = fmp4_video_time(mux) + 90000; pthread_mutex_unlock(&g_lock);
            target = NULL; fmp4_close(mux); mux = NULL;
            seg.err = 1;
            publish_seg(sess, &seg, 0, 0);
            continue;
        }
        pending += it->len;
        pthread_mutex_lock(&g_lock);
        g_building = seg.cap + pending;
        note_peak_locked();
        pthread_mutex_unlock(&g_lock);
    }
    target = NULL;
    if (mux) {
        uint64_t end = fmp4_video_time(mux);
        pthread_mutex_lock(&g_lock); if (end > g_t90_next) g_t90_next = end; pthread_mutex_unlock(&g_lock);
        fmp4_close(mux);                     /* its last fragment goes nowhere */
    }
    if (f) fclose(f);
    free(seg.d); free(initb.d); free(frame); free(it);
    live_unsubscribe();
    printf("[hls] producer stopped (session %u)\n", sess);
    pthread_mutex_lock(&g_lock);
    g_thread = 0;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* Start the producer if no thread exists. g_lock held. 0 ok / running. */
static int ensure_running_locked(void)
{
    if (g_thread) return 0;
    if (g_stop_req || !s_enabled) return -1;
    g_session++;
    g_thread = 1; g_producing = 1; g_target = 0;
    free_ring_locked();
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int rc = pthread_create(&t, &at, producer, (void *)(uintptr_t)g_session);
    pthread_attr_destroy(&at);
    if (rc != 0) { g_thread = 0; g_producing = 0; return -1; }
    return 0;
}

int hls_warm(void)
{
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (s_enabled) { g_last_req = mono_s(); rc = ensure_running_locked(); }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* Wait on g_cond (g_lock held) for at most ms; slices, so a wall-clock jump
 * (NTP) can't stretch it: the caller re-checks its own mono_ms() deadline. */
static void wait_locked(int ms)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += (long)ms * 1000000L;
    while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&g_cond, &g_lock, &dl);
}

/* ---- responses ------------------------------------------------------------------ */
static int send_head(conn_t *cn, int code, const char *reason, const char *ctype, size_t len, const char *cache)
{
    char h[400];
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"          /* hls.js / Chromecast pages: the token is the credential */
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "%s"
        "Connection: close\r\n\r\n",
        code, reason, ctype, len, cache, code == 503 ? "Retry-After: 2\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof h) return -1;
    return conn_send_all(cn, h, (size_t)n);
}

static void reply(conn_t *cn, int code, const char *reason, const char *ctype, const void *body, size_t len, const char *cache)
{
    if (send_head(cn, code, reason, ctype, len, cache) == 0 && len) conn_send_all(cn, body, len);
}

static void reply_text(conn_t *cn, int code, const char *reason, const char *text)
{
    reply(cn, code, reason, "text/plain; charset=utf-8", text, strlen(text), "no-store");
}

/* Playlist text into out. g_lock held, g_nseg > 0. */
static int playlist_locked(char *out, size_t cap)
{
    int first = g_nseg > HLS_LIST ? g_nseg - HLS_LIST : 0, indep = 1;
    for (int i = first; i < g_nseg; i++) indep &= g_seg[i].key != 0;
    size_t n = (size_t)snprintf(out, cap,
        "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:%u\n#EXT-X-MEDIA-SEQUENCE:%u\n%s"
        "#EXT-X-MAP:URI=\"init.mp4\"\n",
        g_target ? g_target : 1, g_seg[first].seq, indep ? "#EXT-X-INDEPENDENT-SEGMENTS\n" : "");
    for (int i = first; i < g_nseg && n < cap; i++) {
        uint32_t ms = (uint32_t)(((uint64_t)g_seg[i].dur90 + 45) / 90);
        n += (size_t)snprintf(out + n, cap - n, "#EXTINF:%u.%03u,\nseg%u.m4s\n", ms / 1000, ms % 1000, g_seg[i].seq);
    }
    return n < cap ? (int)n : -1;
}

static const seg_t *find_locked(uint32_t seq)
{
    for (int i = 0; i < g_nseg; i++) if (g_seg[i].seq == seq) return &g_seg[i];
    return NULL;
}

void hls_serve(conn_t *cn, const char *path)
{
    char tok[33];
    uint32_t seq = 0;
    int kind = hls_parse_path(path, tok, &seq);
    if (kind == HLS_BAD) { reply_text(cn, 404, "Not Found", "not found\n"); return; }
    pthread_mutex_lock(&g_lock);
    if (!s_enabled) { pthread_mutex_unlock(&g_lock); reply_text(cn, 403, "Forbidden", "HLS is off (hls.enabled)\n"); return; }
    if (!tok_check(tok, mono_s())) {
        pthread_mutex_unlock(&g_lock);
        reply_text(cn, 403, "Forbidden", "invalid or expired HLS token\n");
        return;
    }
    g_last_req = mono_s();
    ensure_running_locked();

    if (kind == HLS_PLAYLIST || kind == HLS_INIT) {
        int64_t t0 = mono_ms();
        for (;;) {
            if (!g_thread && ensure_running_locked() != 0) break;
            if (g_producing && kind == HLS_INIT && g_init) break;
            if (g_producing && kind == HLS_PLAYLIST &&
                (g_nseg >= 2 || (g_nseg >= 1 && mono_ms() - t0 >= HLS_ONE_SEG_MS))) break;
            if (mono_ms() - t0 >= HLS_WAIT_MS || g_stop_req || !s_enabled) break;
            g_last_req = mono_s();                      /* waiting counts as watching */
            wait_locked(250);
        }
        if (kind == HLS_INIT) {
            uint8_t *copy = g_producing && g_init ? malloc(g_init_len) : NULL;
            size_t n = g_init_len;
            if (copy) memcpy(copy, g_init, n);
            pthread_mutex_unlock(&g_lock);
            if (copy) reply(cn, 200, "OK", "video/mp4", copy, n, "no-cache");
            else reply_text(cn, 503, "Service Unavailable", "no video yet, retry\n");
            free(copy);
            return;
        }
        char pl[1024];
        int n = g_producing && g_nseg ? playlist_locked(pl, sizeof pl) : -1;
        pthread_mutex_unlock(&g_lock);
        if (n > 0) reply(cn, 200, "OK", "application/vnd.apple.mpegurl", pl, (size_t)n, "no-cache, no-store");
        else reply_text(cn, 503, "Service Unavailable", "no video yet, retry\n");
        return;
    }

    /* segment: sent in pieces copied under the lock; if the ring drops it
     * meanwhile (a client this slow is far behind anyway) the response is
     * cut short and the client retries or skips ahead */
    const seg_t *s = find_locked(seq);
    size_t total = s ? s->n : 0;
    pthread_mutex_unlock(&g_lock);
    if (!s) { reply_text(cn, 404, "Not Found", "segment not available\n"); return; }
    uint8_t *buf = malloc(HLS_SEND_CHUNK);
    if (!buf) { reply_text(cn, 503, "Service Unavailable", "out of memory\n"); return; }
    if (send_head(cn, 200, "OK", "video/mp4", total, "private, max-age=60") == 0) {
        size_t off = 0;
        while (off < total) {
            pthread_mutex_lock(&g_lock);
            s = find_locked(seq);
            size_t k = 0;
            if (s && s->n == total) {
                k = total - off < HLS_SEND_CHUNK ? total - off : HLS_SEND_CHUNK;
                memcpy(buf, s->d + off, k);
            }
            pthread_mutex_unlock(&g_lock);
            if (!k || conn_send_all(cn, buf, k) != 0) break;
            off += k;
        }
    }
    free(buf);
}

int hls_status_json(char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    uint32_t now = mono_s();
    int ntok = 0;
    for (int i = 0; i < HLS_TOKENS; i++) ntok += tok_alive(i, now);
    int n = snprintf(out, cap,
        "{\"enabled\":%s,\"running\":%s,\"session\":%u,\"segments\":%d,\"bytes\":%zu,\"building\":%zu,"
        "\"init\":%zu,\"peak_bytes\":%zu,\"limit_bytes\":%u,\"target\":%u,\"first\":%u,\"next\":%u,"
        "\"tokens\":%d,\"dropped\":%u,\"idle_s\":%u}",
        s_enabled ? "true" : "false", g_producing ? "true" : "false", g_session, g_nseg, g_seg_bytes,
        g_building, g_init_len, g_peak, (unsigned)(HLS_MEM_MAX + HLS_SEG_MAX_BYTES), g_target,
        g_nseg ? g_seg[0].seq : 0, g_next_seq, ntok, g_dropped, g_thread ? now - g_last_req : 0);
    pthread_mutex_unlock(&g_lock);
    return n < 0 ? 0 : (size_t)n < cap ? n : (int)cap - 1;
}

void hls_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_stop_req = 1;
    pthread_cond_broadcast(&g_cond);
    while (g_thread) wait_locked(200);
    free_ring_locked();
    memset(g_tok, 0, sizeof g_tok);
    g_stop_req = 0;
    pthread_mutex_unlock(&g_lock);
}
