/* Fuzz harness: hls.c request side (no network, no producer thread) and
 * cast.c's parsers (what a Cast device or an mDNS answer can send us).
 * Input byte 0 % 4 selects:
 *   0  the rest is a request path for hls_parse_path(); a parsed token is
 *      then checked against the token table (constant-time compare, expiry),
 *      as hls_serve() does. Invariant: a parse succeeds only for
 *      /hls/<32 lower-case hex>/{index.m3u8,init.mp4,seg<N>.m4s}.
 *   1  the rest drives the segment ring: records [size u16][ms u16][key u8]
 *      published into a live session, then the playlist is built.
 *      Invariants: <= HLS_RING segments, bytes within the budget unless only
 *      HLS_MIN_KEEP are left, consecutive numbers, the playlist fits and every
 *      rounded EXTINF <= TARGETDURATION.
 *   2  a CastMessage body: cast_decode(), then its payload through the JSON
 *      helpers (type, RECEIVER_STATUS app lookup, MEDIA_STATUS state); and an
 *      encode -> decode round trip of the input split into four strings.
 *   3  an mDNS answer for cast_parse_mdns(); names must come out sanitised.
 * Build: tests/fuzz/build_fuzzer.sh tests/fuzz/fuzz_hls.c <fuzz build dir> */
#include "hls.c"
#ifdef FUZZ_CAST              /* cast.c is only on the feat/hls branch (not merged): -DFUZZ_CAST there */
#include "cast.h"
#endif
#include <assert.h>

static int g_init_done;

static void setup(void)
{
    const char *e;
    hls_en_set("on", &e);
    char t[33];
    for (int i = 0; i < 4; i++) hls_token_new(t);
    g_init_done = 1;
}

static int is_hex32(const char *p)
{
    for (int i = 0; i < 32; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    return 1;
}

static void fuzz_path(const uint8_t *d, size_t n)
{
    char *s = malloc(n + 1);
    memcpy(s, d, n);
    s[n] = 0;
    char tok[33];
    uint32_t seq = 0;
    int k = hls_parse_path(s, tok, &seq);
    if (k != HLS_BAD) {
        assert(!strncmp(s, "/hls/", 5) && is_hex32(s + 5) && s[37] == '/' && strlen(tok) == 32);
        const char *f = s + 38;
        if (k == HLS_PLAYLIST) assert(!strncmp(f, "index.m3u8", 10) && (f[10] == 0 || f[10] == '?' || f[10] == '#'));
        if (k == HLS_INIT) assert(!strncmp(f, "init.mp4", 8) && (f[8] == 0 || f[8] == '?' || f[8] == '#'));
        if (k == HLS_SEGMENT) {
            char want[24];
            snprintf(want, sizeof want, "seg%u.m4s", seq);
            assert(!strncmp(f, want, strlen(want)));
        }
        pthread_mutex_lock(&g_lock);
        tok_check(tok, mono_s());
        pthread_mutex_unlock(&g_lock);
    }
    free(s);
}

static void fuzz_ring(const uint8_t *d, size_t n)
{
    pthread_mutex_lock(&g_lock);
    free_ring_locked();
    g_session++; g_producing = 1; g_target = 0;
    uint32_t sess = g_session;
    pthread_mutex_unlock(&g_lock);
    for (size_t i = 0; i + 5 <= n; i += 5) {
        size_t sz = (size_t)(d[i] | d[i + 1] << 8) * 32;         /* up to ~2 MB */
        uint32_t ms = (uint32_t)(d[i + 2] | d[i + 3] << 8);
        wbuf_t b = { 0 };
        if (d[i + 4] & 0x80) b.err = 1;
        else if (sz) { b.d = malloc(sz); if (!b.d) continue; b.n = b.cap = sz; }   /* contents never read here */
        publish_seg(sess, &b, (uint64_t)ms * 90, d[i + 4] & 1);
        assert(g_nseg <= HLS_RING);
        assert(g_seg_bytes <= HLS_MEM_MAX || g_nseg <= HLS_MIN_KEEP);
        for (int j = 1; j < g_nseg; j++) assert(g_seg[j].seq == g_seg[j - 1].seq + 1);
    }
    char pl[1024];
    pthread_mutex_lock(&g_lock);
    if (g_nseg) {
        int len = playlist_locked(pl, sizeof pl);
        assert(len > 0 && (size_t)len < sizeof pl);
        for (int j = 0; j < g_nseg; j++) assert((g_seg[j].dur90 + 45000) / 90000 <= (g_target ? g_target : 1) || g_seg[j].dur90 < 45000);
    }
    free_ring_locked();
    g_producing = 0;
    pthread_mutex_unlock(&g_lock);
}

#ifdef FUZZ_CAST
static void fuzz_cast_msg(const uint8_t *d, size_t n)
{
    cast_msg_t m;
    if (cast_decode(d, n, &m) == 0) {
        assert(strlen(m.src) < sizeof m.src && strlen(m.ns) < sizeof m.ns);
        if (m.payload) {
            assert(m.payload >= (const char *)d && m.payload + m.plen <= (const char *)d + n);
            char *js = malloc(m.plen + 1);
            memcpy(js, m.payload, m.plen);
            js[m.plen] = 0;
            char t[40], tr[64], se[64], ps[24];
            cast_json_str(js, "type", t, sizeof t);
            assert(strlen(t) < sizeof t);
            if (cast_find_app(js, "CC1AD845", tr, sizeof tr, se, sizeof se) == 0) assert(tr[0] && strlen(tr) < sizeof tr);
            cast_player_state(js, ps, sizeof ps);
            free(js);
        }
    }
    /* round trip: four NUL-separated strings (truncated to the encoder's limits) */
    char *s = malloc(n + 4);
    memcpy(s, d, n);
    s[n] = s[n + 1] = s[n + 2] = s[n + 3] = 0;
    const char *f[4];
    size_t k = 0;
    for (int i = 0; i < 4; i++) { f[i] = s + k; k += strlen(s + k); if (k < n) k++; }
    unsigned char out[4096];
    size_t len = cast_encode(out, sizeof out, f[0], f[1], f[2], f[3]);
    if (len) {
        assert(len >= 4 && (size_t)(out[0] << 24 | out[1] << 16 | out[2] << 8 | out[3]) == len - 4);
        assert(cast_decode(out + 4, len - 4, &m) == 0);
        assert(!strncmp(m.src, f[0], sizeof m.src - 1) && !strncmp(m.dst, f[1], sizeof m.dst - 1) && !strncmp(m.ns, f[2], sizeof m.ns - 1));
        assert(m.plen == strlen(f[3]) && !memcmp(m.payload, f[3], m.plen));
    }
    free(s);
}

static void fuzz_mdns(const uint8_t *d, size_t n)
{
    char name[64], model[48], ip[16];
    int port;
    if (cast_parse_mdns(d, n, "10.0.0.1", name, sizeof name, model, sizeof model, ip, sizeof ip, &port) == 0) {
        assert(port > 0 && port < 65536 && strlen(ip) < sizeof ip);
        for (const char *p = name; *p; p++) assert(*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20);
        for (const char *p = model; *p; p++) assert(*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20);
    }
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (!g_init_done) setup();
    if (n < 1) return 0;
    switch (d[0] % 4) {
    case 0: fuzz_path(d + 1, n - 1); break;
    case 1: fuzz_ring(d + 1, n - 1); break;
#ifdef FUZZ_CAST
    case 2: fuzz_cast_msg(d + 1, n - 1); break;
    default: fuzz_mdns(d + 1, n - 1); break;
#else
    default: fuzz_path(d + 1, n - 1); break;
#endif
    }
    return 0;
}
