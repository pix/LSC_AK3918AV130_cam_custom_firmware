/* libFuzzer harness: ONVIF SOAP (onvif.c onvif_handle) and the WS-Discovery
 * Probe parser.
 *
 * Input: byte 0 = flags, then one or more HTTP requests separated by "|||".
 * Each request: "<path>\n<Host header>\n<Authorization header>\n<SOAP body>"
 * (limits as in web.c: path < 256, host < 128, authz < 1024, body <= 16384).
 *   flags bit0: HTTP Digest check still runs, but is treated as passed
 *   flags bit1: connection is "TLS" (onvif_handle's is_tls: PasswordText allowed)
 *   flags bit2: each request body is also fed to the WS-Discovery Probe parser
 *   flags bit3: a motion / sound / tamper / line / loiter event is raised before each request
 *               (fills PullPoint queues)
 * Requests of one input share state (PullPoint subscriptions), which is reset
 * between inputs. Changes inside onvif.c only (macros), all for determinism
 * and speed: time() is fixed at 2026-11-22T10:00:00Z, PullMessages' wait
 * thread runs inline and its condition wait returns at once, sync()/sleep()
 * before the (link-wrapped) reboot are no-ops. Responses go into a socketpair
 * and are discarded. Peer 127.0.0.1 (exempt from login backoff). */
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define FZ_NOW ((time_t)1795341600)
static time_t fz_time(time_t *t) { if (t) *t = FZ_NOW; return FZ_NOW; }
static int fz_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{ (void)t; (void)a; fn(arg); return 0; }
static int fz_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *ts)
{ (void)c; (void)m; (void)ts; return ETIMEDOUT; }
static void fz_sync(void) {}
static unsigned fz_sleep(unsigned s) { (void)s; return 0; }
int fz_digest_verify_ex(const char *authz, const char *method, const char *req_uri);

#define time fz_time
#define pthread_create fz_pthread_create
#define pthread_cond_timedwait fz_timedwait
#define sync fz_sync
#define sleep fz_sleep
#define auth_digest_verify_ex fz_digest_verify_ex
#include "onvif.c"
#undef time
#undef pthread_create
#undef pthread_cond_timedwait
#undef sync
#undef sleep
#undef auth_digest_verify_ex
int auth_digest_verify_ex(const char *authz, const char *method, const char *req_uri);

static int g_bypass;
int fz_digest_verify_ex(const char *authz, const char *method, const char *req_uri)
{
    int v = auth_digest_verify_ex(authz, method, req_uri);
    return g_bypass ? 1 : v;
}

static void init_once(void)
{
    static int done;
    if (done) return;
    done = 1;
    signal(SIGPIPE, SIG_IGN);
    auth_init();
    onvif_init();
}

static void reset_state(void)
{
    memset(g_subs, 0, sizeof g_subs);
    g_next_id = 1;
    g_pulls = 0;
    g_motion = g_sound = g_tamper = 0;
    memset(g_ws_seen, 0, sizeof g_ws_seen);
    s_enabled = 1;
}

/* WS-Discovery: the per-datagram part of discovery_thread() (it is inline in
 * the thread loop there), on a 4096-byte datagram buffer as on the camera. */
static void discovery_probe(const char *data, size_t n)
{
    char buf[4096], out[4096];
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    memcpy(buf, data, n);
    buf[n] = 0;
    if (!find_el(buf, "Probe") || !find_el(buf, "MessageID")) return;
    char types[200] = "";
    el_text(buf, "Types", types, sizeof types);
    if (types[0] && !strstr(types, "NetworkVideoTransmitter") && !strstr(types, "Device")) return;
    char msgid[128] = "";
    el_text(buf, "MessageID", msgid, sizeof msgid);
    for (char *p = msgid; *p; p++) if (*p == '<' || *p == '&') *p = '_';
    discovery_msg(out, sizeof out, "ProbeMatches", msgid, "192.168.1.50");
}

/* copy one '\n'-terminated field (bounded) and advance */
static const char *field(const char *p, const char *end, char *out, size_t cap)
{
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *e = nl ? nl : end;
    size_t k = (size_t)(e - p);
    if (k > cap - 1) k = cap - 1;
    memcpy(out, p, k);
    out[k] = 0;
    return nl ? nl + 1 : end;
}

static void one_request(const char *p, size_t n, uint8_t flags, int idx)
{
    const char *end = p + n;
    char path[256], host[128], authz[1024];
    p = field(p, end, path, sizeof path);
    p = field(p, end, host, sizeof host);
    p = field(p, end, authz, sizeof authz);
    size_t bl = (size_t)(end - p);
    if (bl > 16384) bl = 16384;
    char *body = malloc(bl + 1);
    memcpy(body, p, bl);
    body[bl] = 0;                                  /* web.c NUL-terminates SOAP bodies */

    if (flags & 4) discovery_probe(body, bl);
    if (flags & 8) {
        static const char *const kinds[] = { "motion", "sound", "tamper", "line", "loiter" };
        onvif_event(kinds[idx % 5], idx % 5 == 3 ? idx + 1 : !(idx & 1));     /* line: on = object id */
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) abort();
    shutdown(sv[1], SHUT_WR);
    int big = 1 << 20;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    struct timeval tv = { 1, 0 };
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in peer = { 0 };
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    conn_t *cn = conn_plain(sv[0]);
    int penalty = 0;
    int handed = onvif_handle(cn, path, body, host, authz, (const struct sockaddr *)&peer, flags & 2 ? 1 : 0, &penalty);
    if (!handed) conn_close(cn);                   /* else the (inline) pull thread closed it */
    close(sv[1]);
    free(body);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    init_once();
    if (n < 1 || n > 65536) return 0;
    uint8_t flags = d[0];
    const char *p = (const char *)d + 1, *end = (const char *)d + n;
    g_bypass = flags & 1;
    reset_state();
    int idx = 0;
    while (p <= end && idx < 16) {
        const char *sep = end - p >= 3 ? memmem(p, (size_t)(end - p), "|||", 3) : NULL;
        const char *e = sep ? sep : end;
        one_request(p, (size_t)(e - p), flags, idx++);
        if (!sep) break;
        p = sep + 3;
    }
    return 0;
}
