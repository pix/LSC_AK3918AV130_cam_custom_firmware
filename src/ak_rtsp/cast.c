/* Chromecast without a browser: see cast.h and docs/hls.md.
 *
 * Cast v2: every message is a 4-byte big-endian length + a protobuf
 * CastMessage {1 protocol_version, 2 source_id, 3 destination_id, 4 namespace,
 * 5 payload_type, 6 payload_utf8}, over TLS to port 8009 (the device's
 * certificate is self-signed: not verified, as every sender does). Sequence:
 * CONNECT + LAUNCH CC1AD845 (Default Media Receiver) to receiver-0, wait for
 * RECEIVER_STATUS with the app's transportId, CONNECT to it, LOAD the HLS URL
 * (streamType LIVE, fMP4 segments), wait for MEDIA_STATUS PLAYING, answer
 * PINGs, then close the socket without CLOSE messages (the receiver keeps
 * playing, as after any sender app exits). */
#include "cast.h"
#include "hls.h"
#include "tls.h"
#include "conn.h"
#include "web.h"
#include "monotime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define APP_ID        "CC1AD845"          /* Default Media Receiver */
#define NS_CONN       "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEART      "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECV       "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA      "urn:x-cast:com.google.cast.media"
#define SENDER        "sender-ak_rtsp"
#define MSG_MAX       (64 * 1024)
#define CAST_WAIT_MS  25000               /* launch + load + first PLAYING */
#define MDNS_MS       3000
#define MAX_DEV       8

/* ---- JSON (just enough for the receiver's status messages) ------------------- */
static const char *ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

static const char *str_end(const char *p)          /* p at '"': after the closing quote */
{
    for (p++; *p; p++) {
        if (*p == '\\') { if (!*++p) return NULL; }
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *skip_val(const char *p)          /* after the value at p, NULL if malformed */
{
    p = ws(p);
    if (*p == '"') return str_end(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = str_end(p); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p > s ? p : NULL;
}

static const char *obj_get(const char *obj, const char *key)   /* value of member key, NULL if absent */
{
    if (!obj) return NULL;
    const char *p = ws(obj);
    if (*p != '{') return NULL;
    p = ws(p + 1);
    size_t kl = strlen(key);
    while (*p == '"') {
        const char *ke = str_end(p);
        if (!ke) return NULL;
        int match = (size_t)(ke - p - 2) == kl && !strncmp(p + 1, key, kl);
        p = ws(ke);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if (match) return p;
        if (!(p = skip_val(p))) return NULL;
        p = ws(p);
        if (*p != ',') break;
        p = ws(p + 1);
    }
    return NULL;
}

static int get_str(const char *v, char *out, size_t cap)
{
    if (!cap) return -1;
    out[0] = 0;
    if (!v || *v != '"') return -1;
    size_t n = 0;
    for (v++; *v && *v != '"'; v++) {
        char c = *v;
        if (c == '\\') {
            c = *++v;
            if (!c) return -1;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') { c = '?'; for (int i = 0; i < 4 && v[1]; i++) v++; }
        }
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return *v == '"' ? 0 : -1;
}

int cast_json_str(const char *obj, const char *key, char *out, size_t cap) { return get_str(obj_get(obj, key), out, cap); }

int cast_find_app(const char *js, const char *appid, char *tr, size_t tcap, char *se, size_t scap)
{
    const char *apps = obj_get(obj_get(js, "status"), "applications");
    if (!apps || *apps != '[') return -1;
    const char *p = ws(apps + 1);
    while (*p == '{') {
        char id[32];
        if (cast_json_str(p, "appId", id, sizeof id) == 0 && !strcmp(id, appid) &&
            cast_json_str(p, "transportId", tr, tcap) == 0 && tr[0]) {
            cast_json_str(p, "sessionId", se, scap);
            return 0;
        }
        if (!(p = skip_val(p))) return -1;
        p = ws(p);
        if (*p != ',') break;
        p = ws(p + 1);
    }
    return -1;
}

int cast_media_status(const char *js, char *state, size_t cap, double *cur, long *msid)
{
    const char *st = obj_get(js, "status");
    if (cap) state[0] = 0;
    *msid = -1;
    if (!st || *st != '[') return -1;
    st = ws(st + 1);
    if (*st != '{') return -1;
    cast_json_str(st, "playerState", state, cap);
    char *e = NULL;
    const char *v = obj_get(st, "mediaSessionId");
    long id = v ? strtol(v, &e, 10) : -1;
    *msid = v && e != v && id >= 0 ? id : -1;            /* set even without currentTime */
    v = obj_get(st, "currentTime");
    double d = v ? strtod(v, &e) : 0;
    if (!v || e == v || !(d >= 0 && d < 1e9)) return -1;
    *cur = d;
    return 0;
}

void cast_player_state(const char *js, char *out, size_t cap)
{
    const char *st = obj_get(js, "status");
    if (cap) out[0] = 0;
    if (!st || *st != '[') return;
    st = ws(st + 1);
    if (*st == '{') cast_json_str(st, "playerState", out, cap);
}

/* ---- protobuf framing ----------------------------------------------------------- */
static size_t put_varint(unsigned char *p, size_t v)
{
    size_t n = 0;
    do { unsigned char b = v & 0x7f; v >>= 7; p[n++] = (unsigned char)(b | (v ? 0x80 : 0)); } while (v);
    return n;
}

static size_t put_str(unsigned char *p, unsigned tag, const char *s, size_t l)
{
    p[0] = (unsigned char)tag;
    size_t n = 1 + put_varint(p + 1, l);
    memcpy(p + n, s, l);
    return n + l;
}

size_t cast_encode(unsigned char *out, size_t cap, const char *src, const char *dst, const char *ns, const char *json)
{
    size_t ls = strlen(src), ld = strlen(dst), ln = strlen(ns), lj = strlen(json);
    if (lj > MSG_MAX || ls > 255 || ld > 255 || ln > 255 || 4 + 4 + 4 * 11 + ls + ld + ln + lj > cap) return 0;
    size_t n = 4;
    out[n++] = 0x08; out[n++] = 0;                     /* protocol_version CASTV2_1_0 */
    n += put_str(out + n, 0x12, src, ls);
    n += put_str(out + n, 0x1a, dst, ld);
    n += put_str(out + n, 0x22, ns, ln);
    out[n++] = 0x28; out[n++] = 0;                     /* payload_type STRING */
    n += put_str(out + n, 0x32, json, lj);
    uint32_t l = (uint32_t)(n - 4);
    out[0] = (unsigned char)(l >> 24); out[1] = (unsigned char)(l >> 16); out[2] = (unsigned char)(l >> 8); out[3] = (unsigned char)l;
    return n;
}

static int get_varint(const unsigned char *p, size_t len, size_t *i, uint64_t *v)
{
    *v = 0;
    for (int s = 0; s < 64; s += 7) {
        if (*i >= len) return -1;
        unsigned char b = p[(*i)++];
        *v |= (uint64_t)(b & 0x7f) << s;
        if (!(b & 0x80)) return 0;
    }
    return -1;
}

int cast_decode(const unsigned char *p, size_t len, cast_msg_t *m)
{
    memset(m, 0, sizeof *m);
    size_t i = 0;
    while (i < len) {
        uint64_t key, v;
        if (get_varint(p, len, &i, &key)) return -1;
        unsigned f = (unsigned)(key >> 3), wt = (unsigned)(key & 7);
        if (wt == 0) { if (get_varint(p, len, &i, &v)) return -1; }
        else if (wt == 2) {
            if (get_varint(p, len, &i, &v) || v > len - i) return -1;
            size_t l = (size_t)v;
            char *d = f == 2 ? m->src : f == 3 ? m->dst : f == 4 ? m->ns : NULL;
            size_t c = f == 2 ? sizeof m->src : f == 3 ? sizeof m->dst : sizeof m->ns;
            if (d) { size_t k = l < c - 1 ? l : c - 1; memcpy(d, p + i, k); d[k] = 0; }
            if (f == 6) { m->payload = (const char *)p + i; m->plen = l; }
            i += l;
        } else if (wt == 5) { if (len - i < 4) return -1; i += 4; }
        else if (wt == 1) { if (len - i < 8) return -1; i += 8; }
        else return -1;
    }
    return 0;
}

/* ---- mDNS ------------------------------------------------------------------------ */
static int name_has_label(const unsigned char *p, size_t n, size_t i, const char *label)
{
    size_t ll = strlen(label);
    for (int hops = 0; hops < 64 && i < n; hops++) {
        unsigned c = p[i];
        if (c == 0) return 0;
        if ((c & 0xc0) == 0xc0) { if (i + 1 >= n) return 0; i = (size_t)(c & 0x3f) << 8 | p[i + 1]; continue; }
        if (c & 0xc0 || i + 1 + c > n) return 0;
        if (c == ll && !strncasecmp((const char *)p + i + 1, label, ll)) return 1;
        i += 1 + c;
    }
    return 0;
}

static int skip_name(const unsigned char *p, size_t n, size_t *i)
{
    for (int k = 0; k < 128; k++) {
        if (*i >= n) return -1;
        unsigned c = p[*i];
        if (c == 0) { (*i)++; return 0; }
        if ((c & 0xc0) == 0xc0) { if (*i + 2 > n) return -1; *i += 2; return 0; }
        if (c & 0xc0) return -1;
        *i += 1 + c;
    }
    return -1;
}

static void copy_txt(char *out, size_t cap, const unsigned char *s, size_t l)
{
    size_t n = 0;
    for (size_t k = 0; k < l && n + 1 < cap; k++)
        out[n++] = s[k] < 0x20 || s[k] == '"' || s[k] == '\\' || s[k] == 0x7f ? ' ' : (char)s[k];
    out[n] = 0;
}

int cast_parse_mdns(const unsigned char *p, size_t n, const char *src_ip,
                    char *name, size_t ncap, char *model, size_t mcap, char *ip, size_t icap, int *port)
{
    if (ncap) name[0] = 0;
    if (mcap) model[0] = 0;
    snprintf(ip, icap, "%s", src_ip);
    *port = 8009;
    if (n < 12 || !(p[2] & 0x80)) return -1;           /* responses only */
    unsigned qd = (unsigned)p[4] << 8 | p[5], rr = ((unsigned)p[6] << 8 | p[7]) + ((unsigned)p[8] << 8 | p[9]) + ((unsigned)p[10] << 8 | p[11]);
    size_t i = 12;
    for (unsigned q = 0; q < qd && q < 64; q++) { if (skip_name(p, n, &i) || n - i < 4) return -1; i += 4; }
    int cast = 0, have_a = 0;
    for (unsigned r = 0; r < rr && r < 256; r++) {
        size_t owner = i;
        if (skip_name(p, n, &i) || n - i < 10) return -1;
        unsigned type = (unsigned)p[i] << 8 | p[i + 1];
        size_t rdl = (size_t)p[i + 8] << 8 | p[i + 9];
        i += 10;
        if (rdl > n - i) return -1;
        const unsigned char *rd = p + i;
        int ours = name_has_label(p, n, owner, "_googlecast");
        cast |= ours;
        if (type == 16 && ours) {                       /* TXT: fn=<friendly name>, md=<model> */
            for (size_t k = 0; k < rdl;) {
                size_t l = rd[k++];
                if (l > rdl - k) break;
                if (l > 3 && !memcmp(rd + k, "fn=", 3)) copy_txt(name, ncap, rd + k + 3, l - 3);
                else if (l > 3 && !memcmp(rd + k, "md=", 3)) copy_txt(model, mcap, rd + k + 3, l - 3);
                k += l;
            }
        } else if (type == 33 && ours && rdl >= 6) {
            *port = rd[4] << 8 | rd[5];
        } else if (type == 1 && rdl == 4 && !have_a) {
            snprintf(ip, icap, "%u.%u.%u.%u", rd[0], rd[1], rd[2], rd[3]);
            have_a = 1;
        }
        i += rdl;
    }
    return cast && *port > 0 ? 0 : -1;
}

/* ---- state ---------------------------------------------------------------------- */
typedef struct { char name[64], model[48], ip[16]; int port; } cast_dev_t;
enum { JOB_CAST, JOB_STOP, JOB_DISCOVER };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int  g_busy;
static char g_state[16] = "idle";
static char g_msg[200];
static char g_device[32];
static char g_cast_tok[33];             /* token handed to the device we cast to last (g_lock) */
static cast_dev_t g_dev[MAX_DEV];
static int  g_ndev;
static struct { int kind; char ip[16]; int port; } g_job;

static void set_state(const char *st, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_state(const char *st, const char *fmt, ...)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_state, sizeof g_state, "%s", st);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_lock);
    printf("[cast] %s %s\n", st, g_msg);
}

/* ---- network -------------------------------------------------------------------- */
static int tcp_connect_to(const char *ip, int port, int ms, char *err, size_t ecap)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { snprintf(err, ecap, "bad address"); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { snprintf(err, ecap, "socket: %s", strerror(errno)); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        if (errno != EINPROGRESS) { snprintf(err, ecap, "cannot connect to %s:%d (%s)", ip, port, strerror(errno)); close(fd); return -1; }
        struct pollfd pf = { fd, POLLOUT, 0 };
        int e = 0;
        socklen_t el = sizeof e;
        if (poll(&pf, 1, ms) <= 0) { snprintf(err, ecap, "no answer from %s:%d", ip, port); close(fd); return -1; }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) != 0 || e) {
            snprintf(err, ecap, "cannot connect to %s:%d (%s)", ip, port, strerror(e ? e : errno));
            close(fd); return -1;
        }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int send_msg(conn_t *c, const char *dst, const char *ns, const char *json)
{
    size_t cap = strlen(json) + 512;
    unsigned char *b = malloc(cap);
    size_t n = b ? cast_encode(b, cap, SENDER, dst, ns, json) : 0;
    int rc = n ? conn_send_all(c, b, n) : -1;
    free(b);
    return rc;
}

static int read_n(conn_t *c, unsigned char *b, size_t n)
{
    while (n) {
        ssize_t r = conn_recv(c, b, n);
        if (r <= 0) return -1;
        b += r; n -= (size_t)r;
    }
    return 0;
}

/* Next message within ms: 1 (m filled, json = NUL-terminated payload), 0 none, -1 gone. */
static int recv_msg(conn_t *c, unsigned char *buf, char *json, cast_msg_t *m, int ms)
{
    if (!conn_readable(c, ms)) return 0;
    unsigned char h[4];
    if (read_n(c, h, 4)) return -1;
    size_t len = (size_t)h[0] << 24 | (size_t)h[1] << 16 | (size_t)h[2] << 8 | h[3];
    if (!len || len > MSG_MAX || read_n(c, buf, len) || cast_decode(buf, len, m)) return -1;
    size_t pl = m->plen < MSG_MAX ? m->plen : MSG_MAX;
    if (m->payload) memcpy(json, m->payload, pl);
    json[m->payload ? pl : 0] = 0;
    return 1;
}

static void json_escape(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
}

/* LOAD the camera stream in the receiver session se (escaped). mp4: live.mp4,
 * an endless fragmented MP4 the receiver plays as plain media (~1 s behind
 * live); else the HLS playlist (~3 segments behind). No currentTime: LIVE
 * starts at the live edge. 0 ok. */
static int send_load(conn_t *c, const char *transport, const char *se, const char *local, const char *tok,
                     int mp4, int req, char *out, size_t cap)
{
    char url[128];
    const char *file = mp4 ? "live.mp4" : "index.m3u8";
    if (WEB_PORT == 80) snprintf(url, sizeof url, "http://%s/hls/%s/%s", local, tok, file);
    else snprintf(url, sizeof url, "http://%s:%d/hls/%s/%s", local, WEB_PORT, tok, file);
    if (mp4)
        snprintf(out, cap,
            "{\"type\":\"LOAD\",\"requestId\":%d,\"sessionId\":\"%s\",\"autoplay\":true,"
            "\"media\":{\"contentId\":\"%s\",\"contentUrl\":\"%s\",\"contentType\":\"video/mp4\","
            "\"streamType\":\"LIVE\",\"metadata\":{\"metadataType\":0,\"title\":\"Camera live\"}}}", req, se, url, url);
    else
        snprintf(out, cap,
            "{\"type\":\"LOAD\",\"requestId\":%d,\"sessionId\":\"%s\",\"autoplay\":true,"
            "\"media\":{\"contentId\":\"%s\",\"contentUrl\":\"%s\",\"contentType\":\"application/x-mpegurl\","
            "\"streamType\":\"LIVE\",\"hlsSegmentFormat\":\"fmp4\",\"hlsVideoSegmentFormat\":\"fmp4\","
            "\"metadata\":{\"metadataType\":0,\"title\":\"Camera live\"}}}", req, se, url, url);
    return send_msg(c, transport, NS_MEDIA, out);
}

/* ---- catch-up (live.mp4) --------------------------------------------------------
 * A Cast receiver buffers before it starts playing, then plays at normal speed:
 * that buffer stays as delay. Measured on an Android TV (2020/2021 UHD): PLAYING
 * at position 0, ~6 s of BUFFERING, then playing 8 s behind. SEEK does not help
 * there (an endless stream is not seekable: it went back ~1.3 s), so the camera
 * changes the playback rate instead: once the receiver really plays (PLAYING
 * and the position moving, TUNE_SETTLE_MS) it compares the position with the
 * media time live.mp4 has sent (hls_mp4_sent); above LAG_MAX it plays at
 * RATE_FAST down to LAG_SLOW, at RATE_SLOW down to LAG_TARGET, then 1.0 (also
 * when the receiver stops playing, on cancel, or after TUNE_MS of playing).
 * There 1.3x took the delay from 10.8 s to 1.5 s in 31 s without buffering. */
#define TUNE_WAIT_MS   30000           /* for the receiver to really start playing */
#define TUNE_MS        60000           /* of playing: catch-up budget */
#define TUNE_SETTLE_MS 2000            /* PLAYING with the position moving this long before measuring */
#define LAG_MAX        1.5
#define LAG_SLOW       3.0
#define LAG_TARGET     1.0
#define RATE_FAST      "1.5"
#define RATE_SLOW      "1.2"
#define TRACE_AFTER_MS 10000           /* keep watching (trace only) after deciding */

static int g_tuning, g_cancel;         /* g_lock: catch-up running / asked to stop */

/* What the receiver did during the last cast (GET /api/hls/cast "trace"):
 * [t, playerState, position, media sent, event], t = camera monotonic s. */
#define TRACE_N 128
static struct { double t, cur, sent; char st[12], ev[20]; } g_tr[TRACE_N];
static int g_ntr;

static void trace(const char *ev, const char *st, double cur, double sent)
{
    pthread_mutex_lock(&g_lock);
    if (g_ntr < TRACE_N) {
        g_tr[g_ntr].t = (double)mono_ms() / 1000.0;
        g_tr[g_ntr].cur = cur; g_tr[g_ntr].sent = sent;
        snprintf(g_tr[g_ntr].st, sizeof g_tr[g_ntr].st, "%s", st ? st : "");
        snprintf(g_tr[g_ntr].ev, sizeof g_tr[g_ntr].ev, "%s", ev ? ev : "");
        g_ntr++;
    }
    pthread_mutex_unlock(&g_lock);
}

static int tune_cancelled(void)
{
    pthread_mutex_lock(&g_lock);
    int c = g_cancel;
    pthread_mutex_unlock(&g_lock);
    return c;
}

static int send_rate(conn_t *c, const char *transport, char *out, int *rid, long msid, const char *rate)
{
    snprintf(out, 1024, "{\"type\":\"SET_PLAYBACK_RATE\",\"requestId\":%d,\"mediaSessionId\":%ld,\"playbackRate\":%s}",
             (*rid)++, msid, rate);
    char ev[20];
    snprintf(ev, sizeof ev, "rate %s", rate);
    trace(ev, "", -1, -1);
    return send_msg(c, transport, NS_MEDIA, out);
}

static void tune(conn_t *c, unsigned char *buf, char *json, char *out, const char *transport, const char *tok, long msid)
{
    enum { MEASURE, FAST, SLOW, DONE } st = MEASURE;
    int64_t start = mono_ms(), next_q = start, ping = start + 5000, playing_since = 0;
    int64_t stop_at = start + TUNE_WAIT_MS;    /* then TUNE_MS once playing; after DONE TRACE_AFTER_MS of watching */
    double first = -1, lag = -1;
    int rid = 10, rate_on = 0;
    double last_cur = 0;
    char last_ps[24] = "-";
    int64_t last_tr = 0;
    const char *how = "";
    pthread_mutex_lock(&g_lock);
    g_tuning = 1;
    pthread_mutex_unlock(&g_lock);
    while (mono_ms() < stop_at && !tune_cancelled()) {
        int64_t now = mono_ms();
        if (now >= ping) { send_msg(c, "receiver-0", NS_HEART, "{\"type\":\"PING\"}"); ping = now + 5000; }
        if (now >= next_q) {
            snprintf(out, 1024, "{\"type\":\"GET_STATUS\",\"requestId\":%d,\"mediaSessionId\":%ld}", rid++, msid);
            if (send_msg(c, transport, NS_MEDIA, out)) break;
            next_q = now + 700;
        }
        cast_msg_t m;
        int r = recv_msg(c, buf, json, &m, 200);
        if (r < 0) break;
        if (r == 0) continue;
        char type[40] = "", ps[24];
        cast_json_str(json, "type", type, sizeof type);
        if (!strcmp(m.ns, NS_HEART)) {
            if (!strcmp(type, "PING")) send_msg(c, m.src[0] ? m.src : "receiver-0", NS_HEART, "{\"type\":\"PONG\"}");
            continue;
        }
        if (strcmp(m.ns, NS_MEDIA)) continue;
        if (strcmp(type, "MEDIA_STATUS")) {                                          /* INVALID_REQUEST, ... */
            trace(type, "", -1, -1);
            if (rate_on && st != DONE && (!strcmp(type, "INVALID_REQUEST") || !strcmp(type, "INVALID_PLAYER_STATE"))) {
                how = "the TV refused to play faster"; st = DONE; rate_on = 0; stop_at = now + TRACE_AFTER_MS;
            }
            continue;
        }
        double cur = 0, sent = 0;
        long id = -1;
        int pos = cast_media_status(json, ps, sizeof ps, &cur, &id) == 0;
        int snt = hls_mp4_sent(tok, &sent) == 0;
        int have = pos && snt;
        if (strcmp(ps, last_ps) || now - last_tr >= 1000) {         /* trace: changes, at most 1/s otherwise */
            trace("", ps, pos ? cur : -1, snt ? sent : -1);
            snprintf(last_ps, sizeof last_ps, "%s", ps);
            last_tr = now;
        }
        if (have) {
            lag = sent - cur;
            /* "Playing" = PLAYING with the position moving: at start-up a receiver says PLAYING for a moment, then
             * BUFFERING at position 0 while it fills its start-up buffer (seen on an Android TV: ~5 s). */
            if (!strcmp(ps, "PLAYING") && cur > 0.05 && cur > last_cur) { if (!playing_since) playing_since = now; }
            else if (strcmp(ps, "PLAYING")) playing_since = 0;
            last_cur = cur;
        }
        int ready = playing_since && now - playing_since >= TUNE_SETTLE_MS;
        if (rate_on && !snt) {                     /* the receiver no longer fetches live.mp4: normal speed, stop */
            send_rate(c, transport, out, &rid, msid, "1");
            how = "the stream ended"; rate_on = 0; st = DONE; stop_at = now + TRACE_AFTER_MS;
            continue;
        }
        if (st == DONE) continue;                  /* only watching now */
        if (st == MEASURE) {
            if (!have || !ready) continue;
            first = lag;
            stop_at = now + TUNE_MS;
            if (lag <= LAG_MAX) { how = "no catch-up needed"; st = DONE; stop_at = now + TRACE_AFTER_MS; continue; }
            st = lag > LAG_SLOW ? FAST : SLOW;
            if (send_rate(c, transport, out, &rid, msid, st == FAST ? RATE_FAST : RATE_SLOW)) break;
            rate_on = 1;
            set_state("playing", "playing on %s (low latency MP4, %.1f s behind: catching up)", g_device, first);
        } else if (have) {
            int finish = 0;
            if (lag <= LAG_TARGET) { how = "played faster for a moment"; finish = 1; }
            else if (strcmp(ps, "PLAYING")) { how = "stopped catching up (the TV ran out of data)"; finish = 1; }
            else if (st == FAST && lag <= LAG_SLOW) {
                if (send_rate(c, transport, out, &rid, msid, RATE_SLOW)) break;
                st = SLOW;
            }
            if (finish) {                          /* normal speed now, then watch */
                send_rate(c, transport, out, &rid, msid, "1");
                rate_on = 0; st = DONE; stop_at = now + TRACE_AFTER_MS;
            }
        }
    }
    if (rate_on) send_rate(c, transport, out, &rid, msid, "1");
    if (st != DONE && !*how) how = tune_cancelled() ? "catch-up cancelled" : "the TV did not catch up";
    if (first >= 0) set_state("playing", "playing on %s (low latency MP4, %.1f s behind -> %.1f s: %s)", g_device, first, lag, how);
    pthread_mutex_lock(&g_lock);
    g_tuning = 0; g_cancel = 0;
    pthread_mutex_unlock(&g_lock);
}

static void do_cast(const char *ip, int port, int stop)
{
    char err[160] = "", local[16] = "", tok[33];
    pthread_mutex_lock(&g_lock);
    snprintf(g_device, sizeof g_device, "%s:%d", ip, port);
    pthread_mutex_unlock(&g_lock);
    if (!stop) {
        if (!hls_enabled()) { set_state("error", "HLS is off (hls.enabled)"); return; }
        if (tls_redirect()) { set_state("error", "the HTTP->HTTPS redirect is on: a Chromecast does not trust the camera's own certificate; turn the redirect off to cast"); return; }
    }
    int mp4 = hls_cast_mp4();
    if (!stop && !mp4) hls_warm();     /* HLS: segments build while the receiver app starts (saves ~4 s) */
    {   /* one cast at a time: the URL handed out before stops working (Stop, or a new cast) */
        char old[33];
        pthread_mutex_lock(&g_lock);
        snprintf(old, sizeof old, "%s", g_cast_tok);
        g_cast_tok[0] = 0;
        pthread_mutex_unlock(&g_lock);
        hls_token_revoke(old);
    }
    if (!stop) { pthread_mutex_lock(&g_lock); g_ntr = 0; pthread_mutex_unlock(&g_lock); trace("connect", "", -1, -1); }
    set_state("connecting", "%s", g_device);
    int fd = tcp_connect_to(ip, port, 5000, err, sizeof err);
    if (fd < 0) { set_state("error", "%s", err); return; }
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    if (getsockname(fd, (struct sockaddr *)&me, &ml) == 0) inet_ntop(AF_INET, &me.sin_addr, local, sizeof local);
    conn_t *c = tls_client_connect(fd, ip, 0, 0, NULL, 8000, err, sizeof err);   /* takes fd */
    if (!c) { set_state("error", "TLS with %s: %s", g_device, err); return; }
    unsigned char *buf = malloc(MSG_MAX);
    char *json = malloc(MSG_MAX + 1), *out = malloc(1024);
    if (!buf || !json || !out) { set_state("error", "out of memory"); goto done; }
    if (send_msg(c, "receiver-0", NS_CONN, "{\"type\":\"CONNECT\",\"userAgent\":\"ak_rtsp\"}") ||
        send_msg(c, "receiver-0", NS_RECV, stop ? "{\"type\":\"GET_STATUS\",\"requestId\":1}"
                                                : "{\"type\":\"LAUNCH\",\"appId\":\"" APP_ID "\",\"requestId\":1}")) {
        set_state("error", "%s closed the connection", g_device); goto done;
    }
    if (!stop) set_state("launching", "starting the media receiver on %s", g_device);
    char transport[64] = "", session[64] = "", type[40], ps[24], se[80] = "";
    int loaded = 0, ok = 0;
    long msid = -1;                       /* mediaSessionId (catch-up) */
    int64_t deadline = mono_ms() + CAST_WAIT_MS, ping = mono_ms() + 5000;
    while (mono_ms() < deadline && !ok) {
        cast_msg_t m;
        int r = recv_msg(c, buf, json, &m, 500);
        if (r < 0) { set_state("error", "%s closed the connection", g_device); goto done; }
        if (mono_ms() >= ping) { send_msg(c, "receiver-0", NS_HEART, "{\"type\":\"PING\"}"); ping = mono_ms() + 5000; }
        if (r == 0) continue;
        cast_json_str(json, "type", type, sizeof type);
        if (!strcmp(m.ns, NS_HEART)) {
            if (!strcmp(type, "PING")) send_msg(c, m.src[0] ? m.src : "receiver-0", NS_HEART, "{\"type\":\"PONG\"}");
        } else if (!strcmp(m.ns, NS_CONN)) {
            if (!strcmp(type, "CLOSE")) { set_state("error", "%s closed the session", g_device); goto done; }
        } else if (!strcmp(m.ns, NS_RECV)) {
            if (!strcmp(type, "LAUNCH_ERROR") || !strcmp(type, "INVALID_REQUEST")) {
                char why[64] = "";
                cast_json_str(json, "reason", why, sizeof why);
                set_state("error", "%s refused to start the media receiver (%s %s)", g_device, type, why);
                goto done;
            }
            if (strcmp(type, "RECEIVER_STATUS") != 0) continue;
            int found = cast_find_app(json, APP_ID, transport, sizeof transport, session, sizeof session) == 0;
            if (stop) {
                if (!found) { set_state("stopped", "nothing of ours was playing on %s", g_device); ok = 1; break; }
                char sess[80];
                json_escape(sess, sizeof sess, session);
                snprintf(out, 1024, "{\"type\":\"STOP\",\"sessionId\":\"%s\",\"requestId\":2}", sess);
                send_msg(c, "receiver-0", NS_RECV, out);
                set_state("stopped", "stopped the media receiver on %s", g_device);
                ok = 1;
            } else if (found && !loaded) {
                /* bound to the device (a group's members fetch the stream themselves: port 8009 only) */
                struct in_addr dev = { 0 };
                if (port != 8009 || inet_pton(AF_INET, ip, &dev) != 1) dev.s_addr = 0;
                if (!local[0] || hls_token_new_for(tok, dev.s_addr) != 0) { set_state("error", "cannot create an HLS token"); goto done; }
                pthread_mutex_lock(&g_lock);
                snprintf(g_cast_tok, sizeof g_cast_tok, "%s", tok);
                pthread_mutex_unlock(&g_lock);
                json_escape(se, sizeof se, session);
                if (send_msg(c, transport, NS_CONN, "{\"type\":\"CONNECT\",\"userAgent\":\"ak_rtsp\"}") ||
                    send_load(c, transport, se, local, tok, mp4, 2, out, 1024)) { set_state("error", "%s closed the connection", g_device); goto done; }
                loaded = 1;
                trace(mp4 ? "load mp4" : "load hls", "", -1, -1);
                set_state("loading", "%s is loading http://%s/hls/... (%s)", g_device, local, mp4 ? "low latency MP4" : "HLS");
            }
        } else if (!strcmp(m.ns, NS_MEDIA)) {
            int failed = 0;
            if (!strcmp(type, "LOAD_FAILED") || !strcmp(type, "LOAD_CANCELLED") || !strcmp(type, "INVALID_REQUEST")) failed = 1;
            else if (!strcmp(type, "MEDIA_STATUS")) {
                cast_player_state(json, ps, sizeof ps);
                { double cur; long id = -1; char s2[24]; cast_media_status(json, s2, sizeof s2, &cur, &id); if (id >= 0) msid = id; }
                trace("", ps, -1, -1);
                if (!strcmp(ps, "PLAYING")) { set_state("playing", "playing on %s (%s)", g_device, mp4 ? "low latency MP4" : "HLS"); ok = 1; }
                else if (!strcmp(ps, "IDLE")) {
                    char why[32] = "";
                    const char *st = obj_get(json, "status");
                    if (st && *st == '[') cast_json_str(ws(st + 1), "idleReason", why, sizeof why);
                    if (!strcmp(why, "ERROR")) { failed = 1; snprintf(type, sizeof type, "ERROR"); }
                } else if (ps[0]) set_state("loading", "%s: %s", g_device, ps);
            }
            if (failed && mp4 && loaded) {             /* the receiver cannot play live.mp4: same session, HLS */
                mp4 = 0;
                hls_warm();
                if (send_load(c, transport, se, local, tok, 0, 3, out, 1024)) { set_state("error", "%s closed the connection", g_device); goto done; }
                set_state("loading", "%s refused the low latency stream (%s), trying HLS", g_device, type);
                deadline = mono_ms() + CAST_WAIT_MS;
            } else if (failed) {
                set_state("error", "%s could not play the stream (%s)", g_device, type); goto done;
            }
        }
    }
    if (ok && mp4 && msid >= 0) tune(c, buf, json, out, transport, tok, msid);   /* the receiver's start-up buffer */
    if (!ok) {
        pthread_mutex_lock(&g_lock);
        int loading = !strcmp(g_state, "loading");
        pthread_mutex_unlock(&g_lock);
        if (loading) set_state("loading", "%s accepted the stream but is not playing yet", g_device);
        else set_state("error", "no answer from the receiver on %s", g_device);
    }
done:
    free(buf); free(json); free(out);
    conn_close(c);                       /* no CLOSE: the receiver keeps playing */
}

static void do_discover(void)
{
    set_state("discovering", "looking for Cast devices");
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) { set_state("error", "socket: %s", strerror(errno)); return; }
    int ttl = 255;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    static const unsigned char q[] = {
        0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        11, '_', 'g', 'o', 'o', 'g', 'l', 'e', 'c', 'a', 's', 't', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0, 12, 0x80, 1 };                             /* PTR, IN, unicast response wanted */
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    cast_dev_t found[MAX_DEV];
    int nf = 0, sent = 0;
    unsigned char *pk = malloc(9000);
    int64_t end = mono_ms() + MDNS_MS;
    while (pk && mono_ms() < end) {
        if (sent < 2 && mono_ms() >= end - MDNS_MS + sent * 1000) {
            if (sendto(s, q, sizeof q, 0, (struct sockaddr *)&to, sizeof to) < 0 && !sent) {
                set_state("error", "mDNS query failed: %s", strerror(errno)); break;
            }
            sent++;
        }
        struct pollfd pf = { s, POLLIN, 0 };
        if (poll(&pf, 1, 200) <= 0) continue;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(s, pk, 9000, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        char src[16], name[64], model[48], ip[16];
        int port;
        inet_ntop(AF_INET, &from.sin_addr, src, sizeof src);
        if (cast_parse_mdns(pk, (size_t)n, src, name, sizeof name, model, sizeof model, ip, sizeof ip, &port) != 0) continue;
        int dup = 0;
        for (int i = 0; i < nf; i++) dup |= !strcmp(found[i].ip, ip) && found[i].port == port;
        if (dup || nf == MAX_DEV) continue;
        snprintf(found[nf].name, sizeof found[nf].name, "%s", name[0] ? name : ip);
        snprintf(found[nf].model, sizeof found[nf].model, "%s", model);
        snprintf(found[nf].ip, sizeof found[nf].ip, "%s", ip);
        found[nf].port = port;
        nf++;
    }
    free(pk);
    close(s);
    pthread_mutex_lock(&g_lock);
    memcpy(g_dev, found, sizeof(cast_dev_t) * (size_t)nf);
    g_ndev = nf;
    int failed = !strcmp(g_state, "error");
    pthread_mutex_unlock(&g_lock);
    if (!failed) set_state("idle", "%d Cast device%s found", nf, nf == 1 ? "" : "s");
}

static void *worker(void *arg)
{
    (void)arg;
    if (g_job.kind == JOB_DISCOVER) do_discover();
    else do_cast(g_job.ip, g_job.port, g_job.kind == JOB_STOP);
    pthread_mutex_lock(&g_lock);
    g_busy = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static int start_job(int kind, const char *ip, int port)
{
    pthread_mutex_lock(&g_lock);
    if (g_busy && g_tuning) {           /* only catching up: Stop / a new cast cancels it (it polls every 200 ms) */
        g_cancel = 1;
        for (int i = 0; i < 20 && g_busy; i++) { pthread_mutex_unlock(&g_lock); usleep(100000); pthread_mutex_lock(&g_lock); }
    }
    if (g_busy) { pthread_mutex_unlock(&g_lock); return -1; }
    g_busy = 1;
    g_job.kind = kind;
    snprintf(g_job.ip, sizeof g_job.ip, "%s", ip ? ip : "");
    g_job.port = port;
    pthread_mutex_unlock(&g_lock);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);          /* TLS handshake */
    int rc = pthread_create(&t, &at, worker, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) { pthread_mutex_lock(&g_lock); g_busy = 0; pthread_mutex_unlock(&g_lock); return -1; }
    return 0;
}

int cast_api(int is_post, const char *sub, const char *host, int port, char *out, size_t cap)
{
    if (!is_post && !*sub) {
        pthread_mutex_lock(&g_lock);
        char msg[420];
        json_escape(msg, sizeof msg, g_msg);
        int n = snprintf(out, cap, "{\"state\":\"%s\",\"busy\":%s,\"message\":\"%s\",\"device\":\"%s\",\"devices\":[",
                         g_state, g_busy ? "true" : "false", msg, g_device);
        for (int i = 0; i < g_ndev && n > 0 && (size_t)n < cap; i++)
            n += snprintf(out + n, cap - (size_t)n, "%s{\"name\":\"%s\",\"model\":\"%s\",\"host\":\"%s\",\"port\":%d}",
                          i ? "," : "", g_dev[i].name, g_dev[i].model, g_dev[i].ip, g_dev[i].port);
        if (n > 0 && (size_t)n + 16 < cap) n += snprintf(out + n, cap - (size_t)n, "],\"trace\":[");
        for (int i = 0; i < g_ntr && n > 0 && (size_t)n + 160 < cap; i++) {   /* receiver states are device text: escaped */
            char st[40], ev[60];
            json_escape(st, sizeof st, g_tr[i].st);
            json_escape(ev, sizeof ev, g_tr[i].ev);
            n += snprintf(out + n, cap - (size_t)n, "%s[%.2f,\"%s\",%.2f,%.2f,\"%s\"]", i ? "," : "",
                          g_tr[i].t, st, g_tr[i].cur, g_tr[i].sent, ev);
        }
        if (n > 0 && (size_t)n < cap) snprintf(out + n, cap - (size_t)n, "]}");
        pthread_mutex_unlock(&g_lock);
        return 200;
    }
    if (!is_post) { snprintf(out, cap, "{\"error\":\"use POST\"}"); return 405; }
    int kind = !strcmp(sub, "/discover") ? JOB_DISCOVER : !strcmp(sub, "/stop") ? JOB_STOP : !*sub ? JOB_CAST : -1;
    if (kind < 0) { snprintf(out, cap, "{\"error\":\"not found\"}"); return 404; }
    if (!hls_enabled()) { snprintf(out, cap, "{\"error\":\"HLS is off (hls.enabled)\"}"); return 403; }
    struct in_addr a;
    if (kind != JOB_DISCOVER && (!host || inet_pton(AF_INET, host, &a) != 1)) {
        snprintf(out, cap, "{\"error\":\"host must be an IPv4 address\"}"); return 400;
    }
    if (port <= 0) port = 8009;
    if (port > 65535) { snprintf(out, cap, "{\"error\":\"bad port\"}"); return 400; }
    if (start_job(kind, host, port) != 0) { snprintf(out, cap, "{\"error\":\"busy, try again in a few seconds\"}"); return 503; }
    snprintf(out, cap, "{\"started\":true}");
    return 202;
}
