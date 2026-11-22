/* libFuzzer harness: events.c (webhook URL from the events.webhook setting,
 * the webhook server's HTTP response, MQTT command handler), the sound hook's
 * WAV upload parser (speaker_play_wav, POST /hook/sound with the hook token)
 * and sounddet.c's level detector.
 *
 * Input: byte 0 = mode, byte 1 = parameter, rest = data.
 *   mode 0: webhook. data = "<URL>\n<HTTP response from the server>"; the URL
 *           goes through the setting's validator (CTRL_STR url_ok), an event
 *           (motion / sound / tamper / line / loiter, on/off, level from byte 1) is sent.
 *           getaddrinfo/socket/connect inside events.c hand out a socketpair
 *           with the response already in it: nothing touches the network.
 *   mode 1: MQTT command: data = "<name>\n<payload>" -> on_command()
 *   mode 2: speaker_play_wav(data)
 *   mode 3: sounddet_feed(data as 16-bit PCM), sound detection enabled
 * time() in events.c advances 10 s per call (rate limit, timestamps):
 * deterministic. The event queue (no worker thread here) is emptied per input. */
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
    g_fake_sa.sin_port = htons(80);
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
#define time fz_time
#include "events.c"
#undef time
#undef getaddrinfo
#undef freeaddrinfo
#undef socket
#undef connect

#include "auth.h"
#include "rtsp.h"
#include "acme.h"
#include "sounddet.h"
#include "web.h"
#include "term.h"
#include "onvif.h"
#include "snapshot.h"
void aec_register_params(void);

static void init_once(void)
{
    static int done;
    if (done) return;
    done = 1;
    signal(SIGPIPE, SIG_IGN);
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
    speaker_set_ready(1);
}

static void do_webhook(const char *data, size_t n, uint8_t p)
{
    const char *nl = memchr(data, '\n', n);
    size_t ul = nl ? (size_t)(nl - data) : n;
    char *url = malloc(ul + 1);
    memcpy(url, data, ul);
    url[ul] = 0;
    const char *err;
    if (control_set_value("events.webhook", url, &err) != 0 || !s_webhook[0]) { free(url); return; }
    free(url);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) abort();
    size_t rl = nl ? n - ul - 1 : 0;
    if (rl && write(sv[1], nl + 1, rl) != (ssize_t)rl) abort();
    shutdown(sv[1], SHUT_WR);
    g_fake_fd = sv[0];                              /* webhook() closes it */
    static const char *const kinds[] = { "motion", "sound", "tamper", "line", "loiter" };
    ev_t e = { "", p & 1, (int)(int8_t)p, fz_time(NULL) };
    snprintf(e.kind, sizeof e.kind, "%s", kinds[(p >> 1) % 5]);
    webhook(&e);
    char sink[4096];
    while (recv(sv[1], sink, sizeof sink, MSG_DONTWAIT) > 0) {}
    close(sv[1]);
    g_fake_fd = -1;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    init_once();
    if (n < 2 || n > 1 << 20) return 0;
    int mode = d[0] & 3;
    uint8_t p = d[1];
    const char *data = (const char *)d + 2;
    size_t dn = n - 2;

    if (mode == 0) {
        do_webhook(data, dn, p);
    } else if (mode == 1) {
        char *s = malloc(dn + 1);
        memcpy(s, data, dn);
        s[dn] = 0;
        char *nl = strchr(s, '\n');
        if (nl) { *nl = 0; on_command(s, nl + 1); }
        else on_command(s, "");
        free(s);
    } else if (mode == 2) {
        uint8_t *w = malloc(dn ? dn : 1);           /* exact size: a heap upload as in web.c */
        memcpy(w, data, dn);
        const char *err;
        speaker_play_wav(w, dn, &err);
        free(w);
    } else {
        const char *err;
        control_set_value("sound.enabled", "on", &err);
        size_t ns = dn / 2;
        int16_t *pcm = malloc((ns ? ns : 1) * sizeof *pcm);
        memcpy(pcm, data, ns * 2);
        for (int k = 0; k < 3; k++) sounddet_feed(pcm, ns);
        free(pcm);
        control_set_value("sound.enabled", "off", &err);
    }
    char js[1024];
    events_status_json(js, sizeof js);

    /* reset: queue, states, settings touched above */
    g_qh = g_qn = 0;
    g_motion = g_sound = g_tamper = 0;
    g_hook_status[0] = 0;
    s_webhook[0] = 0;
    speaker_stop();
    return 0;
}
