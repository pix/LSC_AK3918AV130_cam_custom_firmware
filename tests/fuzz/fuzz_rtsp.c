/* libFuzzer harness: RTSP server (rtsp.c) + SmolRTSP request parser.
 *
 * Input layout: byte 0 = flags, rest = bytes a client sends on the RTSP
 * connection (requests, interleaved $-frames: RTCP / backchannel RTP).
 *   flags bit0: Digest check is still run, but treated as passed (reach
 *               DESCRIBE/SETUP/PLAY/TEARDOWN; nobody can forge MD5 by fuzzing)
 *   flags bit1: also run the stand-alone parsers (SmolRTSP_Request_parse,
 *               smolrtsp_parse_transport) on a heap copy (see parse_standalone)
 *
 * The connection: handle_client_inner() (the per-client loop, normally on its
 * own thread) runs on this thread; responses go to one end of a socketpair
 * (drained after), reads come from the input (see fz_recv_nb), then EOF. Peer 127.0.0.1
 * (login backoff exempts loopback -> deterministic).
 * live_next() is replaced inside rtsp.c (no real-time waits): after PLAY it
 * hands out one small H.264 access unit and one audio period, for TCP
 * (interleaved) sessions only, so nothing is sent over UDP. */
#include <sys/socket.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define auth_digest_verify fz_auth_digest_verify
#define live_next fz_live_next
#define conn_recv_nb fz_recv_nb
#define conn_readable fz_readable
#include "rtsp.c"
#undef auth_digest_verify
#undef live_next
#undef conn_recv_nb
#undef conn_readable

int auth_digest_verify(const char *authz, const char *method);
int live_next(live_cursor_t *c, uint8_t *buf, size_t cap, live_item_t *it, int timeout_ms);

/* Reads: the client loop handles one request per recv() (pipelined requests
 * sitting in the buffer wait for the next packet), so the input is cut into
 * "packets" at the separator "|||" and handed out one per read. */
static const uint8_t *g_in;
static size_t g_in_len, g_in_pos;
static size_t pkt_end(void)
{
    const uint8_t *s = g_in_len - g_in_pos >= 3 ? memmem(g_in + g_in_pos, g_in_len - g_in_pos, "|||", 3) : NULL;
    return s ? (size_t)(s - g_in) : g_in_len;
}
static int g_peer = -1;                          /* client end: responses are drained */
static void drain(void)
{
    char sink[8192];
    while (g_peer >= 0 && recv(g_peer, sink, sizeof sink, MSG_DONTWAIT) > 0) {}
}
int fz_readable(conn_t *c, int timeout_ms) { (void)c; (void)timeout_ms; drain(); return 1; }   /* data or EOF */
ssize_t fz_recv_nb(conn_t *c, void *p, size_t n)
{
    (void)c;
    if (g_in_pos >= g_in_len) return 0;
    size_t e = pkt_end(), k = e - g_in_pos;
    if (k > n) k = n;
    memcpy(p, g_in + g_in_pos, k);
    g_in_pos += k;
    if (g_in_pos == e && e < g_in_len) g_in_pos += 3;
    if (!k) return -2;                           /* empty packet: "nothing yet" */
    return (ssize_t)k;
}

static int g_bypass;
int fz_auth_digest_verify(const char *authz, const char *method)
{
    int v = auth_digest_verify(authz, method);
    return g_bypass ? 1 : v;
}

static int g_served;
static const uint8_t k_au[] = {
    0, 0, 0, 1, 0x09, 0xf0,                                     /* AUD */
    0, 0, 0, 1, 0x67, 0x4d, 0x00, 0x28, 0xe9, 0x00, 0xf0, 0x04, /* SPS */
    0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80,                         /* PPS */
    0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33, 0xff, 0xfe, 0xf6, 0xf0, 0xfe, 0x05, 0x36, 0x56, /* IDR */
};
int fz_live_next(live_cursor_t *c, uint8_t *buf, size_t cap, live_item_t *it, int timeout_ms)
{
    (void)timeout_ms;
    Client *cl = (Client *)((char *)c - offsetof(Client, cur));
    if (!cl->is_tcp || g_served >= 2) return 0;
    if (g_served++ == 0) {
        memcpy(buf, k_au, sizeof k_au);
        it->len = sizeof k_au; it->key = 1; it->ts = 1000;
        return LIVE_VIDEO;
    }
    for (int i = 0; i < LIVE_APER; i++) it->pcm[i] = (int16_t)(i * 97);
    it->n = 320; it->ts = 1000;
    return LIVE_AUDIO;
}

static void init_once(void)
{
    static int done;
    if (done) return;
    done = 1;
    signal(SIGPIPE, SIG_IGN);
    auth_init();
    rtsp_register_params();
    speaker_set_ready(1);                         /* backchannel audio reaches the queue */
}

/* Exact-size copy plus the NUL rtsp.c always keeps after its buffer: without
 * it SmolRTSP reads one byte past the slice (smolrtsp_match_numeric /
 * match_ident / match_header_name look at input.ptr[0] of an empty slice, e.g.
 * "X Y RTSP/"); on the camera that byte is rtsp.c's terminator. Set
 * FUZZ_RTSP_EXACT to see it. */
static void parse_standalone(const uint8_t *d, size_t n)
{
#ifdef FUZZ_RTSP_EXACT
    char *p = malloc(n ? n : 1);
#else
    char *p = malloc(n + 1);
    p[n] = 0;
#endif
    memcpy(p, d, n);
    SmolRTSP_Request req = SmolRTSP_Request_uninit();
    SmolRTSP_Request_parse(&req, CharSlice99_new(p, n));
    SmolRTSP_TransportConfig tc;
    smolrtsp_parse_transport(&tc, CharSlice99_new(p, n));
    free(p);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    init_once();
    if (n < 1 || n > 16384) return 0;
    uint8_t flags = d[0];
    d++; n--;
    g_bypass = flags & 1;
    g_served = 0;
    if (flags & 2) parse_standalone(d, n);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) abort();
    int big = 1 << 20;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    struct timeval tv = { 1, 0 };
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    g_in = d; g_in_len = n; g_in_pos = 0; g_peer = sv[1];

    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(40000);
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    conn_t *cn = conn_plain(sv[0]);
    handle_client_inner(cn, &ss, sizeof *sin);   /* closes sv[0] and frees cn */

    drain();
    close(sv[1]);
    g_peer = -1;
    speaker_stop();                               /* backchannel audio queued: drop */
    return 0;
}
