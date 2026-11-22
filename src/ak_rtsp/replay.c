#include "replay.h"
#include "recidx.h"
#include "record.h"
#include "fmp4r.h"
#include "onvif_g.h"
#include "monotime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include "mbedtls/base64.h"

#define MAX_FRAME   (512 * 1024)      /* as the live hub (LIVE_MAX_FRAME) */
#define MAX_PAYLOAD 1400              /* RTP payload per packet */
#define LEAD_US     150000            /* send this far ahead of real time */
#define SR_EVERY_US 5000000
#define NTP_OFFSET  2208988800u       /* 1900 -> 1970 */

typedef struct {
    int set, tcp, ch, rtcp_ch, fd, rtcp_fd;
    uint16_t seq;
    uint32_t ssrc, base, pkts, octets;
    int disc;                         /* next access unit follows a discontinuity (D) */
} track_t;

struct replay {
    char day[11], token[RECIDX_TOKEN_MAX];
    int single;                       /* file token: npt is the file's media time */
    recfile_t *files;
    int nfiles, cur;
    fmp4r_t rd;
    int rd_open;
    size_t si;                        /* next sample in rd.s */
    int has_audio;
    int64_t dur_ms;                   /* npt length announced in the SDP */
    track_t tr[2];
    /* play state */
    int playing, started, ext, intra, norate, need_key, new_file, gap_before, waiting;
    double scale;
    uint8_t cseq;
    int64_t wall0, ts0;               /* pacing anchor: mono us <-> session us */
    int64_t rtp_off;                  /* file media us -> session us */
    int64_t last_end;                 /* session us just after the last sent sample */
    int64_t end_abs_ms;               /* Range end, 0 none */
    int64_t ntp0_us;                  /* wall clock of session time 0 (RTCP SR) */
    int64_t next_sr;
    uint8_t *frame;
    size_t frame_cap;
    uint8_t pkt[4 + 12 + 16 + MAX_PAYLOAD + 16];
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_sessions;

int replay_sessions(void) { return g_sessions; }

static uint32_t rnd32(void)
{
    uint32_t v = 0;
    if (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v) v = (uint32_t)rand() ^ (uint32_t)mono_us();
    return v;
}

/* ITU-T G.711 u-law (as rtsp.c) */
static uint8_t ulaw(int16_t pcm)
{
    const int BIAS = 0x84, CLIP = 32635;
    int sign = (pcm >> 8) & 0x80, s = pcm;
    if (sign) s = -s;
    if (s > CLIP) s = CLIP;
    s += BIAS;
    int exp = 7;
    for (int mask = 0x4000; !(s & mask) && exp > 0; mask >>= 1) exp--;
    int mant = (s >> (exp + 3)) & 0x0f;
    return (uint8_t)~(sign | (exp << 4) | mant);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* ---- URI / playlist ------------------------------------------------------------------ */
static const char *uri_path(const char *uri)
{
    const char *p = strstr(uri, "://");
    if (p) { p = strchr(p + 3, '/'); return p ? p : ""; }
    return uri;
}

int replay_is_uri(const char *uri)
{
    return uri && !strncmp(uri_path(uri), "/replay/", 8);
}

/* token from /replay/<token>[/][?...] (DESCRIBE: nothing else may follow) */
static int uri_token(const char *uri, char *out, size_t cap)
{
    const char *p = uri_path(uri);
    if (strncmp(p, "/replay/", 8)) return -1;
    p += 8;
    size_t n = strcspn(p, "/?");
    if (!n || n >= cap) return -1;
    const char *rest = p + n + (p[n] == '/');
    if (*rest && *rest != '?') return -1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

static int open_file(replay_t *r, int i)
{
    if (r->rd_open) { fmp4r_close(&r->rd); r->rd_open = 0; }
    char path[300];
    if (i < 0 || i >= r->nfiles || record_path(r->day, r->files[i].name, path, sizeof path) != 0) return -1;
    if (fmp4r_open(&r->rd, path) != 0) return -1;
    r->rd_open = 1;
    r->cur = i;
    r->si = 0;
    return 0;
}

/* first file from i on that opens; -1 none */
static int open_from(replay_t *r, int i)
{
    for (; i < r->nfiles; i++) if (open_file(r, i) == 0) return i;
    return -1;
}

/* newer files of the day (the recorder went on to the next segment) */
static void refresh_playlist(replay_t *r)
{
    if (r->single || !r->nfiles) return;
    recfile_t *f = malloc(RECIDX_MAX_FILES * sizeof *f);
    if (!f) return;
    int n = recidx_files(r->day, f, RECIDX_MAX_FILES), add = 0;
    for (int i = 0; i < n; i++) if (strcmp(f[i].name, r->files[r->nfiles - 1].name) > 0) add++;
    if (add) {
        recfile_t *nf = realloc(r->files, (size_t)(r->nfiles + add) * sizeof *nf);
        if (nf) {
            r->files = nf;
            for (int i = 0; i < n; i++)
                if (strcmp(f[i].name, r->files[r->nfiles - 1].name) > 0) r->files[r->nfiles++] = f[i];
        }
    }
    free(f);
}

static void b64(const uint8_t *d, size_t n, char *out, size_t cap)
{
    size_t ol = 0;
    if (mbedtls_base64_encode((unsigned char *)out, cap, &ol, d, n) != 0) ol = 0;
    out[ol < cap ? ol : cap - 1] = 0;
}

replay_t *replay_open(const char *uri, char *sdp, size_t cap, int *status, const char **reason)
{
    char token[RECIDX_TOKEN_MAX], day[11], name[44];
    *status = 404;
    if (!onvif_g_enabled()) { *reason = "replay is off (onvif.replay)"; return NULL; }
    if (uri_token(uri, token, sizeof token) || recidx_parse_token(token, day, name)) { *reason = "unknown recording"; return NULL; }
    replay_t *r = calloc(1, sizeof *r);
    if (!r) { *status = 500; *reason = "out of memory"; return NULL; }
    r->rd.fd = -1;
    snprintf(r->day, sizeof r->day, "%s", day);
    snprintf(r->token, sizeof r->token, "%s", token);
    if (name[0]) {
        r->single = 1;
        r->files = malloc(sizeof *r->files);
        if (r->files && recidx_file(day, name, r->files) == 0) r->nfiles = 1;
    } else {
        recfile_t *f = malloc(RECIDX_MAX_FILES * sizeof *f);
        int n = f ? recidx_files(day, f, RECIDX_MAX_FILES) : 0;
        if (n > 0) { r->files = realloc(f, (size_t)n * sizeof *f); if (!r->files) r->files = f; r->nfiles = n; }
        else free(f);
    }
    if (!r->nfiles) { free(r->files); free(r); *reason = "unknown recording"; return NULL; }

    pthread_mutex_lock(&g_lock);
    int ok = g_sessions < REPLAY_MAX_SESSIONS;
    if (ok) g_sessions++;
    pthread_mutex_unlock(&g_lock);
    if (!ok) { free(r->files); free(r); *status = 503; *reason = "too many replay sessions"; return NULL; }

    if (open_from(r, 0) < 0) {
        *status = 415; *reason = "no playable recording";
        replay_close(r);
        return NULL;
    }
    r->has_audio = r->rd.aid && r->rd.arate == 8000;
    if (r->single) {
        int64_t d = r->rd.duration_us;
        if (d <= 0) d = fmp4r_scan_end(&r->rd);
        r->dur_ms = d / 1000;
    } else {
        r->dur_ms = ((int64_t)r->files[r->nfiles - 1].end - r->files[0].start) * 1000;
    }
    if (r->dur_ms < 0) r->dur_ms = 0;
    for (int k = 0; k < 2; k++) { r->tr[k].fd = r->tr[k].rtcp_fd = -1; r->tr[k].ssrc = rnd32(); r->tr[k].base = rnd32(); r->tr[k].seq = (uint16_t)rnd32(); }
    r->scale = 1.0;

    char s64[400], p64[400];
    b64(r->rd.sps, r->rd.sps_len, s64, sizeof s64);
    b64(r->rd.pps, r->rd.pps_len, p64, sizeof p64);
    char start[24];
    recidx_iso(r->files[0].start, start);
    int n = snprintf(sdp, cap,
        "v=0\r\no=- %u 1 IN IP4 0.0.0.0\r\ns=ak_rtsp replay %s\r\nt=0 0\r\n"
        "a=range:npt=0-%lld.%03d\r\n"
        "m=video 0 RTP/AVP 96\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s\r\n"
        "a=control:streamid=0\r\na=recvonly\r\n",
        rnd32() & 0x7fffffff, r->token, (long long)(r->dur_ms / 1000), (int)(r->dur_ms % 1000),
        r->rd.sps[1], r->rd.sps[2], r->rd.sps[3], s64, p64);
    if (r->has_audio && n > 0 && (size_t)n < cap)
        snprintf(sdp + n, cap - (size_t)n, "m=audio 0 RTP/AVP 0\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:0 PCMU/8000\r\n"
                                           "a=control:streamid=1\r\na=recvonly\r\n");
    printf("[replay] %s: %d file(s) from %s, %lld s%s\n", r->token, r->nfiles, start, (long long)(r->dur_ms / 1000),
           r->has_audio ? ", audio" : "");
    return r;
}

void replay_set_track(replay_t *r, int track, int tcp, int rtp_ch, int rtcp_ch, int rtp_fd, int rtcp_fd)
{
    if (!r || track < 0 || track > 1) return;
    track_t *t = &r->tr[track];
    t->set = 1; t->tcp = tcp; t->ch = rtp_ch; t->rtcp_ch = rtcp_ch;
    t->fd = tcp ? -1 : rtp_fd;
    t->rtcp_fd = tcp ? -1 : rtcp_fd;
}

void replay_close(replay_t *r)
{
    if (!r) return;
    if (r->rd_open) fmp4r_close(&r->rd);
    free(r->files);
    free(r->frame);
    free(r);
    pthread_mutex_lock(&g_lock);
    g_sessions--;
    pthread_mutex_unlock(&g_lock);
}

/* ---- sending ------------------------------------------------------------------------ */
enum { F_C = 0x80, F_E = 0x40, F_D = 0x20, F_T = 0x10 };

static uint32_t rtp_ts(const replay_t *r, int k, int64_t session_us)
{
    uint64_t u = session_us > 0 ? (uint64_t)session_us : 0;
    return r->tr[k].base + (uint32_t)(k ? u * 8 / 1000 : u * 9 / 100);
}

static int out(replay_t *r, conn_t *cn, int tcp, int ch, int fd, size_t len)
{
    if (tcp) {
        r->pkt[0] = '$'; r->pkt[1] = (uint8_t)ch; r->pkt[2] = (uint8_t)(len >> 8); r->pkt[3] = (uint8_t)len;
        return conn_send_all(cn, r->pkt, len + 4) == 0 ? 0 : -1;
    }
    if (fd >= 0) (void)!send(fd, r->pkt + 4, len, MSG_NOSIGNAL);    /* UDP: a lost datagram is not fatal */
    return 0;
}

static int rtp(replay_t *r, conn_t *cn, int k, uint32_t ts, int marker, const uint8_t *h, size_t hl,
               const uint8_t *p, size_t pl, uint8_t flags, int64_t abs_ms)
{
    track_t *t = &r->tr[k];
    uint8_t *b = r->pkt + 4;
    size_t o = 0;
    b[o++] = (uint8_t)(0x80 | (r->ext ? 0x10 : 0));
    b[o++] = (uint8_t)((marker ? 0x80 : 0) | (k ? 0 : 96));
    b[o++] = (uint8_t)(t->seq >> 8); b[o++] = (uint8_t)t->seq;
    put32(b + o, ts); o += 4;
    put32(b + o, t->ssrc); o += 4;
    if (r->ext) {                                /* ONVIF replay extension */
        b[o++] = 0xAB; b[o++] = 0xAC; b[o++] = 0; b[o++] = 3;
        int64_t ms = abs_ms > 0 ? abs_ms : 0;
        put32(b + o, (uint32_t)(ms / 1000) + NTP_OFFSET); o += 4;
        put32(b + o, (uint32_t)(((uint64_t)(ms % 1000) << 32) / 1000)); o += 4;
        b[o++] = flags; b[o++] = r->cseq; b[o++] = 0; b[o++] = 0;
    }
    if (hl) { memcpy(b + o, h, hl); o += hl; }
    if (pl) { memcpy(b + o, p, pl); o += pl; }
    t->seq++; t->pkts++; t->octets += (uint32_t)(hl + pl);
    return out(r, cn, t->tcp, t->ch, t->fd, o);
}

/* RTCP sender report (+ BYE) for a track, at session time now_us */
static int rtcp(replay_t *r, conn_t *cn, int k, int64_t now_us, int bye)
{
    track_t *t = &r->tr[k];
    if (!t->set) return 0;
    uint8_t *b = r->pkt + 4;
    int64_t ntp_us = r->ntp0_us + (now_us > 0 ? now_us : 0);
    b[0] = 0x80; b[1] = 200; b[2] = 0; b[3] = 6;
    put32(b + 4, t->ssrc);
    put32(b + 8, (uint32_t)(ntp_us / 1000000) + NTP_OFFSET);
    put32(b + 12, (uint32_t)(((uint64_t)(ntp_us % 1000000) << 32) / 1000000));
    put32(b + 16, rtp_ts(r, k, now_us));
    put32(b + 20, t->pkts);
    put32(b + 24, t->octets);
    size_t len = 28;
    if (bye) { b[28] = 0x81; b[29] = 203; b[30] = 0; b[31] = 1; put32(b + 32, t->ssrc); len = 36; }
    return out(r, cn, t->tcp, t->rtcp_ch, t->rtcp_fd, len);
}

static int send_nal(replay_t *r, conn_t *cn, uint32_t ts, const uint8_t *nal, size_t len, int marker, uint8_t flags, int64_t abs_ms)
{
    if (!len) return 0;
    if (len <= MAX_PAYLOAD) return rtp(r, cn, 0, ts, marker, NULL, 0, nal, len, flags, abs_ms);
    uint8_t fu[2];
    fu[0] = (uint8_t)((nal[0] & 0xE0) | 28);
    const uint8_t *p = nal + 1;
    size_t rem = len - 1;
    int first = 1;
    while (rem) {
        size_t c = rem > MAX_PAYLOAD - 2 ? MAX_PAYLOAD - 2 : rem;
        fu[1] = (uint8_t)((nal[0] & 0x1F) | (first ? 0x80 : 0) | (c == rem ? 0x40 : 0));
        if (rtp(r, cn, 0, ts, marker && c == rem, fu, 2, p, c, flags, abs_ms)) return -1;
        p += c; rem -= c; first = 0;
    }
    return 0;
}

static int send_video(replay_t *r, conn_t *cn, const fmp4r_sample_t *s, const uint8_t *d, size_t n, uint32_t ts,
                      uint8_t flags, int64_t abs_ms)
{
    if (s->key && (send_nal(r, cn, ts, r->rd.sps, r->rd.sps_len, 0, flags, abs_ms) ||
                   send_nal(r, cn, ts, r->rd.pps, r->rd.pps_len, 0, flags, abs_ms)))
        return -1;
    size_t nl = r->rd.nal_len, o = 0;
    const uint8_t *prev = NULL;
    size_t prev_len = 0;
    while (o + nl <= n) {
        size_t l = 0;
        for (size_t i = 0; i < nl; i++) l = l << 8 | d[o + i];
        o += nl;
        if (l > n - o) break;                    /* broken length: the rest of the sample is dropped */
        const uint8_t *nal = d + o;
        o += l;
        if (!l || (nal[0] & 0x1F) == 9) continue;    /* empty, access unit delimiter */
        if (prev && send_nal(r, cn, ts, prev, prev_len, 0, flags, abs_ms)) return -1;
        prev = nal; prev_len = l;
    }
    return prev ? send_nal(r, cn, ts, prev, prev_len, 1, flags, abs_ms) : 0;
}

static int send_audio(replay_t *r, conn_t *cn, const uint8_t *d, size_t n, int64_t session_us, uint8_t flags, int64_t abs_ms)
{
    uint8_t u[1024];
    size_t ns = n / 2, done = 0;
    while (done < ns) {
        size_t c = ns - done > sizeof u ? sizeof u : ns - done;
        for (size_t i = 0; i < c; i++) {
            const uint8_t *q = d + (done + i) * 2;
            u[i] = ulaw((int16_t)(r->rd.audio_be ? (q[0] << 8 | q[1]) : (q[1] << 8 | q[0])));
        }
        uint32_t ts = rtp_ts(r, 1, session_us) + (uint32_t)done;
        if (rtp(r, cn, 1, ts, 1, NULL, 0, u, c, flags, abs_ms + (int64_t)done / 8)) return -1;
        done += c;
    }
    return 0;
}

/* abs time (ms) of a sample of the current file */
static int64_t abs_ms(const replay_t *r, const fmp4r_sample_t *s)
{
    return (int64_t)r->files[r->cur].start * 1000 + s->t_us / 1000;
}

/* Next sample to send (loads fragments, moves through the playlist), NULL at
 * the end or (r->waiting) while the file being recorded has no new data yet. */
static const fmp4r_sample_t *peek(replay_t *r)
{
    r->waiting = 0;
    for (int guard = 0; guard < 100000 && r->rd_open; guard++) {
        if (r->si < r->rd.ns) {
            const fmp4r_sample_t *s = &r->rd.s[r->si];
            int drop = s->audio ? (!r->has_audio || r->intra || r->need_key || r->rd.arate != 8000)
                                : ((r->need_key || r->intra) && !s->key);
            if (drop) { r->si++; continue; }
            if (!s->audio) r->need_key = 0;
            if (r->new_file) {                   /* session time runs on across files (gaps squeezed) */
                r->rtp_off = r->last_end - s->t_us;
                r->new_file = 0;
            }
            return s;
        }
        if (fmp4r_next(&r->rd) == 1) { r->si = 0; continue; }
        if (record_is_live(r->day, r->files[r->cur].name)) { r->waiting = 1; return NULL; }
        if (r->cur + 1 >= r->nfiles) refresh_playlist(r);
        int prev = r->cur;
        int64_t prev_end = (int64_t)r->files[prev].start * 1000 + (r->last_end - r->rtp_off) / 1000;
        if (open_from(r, prev + 1) < 0) return NULL;
        r->new_file = 1;
        r->need_key = 1;
        if ((int64_t)r->files[r->cur].start * 1000 - prev_end > 2000) r->tr[0].disc = r->tr[1].disc = 1;
    }
    return NULL;
}

/* this sample is its track's last one before a gap (E) or the end (T)? */
static uint8_t end_flags(replay_t *r, const fmp4r_sample_t *s)
{
    uint8_t f = 0;
    if (r->end_abs_ms && abs_ms(r, s) + s->dur_us / 1000 >= r->end_abs_ms) return F_E | F_T;
    if ((int)r->si != r->rd.last_idx[s->audio] || record_is_live(r->day, r->files[r->cur].name) || fmp4r_more(&r->rd)) return 0;
    if (r->cur + 1 >= r->nfiles) return F_E | F_T;
    int64_t end = abs_ms(r, s) + s->dur_us / 1000;
    if ((int64_t)r->files[r->cur + 1].start * 1000 - end > 2000) f = F_E;
    return f;
}

static int finish(replay_t *r, conn_t *cn)
{
    int rc = 0;
    for (int k = 0; k < 2; k++) if (rtcp(r, cn, k, r->last_end, 1)) rc = -1;
    r->playing = 0;
    printf("[replay] %s: end of recording\n", r->token);
    return rc ? -1 : 1;
}

int replay_step(replay_t *r, conn_t *cn)
{
    if (!r || !r->playing) return 0;
    int64_t now = mono_us();
    if (r->started && now >= r->next_sr) {
        r->next_sr = now + SR_EVERY_US;
        for (int k = 0; k < 2; k++) if (rtcp(r, cn, k, r->last_end, 0)) return -1;
    }
    for (int n = 0; n < 64; n++) {
        const fmp4r_sample_t *s = peek(r);
        if (!s) {
            if (r->waiting) { usleep(100000); return 0; }
            return finish(r, cn);
        }
        int64_t sess = r->rtp_off + s->t_us;
        if (!r->started) {                       /* first sample after PLAY: anchor pacing, SR for sync */
            r->started = 1;
            r->wall0 = now; r->ts0 = sess;
            for (int k = 0; k < 2; k++) if (rtcp(r, cn, k, sess, 0)) return -1;
            r->next_sr = now + SR_EVERY_US;
        }
        if (!r->norate) {
            int64_t due = r->wall0 + (int64_t)((double)(sess - r->ts0) / r->scale);
            if (due - now > LEAD_US) {
                int64_t w = due - now - LEAD_US;
                usleep((useconds_t)(w > 20000 ? 20000 : w));
                return 0;
            }
        }
        int64_t am = abs_ms(r, s);
        if (r->end_abs_ms && am > r->end_abs_ms) return finish(r, cn);
        uint8_t flags = end_flags(r, s);
        if (!s->audio && s->key) flags |= F_C;
        if (r->tr[s->audio].disc) { flags |= F_D; r->tr[s->audio].disc = 0; }
        if (r->tr[s->audio].set) {
            size_t want = s->size;
            if (want <= (s->audio ? 65536u : MAX_FRAME) && want > r->frame_cap) {
                uint8_t *nb = realloc(r->frame, want);
                if (nb) { r->frame = nb; r->frame_cap = want; }
            }
            int len = r->frame ? fmp4r_read(&r->rd, s, r->frame, r->frame_cap) : -1;
            if (len > 0) {
                int rc = s->audio ? send_audio(r, cn, r->frame, (size_t)len, sess, flags, am)
                                  : send_video(r, cn, s, r->frame, (size_t)len, rtp_ts(r, 0, sess), flags, am);
                if (rc) return -1;
            }
        }
        if (sess + s->dur_us > r->last_end) r->last_end = sess + s->dur_us;
        r->si++;
    }
    return 0;
}

/* ---- PLAY ------------------------------------------------------------------------- */
/* npt value: seconds[.frac] or h:mm:ss[.frac] -> ms; -1 bad */
static int64_t npt_ms(const char *s, const char **end)
{
    int64_t v = 0, parts[3] = { 0, 0, 0 };
    int np = 0;
    const char *p = s;
    for (;;) {
        if (!isdigit((unsigned char)*p)) return -1;
        v = 0;
        while (isdigit((unsigned char)*p)) { if (v < 100000000) v = v * 10 + (*p - '0'); p++; }
        parts[np++] = v;
        if (*p == ':' && np < 3) { p++; continue; }
        break;
    }
    int64_t sec = np == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : np == 2 ? parts[0] * 60 + parts[1] : parts[0];
    int64_t ms = sec * 1000;
    if (*p == '.') {
        p++;
        int k = 0, f = 0;
        while (isdigit((unsigned char)*p)) { if (k < 3) { f = f * 10 + (*p - '0'); k++; } p++; }
        while (k < 3) { f *= 10; k++; }
        ms += f;
    }
    *end = p;
    return ms;
}

static void clock_str(int64_t ms, char *out, size_t cap)
{
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    if (!gmtime_r(&t, &tm)) { snprintf(out, cap, "19700101T000000Z"); return; }
    snprintf(out, cap, "%04d%02d%02dT%02d%02d%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ms % 1000));
}

/* position on absolute time (ms): the file holding it, else the next one; -1 past the end */
static int seek_abs(replay_t *r, int64_t t)
{
    int i = 0;
    while (i < r->nfiles && ((int64_t)r->files[i].end + 1) * 1000 <= t && !r->files[i].live) i++;
    if (i >= r->nfiles) return -1;
    if (open_from(r, i) < 0) return -1;
    int64_t local = t - (int64_t)r->files[r->cur].start * 1000;
    fmp4r_seek(&r->rd, local > 0 ? local * 1000 : 0);
    return 0;
}

int replay_play(replay_t *r, const char *range, const char *scale, const char *frames, const char *rate_control,
                const char *require, unsigned cseq, const char *url,
                char *range_out, size_t rcap, char *rtpinfo, size_t icap, char *scale_out, size_t scap,
                const char **reason)
{
    range_out[0] = rtpinfo[0] = scale_out[0] = 0;
    if (!r->tr[0].set) { *reason = "SETUP the video track first"; return 455; }
    double sc = 1.0;
    if (scale && *scale) {
        char *e;
        sc = strtod(scale, &e);
        if (e == scale || !(sc > 0.0) || sc > 16.0) { *reason = "Scale must be > 0 and <= 16 (no reverse playback)"; return 456; }
    }
    if (require && strstr(require, "onvif-replay")) r->ext = 1;
    int intra = sc > 1.0 || (frames && !strncmp(frames, "intra", 5));
    int norate = rate_control && !strncmp(rate_control, "no", 2);

    int seek = !r->started && !r->playing && r->si == 0;    /* first PLAY: from the start */
    int clock = 0;
    int64_t from = 0, to = 0;                     /* abs ms (clock) or npt ms */
    if (range && *range) {
        const char *p = range, *e;
        while (*p == ' ') p++;
        if (!strncmp(p, "npt=", 4)) {
            p += 4;
            if (!strncmp(p, "now", 3)) { from = 0; p += 3; }
            else if ((from = npt_ms(p, &e)) < 0) { *reason = "bad npt range"; return 457; }
            else p = e;
            if (*p++ != '-') { *reason = "bad npt range"; return 457; }
            if (isdigit((unsigned char)*p) && (to = npt_ms(p, &e)) < 0) { *reason = "bad npt range"; return 457; }
        } else if (!strncmp(p, "clock=", 6)) {
            p += 6;
            clock = 1;
            from = recidx_parse_time_ms(p);
            const char *t = strchr(p, 'T'), *dash = t ? strchr(t, '-') : NULL;
            if (from < 0 || !dash) { *reason = "bad clock range"; return 457; }
            if (dash[1] && dash[1] != ';' && (to = recidx_parse_time_ms(dash + 1)) < 0) { *reason = "bad clock range"; return 457; }
        } else {
            *reason = "Range must be npt= or clock="; return 457;
        }
        seek = 1;
    }
    int64_t first_ms = (int64_t)r->files[0].start * 1000;
    int64_t abs_from = clock ? from : first_ms + from;   /* day: npt counts from the first file */
    r->end_abs_ms = to ? (clock ? to : first_ms + to) : 0;
    if (seek) {
        int rc;
        if (r->single && !clock) {               /* file token: npt is the file's own media time */
            rc = open_from(r, 0) < 0 ? -1 : 0;
            if (!rc) fmp4r_seek(&r->rd, from * 1000);
            if (!rc && r->dur_ms && from > r->dur_ms) rc = -1;
        } else {
            rc = seek_abs(r, abs_from);
        }
        if (rc) { *reason = "range is past the end of the recording"; return 457; }
        r->need_key = 1;
        r->tr[0].disc = r->tr[1].disc = 1;
        if (r->started) r->new_file = 1;          /* session time continues after the jump */
    }
    r->cseq = (uint8_t)cseq;
    r->scale = sc; r->intra = intra; r->norate = norate;
    if (!r->started && !r->ntp0_us) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        r->ntp0_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    }

    /* where playing starts: first sample to be sent */
    const fmp4r_sample_t *s = peek(r);
    int64_t sess = s ? r->rtp_off + s->t_us : r->last_end;
    if (s && !r->started && seek) { r->rtp_off = -s->t_us; sess = 0; }
    int64_t start_abs = s ? abs_ms(r, s) : abs_from;
    if (r->started) { r->wall0 = mono_us(); r->ts0 = sess; }   /* re-anchor pacing (seek / resume) */
    r->playing = 1;

    if (clock) {
        char a[40], b[40] = "";
        clock_str(start_abs, a, sizeof a);
        if (r->end_abs_ms) clock_str(r->end_abs_ms, b, sizeof b);
        snprintf(range_out, rcap, "clock=%s-%s", a, b);
    } else {
        int64_t npt = r->single ? (s ? s->t_us / 1000 : from) : start_abs - first_ms;
        if (npt < 0) npt = 0;
        int64_t end = r->end_abs_ms ? (r->single ? to : r->end_abs_ms - first_ms) : r->dur_ms;
        snprintf(range_out, rcap, "npt=%lld.%03d-%lld.%03d", (long long)(npt / 1000), (int)(npt % 1000),
                 (long long)(end / 1000), (int)(end % 1000));
    }
    if (scale && *scale) snprintf(scale_out, scap, "%.2f", sc);
    char base[300];
    snprintf(base, sizeof base, "%s", url ? url : "");
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') base[--bl] = 0;
    int n = snprintf(rtpinfo, icap, "url=%s/streamid=0;seq=%u;rtptime=%u", base, r->tr[0].seq, rtp_ts(r, 0, sess));
    if (r->tr[1].set && n > 0 && (size_t)n < icap)
        snprintf(rtpinfo + n, icap - (size_t)n, ",url=%s/streamid=1;seq=%u;rtptime=%u", base, r->tr[1].seq, rtp_ts(r, 1, sess));
    printf("[replay] %s: PLAY %s%s%s\n", r->token, range_out, r->ext ? " (onvif-replay)" : "", intra ? " (I-frames)" : "");
    return 200;
}

void replay_pause(replay_t *r)
{
    if (r) r->playing = 0;
}
