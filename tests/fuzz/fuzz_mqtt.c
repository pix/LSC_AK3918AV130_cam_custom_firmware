/* libFuzzer harness: MQTT client (mqtt.c) fed by a fuzzed broker, through to
 * the command handler (events.c on_command -> control_exec / control_set_value).
 *
 * Input: byte 0 = flags, rest = the byte stream "the broker" sends: CONNACK,
 * then PUBLISH (commands on <base>/cmd/<name>, homeassistant/status), or
 * anything else.
 *   flags bit0: discovery on     bit1: user name set     bit2: password set
 *   flags bit3: custom base topic "home/cam1" (else lsc_camera/<node>)
 * Path: do_connect() (CONNECT, read CONNACK, SUBSCRIBE, discovery, states),
 * then the receive loop of mqtt_thread(): read_packet() -> handle_publish().
 * Changes inside mqtt.c / events.c only (macros): getaddrinfo/socket/connect
 * hand out one end of a socketpair (nothing touches the network); the input
 * is written to the other end and half-closed, so the loop ends at EOF;
 * time() in events.c advances 10 s per call, so its 10-commands-per-10-s
 * limit never depends on the wall clock (deterministic). Both files have a
 * static g_lock, renamed apart. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_fake_fd = -1;
static struct sockaddr_in g_fake_sa;
static struct addrinfo g_fake_ai;
static int fz_getaddrinfo(const char *h, const char *s, const struct addrinfo *hi, struct addrinfo **res)
{
    (void)h; (void)s; (void)hi;
    g_fake_sa.sin_family = AF_INET;
    g_fake_sa.sin_port = htons(1883);
    g_fake_sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_fake_ai.ai_family = AF_INET;
    g_fake_ai.ai_socktype = SOCK_STREAM;
    g_fake_ai.ai_addr = (struct sockaddr *)&g_fake_sa;
    g_fake_ai.ai_addrlen = sizeof g_fake_sa;
    *res = &g_fake_ai;
    return 0;
}
static void fz_freeaddrinfo(struct addrinfo *a) { (void)a; }
static int fz_socket(int d, int t, int p) { (void)d; (void)t; (void)p; return g_fake_fd; }
static int fz_connect(int fd, const struct sockaddr *a, socklen_t l) { (void)fd; (void)a; (void)l; return 0; }
static time_t g_fz_t = 1795341600;
static time_t fz_time(time_t *t) { g_fz_t += 10; if (t) *t = g_fz_t; return g_fz_t; }

#define getaddrinfo fz_getaddrinfo
#define freeaddrinfo fz_freeaddrinfo
#define socket fz_socket
#define connect fz_connect
#define g_lock mqtt_g_lock
#include "mqtt.c"
#undef g_lock
#define g_lock events_g_lock
#define time fz_time
#include "events.c"
#undef time
#undef g_lock
#undef getaddrinfo
#undef freeaddrinfo
#undef socket
#undef connect

#include "auth.h"
#include "rtsp.h"
#include "system.h"
#include "acme.h"
#include "sounddet.h"
#include "sched.h"
#include "web.h"
#include "term.h"
#include "record.h"
#include "onvif.h"
#include "snapshot.h"
void aec_register_params(void);

static void init_once(void)
{
    static int done;
    if (done) return;
    done = 1;
    signal(SIGPIPE, SIG_IGN);
    /* host_server.c / main.c registration order (no *_start(): no threads) */
    auth_init();
    rtsp_register_params();
    sys_register_params();
    aec_register_params();
    mqtt_init();
    acme_init();
    events_init();
    sounddet_init();
    sched_init();
    web_register_params();
    term_init();
    record_init();
    onvif_init();
    snapshot_init();
    node_id();                                     /* reads the PC's wlan0 MAC, if any: */
    snprintf(g_node, sizeof g_node, "lsc_000000000000");   /* fixed, as on a camera without one */
    speaker_set_ready(1);
}

static int g_peer = -1;
static void drain(void)
{
    char sink[16384];
    while (recv(g_peer, sink, sizeof sink, MSG_DONTWAIT) > 0) {}
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    init_once();
    if (n < 1 || n > 65536) return 0;
    uint8_t flags = d[0];
    d++; n--;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) abort();
    int big = 1 << 20;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(sv[1], d + off, n - off);
        if (w <= 0) abort();
        off += (size_t)w;
    }
    shutdown(sv[1], SHUT_WR);
    g_fake_fd = sv[0];
    g_peer = sv[1];

    /* settings as the user would set them (validators run) */
    const char *err;
    s_enabled = 1; s_tls = 0; s_port = 1883;
    snprintf(s_host, sizeof s_host, "broker.lan");
    s_discovery = flags & 1;
    snprintf(s_user, sizeof s_user, "%s", flags & 2 ? "homeassistant" : "");
    snprintf(s_pass, sizeof s_pass, "%s", flags & 4 ? "s3cret pass" : "");
    p_topic_set(flags & 8 ? "home/cam1" : "", &err);
    g_connected = 0; g_reconnect = 0; g_kick = 0; g_err[0] = 0; g_conn = NULL;
    g_cmd = on_command;

    if (do_connect() == 0) {
        drain();
        for (;;) {                                 /* mqtt_thread()'s receive loop */
            uint8_t t, *b = NULL; size_t l = 0;
            if (read_packet(g_conn, &t, &b, &l)) { snprintf(g_err, sizeof g_err, "connection lost"); break; }
            if ((t & 0xf0) == 0x30) handle_publish(t, b, l);
            free(b);
            if (g_kick) { g_kick = 0; publish_states(); }
            drain();
        }
        disconnect(0);
    } else {
        /* do_connect closed the connection (or never opened it) */
    }
    char js[512];
    mqtt_status_json(js, sizeof js);
    drain();
    close(sv[1]);
    g_fake_fd = g_peer = -1;
    speaker_stop();
    return 0;
}
