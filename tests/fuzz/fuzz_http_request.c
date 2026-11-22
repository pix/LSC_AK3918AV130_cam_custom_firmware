/* libFuzzer harness: the whole HTTP request path of web.c.
 *
 * One input = one connection: byte 0 selects the client (see below), the
 * rest is written into a socketpair, the write side is shut down, and the
 * other end goes through conn_plain() -> read_request() -> handle_conn(),
 * exactly as serve() does (minus the 1 s failed-login penalty sleep). The
 * response is drained and discarded by a pump thread (poll on both
 * directions, so large uploads + large responses cannot deadlock).
 *
 * Byte 0:
 *   bits 0-1  peer: 0 loopback (login backoff exempt), 1 192.0.2.1,
 *             2 192.0.2.9 (configured as web.proxies -> X-Forwarded-*), 3 no address
 *   bit 2     the harness inserts a VALID Digest Authorization header
 *             (admin/admin, fresh nonce, qop=auth, uri = request target), so
 *             every authenticated handler is reachable through the real login
 *             check too (bit clear: the fuzzer's own headers only).
 *
 * fuzz_http_handlers.c includes this file with WEB_TEST_NOAUTH defined.
 *
 * Skipped by path (request line parsed exactly as read_request_buf does):
 *   /api/mqtt/fetch    opens a TCP connection to a host/port from the body
 *   /api/wifi/connect  starts the 45 s background reconnect thread (netcfg)
 * sleep/usleep/sync/fsync are replaced by no-ops for the whole binary
 * (do_reboot's sleep(1), aec_dump's 50 ms polling, sync() after settings).
 */
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include "web.c"
#include "md5.h"
#include "speaker.h"
#include "sched.h"   /* firmware scheduler (sched_init); <sched.h> above is the system one */

unsigned int sleep(unsigned int s) { (void)s; return 0; }
int usleep(useconds_t u) { (void)u; return 0; }
void sync(void) {}
int fsync(int fd) { (void)fd; return 0; }

#define HOOK_TOK "0123456789abcdef0123456789abcdef"
#define REC_DAY  REC_DIR "/2026-11-01"

typedef struct { int fd; const uint8_t *d; size_t n; } pump_t;

static void *pump(void *arg)
{
    pump_t *p = arg;
    size_t off = 0;
    int wdone = 0;
    static __thread char buf[65536];
    int show = getenv("FUZZ_HTTP_SHOW") ? 2 : 0;   /* debugging: print the start of each response */
    if (!p->n) { shutdown(p->fd, SHUT_WR); wdone = 1; }
    for (;;) {
        struct pollfd pf = { p->fd, (short)(POLLIN | (wdone ? 0 : POLLOUT)), 0 };
        if (poll(&pf, 1, -1) < 0) { if (errno == EINTR) continue; break; }
        if (pf.revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = recv(p->fd, buf, sizeof buf, MSG_DONTWAIT);
            if (r == 0) break;
            if (r < 0 && errno != EAGAIN && errno != EINTR) break;
            if (r > 0 && show) { fprintf(stderr, "[resp] %.*s\n", (int)(r < 600 ? r : 600), buf); show--; }
        }
        if (!wdone && (pf.revents & POLLOUT)) {
            ssize_t w = send(p->fd, p->d + off, p->n - off, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w < 0) { if (errno != EAGAIN && errno != EINTR) wdone = 1; }
            else if ((off += (size_t)w) == p->n) { shutdown(p->fd, SHUT_WR); wdone = 1; }
        }
    }
    return NULL;
}

static void put_file(const char *path, size_t size, int force)
{
    struct stat st;
    if (!force && stat(path, &st) == 0 && (size_t)st.st_size == size) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (size_t i = 0; i < size; i++) fputc((int)(i * 7 & 0xff), f);
    fclose(f);
}

static void fixtures(void)
{
    FILE *f = fopen(HOOK_TOKEN_FILE, "r");
    char b[64] = "";
    if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
    if (strncmp(b, HOOK_TOK, 32)) {
        f = fopen(HOOK_TOKEN_FILE, "w");
        if (f) { fputs(HOOK_TOK "\n", f); fclose(f); }
    }
    mkdir(REC_DIR, 0755);
    mkdir(REC_DAY, 0755);
    put_file(REC_DAY "/12-00-00_motion.mp4", 1000, 0);
    put_file(REC_DAY "/12-00-05_motion.jpg", 300, 0);
    put_file(REC_DAY "/13-00-00_cont.mp4", 400 * 1024, 0);   /* > 300 KB: sender thread */
}

static int g_inited;
static void init_once(void)
{
    if (g_inited) return;
    g_inited = 1;
    signal(SIGPIPE, SIG_IGN);
    /* start from defaults: nothing saved by an earlier run */
    static const char *const stale[] = { SETTINGS_FILE, EXT_SETTINGS_FILE, AUTH_FILE, AUTH_ONVIF_FILE, OSD_FILE,
        CFG_DIR "/ak_rtsp_tls.conf", CFG_DIR "/ak_rtsp_tls_custom.crt", CFG_DIR "/ak_rtsp_tls_custom.key",
        CFG_DIR "/ak_rtsp_tls_custom.src", CFG_DIR "/ak_rtsp_acme_ca.crt" };
    for (size_t i = 0; i < sizeof stale / sizeof stale[0]; i++) unlink(stale[i]);
    auth_init();
    osd_init();
    rtsp_register_params();
    sys_register_params();
    mqtt_init();
    acme_init();
    events_init();
    sounddet_init();
    sched_init();
    web_register_params();
    term_init();
    record_init();
    snapshot_init();
    speaker_set_ready(1);
    tls_init();
    fixtures();
}

/* the request target as read_request_buf() sees it */
static int request_line(const uint8_t *d, size_t n, char method[8], char path[256], size_t *line_end)
{
    size_t i = 0;
    while (i < n && (d[i] == '\r' || d[i] == '\n')) i++;
    size_t s = i;
    while (i < n && d[i] != '\r' && d[i] != '\n' && d[i]) i++;
    if (line_end) *line_end = i;
    char line[600];
    size_t l = i - s < sizeof line - 1 ? i - s : sizeof line - 1;
    memcpy(line, d + s, l); line[l] = 0;
    char ver[16];
    method[0] = path[0] = 0;
    return sscanf(line, "%7s %255s %15s", method, path, ver) == 3 ? 0 : -1;
}

static int skipped(const char *target)
{
    char p[256];
    snprintf(p, sizeof p, "%s", target);
    char *q = strchr(p, '?');
    if (q) *q = 0;
    if (!strcmp(p, "/api/mqtt/fetch") || !strcmp(p, "/api/wifi/connect")) return 1;
    /* FUZZ_HTTP_SKIP="/a,/b": also skip these exact paths (to keep fuzzing past a known bug) */
    const char *env = getenv("FUZZ_HTTP_SKIP");
    for (const char *s = env; s && *s;) {
        size_t l = strcspn(s, ",");
        if (l && strlen(p) == l && !strncmp(p, s, l)) return 1;
        s += l + (s[l] == ',');
    }
    return 0;
}

/* valid Authorization: Digest for (method, uri) with the current credentials */
static int make_authz(const char *method, const char *uri, char *out, size_t cap)
{
    static unsigned g_cn;
    char chal[256], nonce[64] = "";
    auth_digest_challenge(chal, sizeof chal, 1, 0);
    const char *p = strstr(chal, "nonce=\"");
    if (!p) return -1;
    snprintf(nonce, sizeof nonce, "%.24s", p + 7);
    char ha1[33], ha2[33], resp[33], cnonce[24];
    snprintf(cnonce, sizeof cnonce, "c%08x", ++g_cn);
    md5_ctx c; uint8_t dg[16];
    md5_init(&c); md5_update(&c, "admin:" AUTH_REALM ":admin", strlen("admin:" AUTH_REALM ":admin")); md5_final(&c, dg); md5_to_hex(dg, ha1);
    md5_init(&c); md5_update(&c, method, strlen(method)); md5_update(&c, ":", 1); md5_update(&c, uri, strlen(uri)); md5_final(&c, dg); md5_to_hex(dg, ha2);
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
    md5_init(&c); md5_update(&c, tmp, strlen(tmp)); md5_final(&c, dg); md5_to_hex(dg, resp);
    snprintf(out, cap, "\r\nAuthorization: Digest username=\"admin\", realm=\"" AUTH_REALM "\", nonce=\"%s\", uri=\"%s\", "
             "algorithm=MD5, response=\"%s\", qop=auth, nc=00000001, cnonce=\"%s\"", nonce, uri, resp, cnonce);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    init_once();
    if (size < 1) return 0;
    uint8_t mode = data[0];
    const uint8_t *d = data + 1;
    size_t n = size - 1;
    char method[8], target[256];
    size_t le = 0;
    int have_line = request_line(d, n, method, target, &le) == 0;
    if (have_line && skipped(target)) return 0;

    /* per-iteration state reset (keeps inputs reproducible) */
    if (!auth_is_default()) auth_reset();
    if (tls_redirect()) { const char *e; tls_set_config(NULL, 0, -1, &e); }
    snprintf(s_proxies, sizeof s_proxies, "192.0.2.9");
    s_hosts[0] = 0;
    fixtures();

    uint8_t *req = NULL;
    if ((mode & 4) && have_line) {
        char authz[700];
        if (make_authz(method, target, authz, sizeof authz) == 0) {
            size_t al = strlen(authz);
            req = malloc(n + al);
            memcpy(req, d, le);
            memcpy(req + le, authz, al);
            memcpy(req + le + al, d + le, n - le);
            d = req; n += al;
        }
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    struct sockaddr_storage none;
    memset(&none, 0, sizeof none);
    const struct sockaddr *peer = (const struct sockaddr *)&sin;
    switch (mode & 3) {
    case 0: sin.sin_addr.s_addr = htonl(0x7f000001); break;
    case 1: sin.sin_addr.s_addr = htonl(0xc0000201); break;
    case 2: sin.sin_addr.s_addr = htonl(0xc0000209); break;
    default: peer = (const struct sockaddr *)&none; break;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) { free(req); return 0; }
    pump_t pj = { sv[0], d, n };
    pthread_t th;
    if (pthread_create(&th, NULL, pump, &pj) != 0) { close(sv[0]); close(sv[1]); free(req); return 0; }

    conn_t *cn = conn_plain(sv[1]);
    struct timeval tv = { 3, 0 };
    setsockopt(cn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct timeval tvs = { 10, 0 };
    setsockopt(cn->fd, SOL_SOCKET, SO_SNDTIMEO, &tvs, sizeof tvs);

    /* = serve() */
    req_t *rq = calloc(1, sizeof *rq);
    if (read_request(cn, rq) != 0) {
        respond_text(cn, 400, "Bad Request", "bad request\n");
        free(rq->big); free(rq);
        conn_close(cn);
    } else {
        struct sockaddr_in *pin = (struct sockaddr_in *)peer;
        if (peer->sa_family == AF_INET && is_proxy(pin->sin_addr.s_addr)) {
            rq->via_proxy = 1;
            struct in_addr ca;
            if (rq->xff[0] && inet_pton(AF_INET, rq->xff, &ca) == 1) sin.sin_addr = ca;
        }
        pthread_mutex_lock(&g_web_lock);
        g_penalty = 0;
        int handed = handle_conn(cn, rq, peer);
        g_peer = NULL;
        pthread_mutex_unlock(&g_web_lock);
        if (handed == 2) { hls_serve(cn, rq->path); handed = 0; }
        free(rq->big); free(rq);
        if (!handed) conn_close(cn);
    }
    pthread_join(th, NULL);           /* EOF once every holder of sv[1] closed it */
    close(sv[0]);
    for (;;) {                        /* a download thread drops its count after closing */
        pthread_mutex_lock(&g_send_lock);
        int busy = g_senders;
        pthread_mutex_unlock(&g_send_lock);
        if (!busy) break;
        sched_yield();
    }
    if (peer->sa_family == AF_INET) auth_record(peer, 1);   /* clear login backoff */
    free(req);
    return 0;
}
