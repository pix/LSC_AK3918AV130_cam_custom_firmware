#include "live.h"
#include "venc.h"
#include "audio.h"
#include "monotime.h"
#include "speaker.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#define RING     64                    /* ~4 s at 15 fps */
#define MAX_FRAME LIVE_MAX_FRAME

typedef struct { uint8_t *data; size_t len; int key; uint64_t ts; uint32_t seq; } slot_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static slot_t   g_ring[RING];
static uint32_t g_next_seq = 1;        /* seq of the next frame to be pushed */
static int      g_subs;                /* subscribers: web viewers + RTSP clients */
static int      g_viewers;             /* web (WebSocket) viewers only */

/* audio: small ring of capture periods (256 samples = 32 ms each) */
#define ARING 64
#define APER  LIVE_APER
typedef struct { int16_t pcm[APER]; size_t n; uint64_t ts; uint32_t seq; } aslot_t;
static aslot_t   g_aring[ARING];
static uint32_t  g_anext = 1;

/* ---- producer ------------------------------------------------------------- */
void live_push_frame(const uint8_t *data, size_t len, int keyframe, uint64_t ts_ms)
{
    if (!g_subs || len == 0 || len > MAX_FRAME) return;   /* racy read is fine: cheap early-out */
    uint8_t *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    pthread_mutex_lock(&g_lock);
    slot_t *s = &g_ring[g_next_seq % RING];
    free(s->data);
    s->data = copy; s->len = len; s->key = keyframe; s->ts = ts_ms; s->seq = g_next_seq;
    g_next_seq++;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

int live_viewers(void) { return g_viewers; }

static volatile int g_mute;
void live_set_audio_mute(int on) { g_mute = on; }

void live_push_audio(const int16_t *pcm, size_t n)
{
    if (!g_subs || n == 0) return;
    if (n > APER) n = APER;
    pthread_mutex_lock(&g_lock);
    aslot_t *a = &g_aring[g_anext % ARING];
    if (g_mute) memset(a->pcm, 0, n * sizeof(int16_t));
    else memcpy(a->pcm, pcm, n * sizeof(int16_t));
    a->n = n; a->ts = (uint64_t)mono_ms(); a->seq = g_anext++;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

/* Newest keyframe seq still in the ring, or 0. Caller holds g_lock. */
static uint32_t newest_key(void)
{
    for (uint32_t q = g_next_seq - 1; q > 0 && g_next_seq - q <= RING; q--) {
        slot_t *s = &g_ring[q % RING];
        if (s->seq == q && s->key) return q;
    }
    return 0;
}

/* ---- consumers --------------------------------------------------------------- */
int live_subscribe(void)
{
    pthread_mutex_lock(&g_lock);
    g_subs++;
    pthread_mutex_unlock(&g_lock);
    venc_request_idr();          /* smart encoding: no waiting for a 20 s GOP */
    return 0;
}

void live_unsubscribe(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_subs > 0) g_subs--;
    if (g_subs == 0) {                           /* nobody watching: free the ring */
        for (int i = 0; i < RING; i++) { free(g_ring[i].data); memset(&g_ring[i], 0, sizeof(g_ring[i])); }
    }
    pthread_mutex_unlock(&g_lock);
}

int live_next(live_cursor_t *c, uint8_t *buf, size_t cap, live_item_t *it, int timeout_ms)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&g_lock);
    for (;;) {
        /* video: start at, and after falling too far behind skip to, the
         * newest keyframe still in the ring */
        if (c->want == 0 || g_next_seq - c->want > RING - 2) {
            uint32_t k = newest_key();
            if (k && k >= c->want) c->want = k;
            else if (c->want == 0 || g_next_seq - c->want > RING - 2) c->want = 0;
        }
        /* audio: only once video is synced, so both start together */
        if (!c->want || c->awant == 0 || g_anext - c->awant > ARING - 2) c->awant = g_anext;
        if (c->want && c->awant < g_anext) {
            uint32_t q = c->awant++;
            aslot_t *a = &g_aring[q % ARING];
            if (a->seq != q) continue;
            memcpy(it->pcm, a->pcm, a->n * sizeof(int16_t));
            it->n = a->n; it->ts = a->ts; it->key = 0; it->len = 0;
            pthread_mutex_unlock(&g_lock);
            return LIVE_AUDIO;
        }
        if (c->want && c->want < g_next_seq) {
            slot_t *s = &g_ring[c->want % RING];
            if (s->seq != c->want) { c->want = 0; continue; }
            c->want++;
            if (s->len > cap) continue;          /* can't happen with MAX_FRAME buffers */
            memcpy(buf, s->data, s->len);
            it->len = s->len; it->key = s->key; it->ts = s->ts; it->n = 0;
            pthread_mutex_unlock(&g_lock);
            return LIVE_VIDEO;
        }
        if (pthread_cond_timedwait(&g_cond, &g_lock, &dl) == ETIMEDOUT) {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }
}

/* Rewind a cursor to the earliest keyframe at most back_ms older than the
 * newest frame (pre-roll for recordings), and its audio to the matching
 * capture time. Returns 0, or -1 if the ring holds no keyframe yet. */
int live_rewind(live_cursor_t *c, uint32_t back_ms)
{
    pthread_mutex_lock(&g_lock);
    uint32_t newest = g_next_seq - 1, best = 0;
    slot_t *ns = &g_ring[newest % RING];
    if (newest && ns->seq == newest && ns->data) {
        for (uint32_t q = newest; q > 0 && g_next_seq - q <= RING - 2; q--) {
            slot_t *s = &g_ring[q % RING];
            if (s->seq != q || !s->data) break;
            if (ns->ts - s->ts > back_ms) break;
            if (s->key) best = q;
        }
    }
    if (best) {
        c->want = best;
        /* audio: the oldest period captured after (now - age of that keyframe) */
        uint64_t age = ns->ts - g_ring[best % RING].ts, from = (uint64_t)mono_ms() - age;
        uint32_t a = g_anext;
        for (uint32_t q = g_anext - 1; q > 0 && g_anext - q <= ARING - 2; q--) {
            aslot_t *x = &g_aring[q % ARING];
            if (x->seq != q || x->ts < from) break;
            a = q;
        }
        c->awant = a;
    }
    pthread_mutex_unlock(&g_lock);
    return best ? 0 : -1;
}

/* ---- tokens ---------------------------------------------------------------- */
static struct { char tok[33]; uint32_t until; } g_tok[4];

int live_token_new(char out[33])
{
    uint8_t r[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, r, sizeof(r));
    close(fd);
    if (n != (ssize_t)sizeof(r)) return -1;
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", r[i]);
    pthread_mutex_lock(&g_lock);
    int slot = 0;
    for (int i = 1; i < 4; i++) if (g_tok[i].until < g_tok[slot].until) slot = i;   /* oldest */
    memcpy(g_tok[slot].tok, out, 33);
    g_tok[slot].until = mono_s() + 60;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int live_token_take(const char *tok)
{
    if (!tok || strlen(tok) != 32) return 0;
    int ok = 0;
    uint32_t now = mono_s();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 4; i++) {
        unsigned char d = 0;
        for (int k = 0; k < 32; k++) d |= (unsigned char)(g_tok[i].tok[k] ^ tok[k]);
        if (!d && g_tok[i].tok[0] && now <= g_tok[i].until) {
            ok = 1;
            memset(&g_tok[i], 0, sizeof(g_tok[i]));   /* single use */
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ---- WebSocket viewer ------------------------------------------------------- */

/* One binary message: [flags:1][ts_ms:8 BE][payload].
 * flags bit0 = video keyframe, bit1 = audio (payload = s16le PCM, 8 kHz mono),
 * otherwise payload = Annex-B H.264 frame. */
static int ws_send_msg(conn_t *cn, const uint8_t *data, size_t len, int flags, uint64_t ts);
static int ws_send_frame(conn_t *cn, const uint8_t *data, size_t len, int key, uint64_t ts)
{
    return ws_send_msg(cn, data, len, key ? 1 : 0, ts);
}
static int ws_send_msg(conn_t *cn, const uint8_t *data, size_t len, int flags, uint64_t ts)
{
    uint8_t h[14 + 9];
    size_t plen = len + 9, hl = 0;
    h[hl++] = 0x82;                                  /* FIN + binary */
    if (plen < 126) h[hl++] = (uint8_t)plen;
    else if (plen <= 0xffff) { h[hl++] = 126; h[hl++] = (uint8_t)(plen >> 8); h[hl++] = (uint8_t)plen; }
    else { h[hl++] = 127; for (int i = 7; i >= 0; i--) h[hl++] = (uint8_t)((uint64_t)plen >> (8 * i)); }
    h[hl++] = (uint8_t)flags;
    for (int i = 7; i >= 0; i--) h[hl++] = (uint8_t)(ts >> (8 * i));
    if (conn_send_all(cn, h, hl) != 0) return -1;
    return conn_send_all(cn, data, len);
}

/* Client -> camera WebSocket frames (non-blocking). Binary frames carry
 * talk-back audio: 16-bit LE mono PCM at 8 kHz, played on the speaker.
 * Returns 1 when the client is gone or misbehaves, 0 otherwise. */
typedef struct { uint8_t b[20480]; size_t n; uint64_t talk_samples; int64_t talk_last_ms; double talk_sq; int talk_peak; } rx_t;

static int client_rx(conn_t *cn, rx_t *rx)
{
    if (!conn_readable(cn, 0)) return 0;
    if (rx->n == sizeof(rx->b)) return 1;
    ssize_t n = conn_recv_nb(cn, rx->b + rx->n, sizeof(rx->b) - rx->n);
    if (n == -2) return 0;                             /* partial TLS record */
    if (n <= 0) return 1;
    rx->n += (size_t)n;
    for (;;) {
        if (rx->n < 2) return 0;
        uint8_t op = rx->b[0] & 0x0f;
        int masked = rx->b[1] & 0x80;
        uint64_t len = rx->b[1] & 0x7f;
        size_t h = 2;
        if (len == 126) { if (rx->n < 4) return 0; len = (uint64_t)rx->b[2] << 8 | rx->b[3]; h = 4; }
        else if (len == 127) return 1;                 /* we never accept >64 KiB frames */
        if (!masked || len > 16384) return 1;           /* RFC 6455: clients must mask */
        if (rx->n < h + 4 + len) {
            if (h + 4 + len > sizeof(rx->b)) return 1;
            return 0;                                   /* wait for the rest */
        }
        uint8_t *m = rx->b + h, *d = rx->b + h + 4;
        for (uint64_t i = 0; i < len; i++) d[i] ^= m[i & 3];
        if (op == 0x8) return 1;                        /* close */
        if (op == 0x2 && len >= 2) {
            int64_t now = mono_ms();
            if (rx->talk_samples == 0 || now - rx->talk_last_ms > 2000)
                printf("[live] talk-back audio started (speaker %s)\n", speaker_ready() ? "ready" : "NOT ready");
            rx->talk_samples += (uint64_t)len / 2;
            for (uint64_t i = 0; i + 1 < len; i += 2) {
                int v = (int16_t)(d[i] | d[i + 1] << 8);
                rx->talk_sq += (double)v * v;
                if (v < 0) v = -v;
                if (v > rx->talk_peak) rx->talk_peak = v;
            }
            rx->talk_last_ms = now;
            if (speaker_ready())
                speaker_queue_stream((const int16_t *)d, (size_t)len / 2);   /* jitter buffer + latency cap */
        }
        size_t used = h + 4 + (size_t)len;
        memmove(rx->b, rx->b + used, rx->n - used);
        rx->n -= used;
    }
}

static void *viewer_thread(void *arg)
{
    conn_t *cn = arg;
    uint8_t *buf = malloc(LIVE_MAX_FRAME);
    rx_t *rx = calloc(1, sizeof(rx_t));
    live_item_t *it = calloc(1, sizeof(live_item_t));
    struct timeval tv = { 3, 0 };
    setsockopt(cn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    printf("[live] viewer connected (%d)\n", g_viewers);

    live_cursor_t cur = { 0, 0 };
    live_subscribe();
    while (buf && rx && it) {
        int k = live_next(&cur, buf, LIVE_MAX_FRAME, it, 200);
        if (k == LIVE_AUDIO) {
            if (ws_send_msg(cn, (const uint8_t *)it->pcm, it->n * sizeof(int16_t), 2, it->ts) != 0) break;
        } else if (k == LIVE_VIDEO) {
            if (ws_send_frame(cn, buf, it->len, it->key, it->ts) != 0) break;
        }
        if (client_rx(cn, rx)) break;
    }
    live_unsubscribe();
    if (rx && rx->talk_samples)
        printf("[live] talk-back: %.1f s of audio received from this viewer (peak %d, rms %.0f of 32767)\n",
               rx->talk_samples / 8000.0, rx->talk_peak, sqrt(rx->talk_sq / (double)rx->talk_samples));
    free(buf);
    free(rx);
    free(it);
    conn_close(cn);
    pthread_mutex_lock(&g_lock);
    g_viewers--;
    pthread_mutex_unlock(&g_lock);
    printf("[live] viewer disconnected (%d left)\n", g_viewers);
    return NULL;
}

int live_ws_start(conn_t *cn)
{
    pthread_mutex_lock(&g_lock);
    if (g_viewers >= LIVE_MAX_VIEWERS) { pthread_mutex_unlock(&g_lock); return -1; }
    g_viewers++;
    pthread_mutex_unlock(&g_lock);
    pthread_t t;
    if (pthread_create(&t, NULL, viewer_thread, cn) != 0) {
        pthread_mutex_lock(&g_lock); g_viewers--; pthread_mutex_unlock(&g_lock);
        return -1;
    }
    pthread_detach(t);
    return 0;
}
