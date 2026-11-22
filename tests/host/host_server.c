/* The camera's ak_rtsp on the PC (tests/host): main.c's start-up without the
 * hardware steps. Real code: web/HTTPS (80/443), RTSP/RTSPS (554/322), ONVIF,
 * control port 8091, MQTT, events, recordings, settings, TLS/ACME. Video:
 * an Annex-B H.264 file (argv[1], with AUD NALs) pushed to the live hub at
 * 15 fps in a loop; audio: a 440 Hz tone, 8 kHz.
 * Run it only through run_sandboxed.sh (own network / process namespaces).
 * SIGTERM/SIGINT exit cleanly so LeakSanitizer / valgrind report. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "ak_rtsp.h"
#include "auth.h"
#include "control.h"
#include "osd.h"
#include "rtsp.h"
#include "system.h"
#include "mqtt.h"
#include "acme.h"
#include "events.h"
#include "notify.h"
#include "sounddet.h"
#include "sched.h"
#include "web.h"
#include "hls.h"
#include "term.h"
#include "record.h"
#include "onvif.h"
#include "snapshot.h"
#include "resetkey.h"
#include "netcfg.h"
#include "live.h"
#include "speaker.h"
#include "monotime.h"

static volatile sig_atomic_t g_stop;
static void on_term(int s) { (void)s; g_stop = 1; }

static uint8_t *g_h264;
static size_t g_h264_len;

/* next access unit (from one AUD to the next) */
static size_t next_au(size_t pos, size_t *len, int *key)
{
    size_t i = pos + 4, start = pos;
    *key = 0;
    for (; i + 4 < g_h264_len; i++) {
        if (g_h264[i] == 0 && g_h264[i + 1] == 0 && g_h264[i + 2] == 1) {
            int t = g_h264[i + 3] & 0x1f;
            if (t == 5) *key = 1;
            if (t == 9 && i > start + 4) { size_t s = g_h264[i - 1] == 0 ? i - 1 : i; *len = s - start; return s; }
        }
    }
    *len = g_h264_len - start;
    return g_h264_len;
}

static void *feeder(void *a)
{
    (void)a;
    /* as on the camera: frames and audio periods arrive at their real rate,
     * timestamps from one monotonic clock (encoder ms / capture ms) */
    size_t pos = 0;
    int16_t pcm[256];
    double ph = 0;
    int64_t t0 = mono_ms(), next = t0;
    for (unsigned tick = 0; !g_stop; tick++) {
        if (tick % 2 == 0) {                              /* video 15 fps */
            size_t len; int key;
            size_t nxt = next_au(pos, &len, &key);
            if (len > 0 && len < LIVE_MAX_FRAME) live_push_frame(g_h264 + pos, len, key, (uint64_t)mono_ms());
            pos = nxt >= g_h264_len ? 0 : nxt;
        }
        for (int i = 0; i < 256; i++) { pcm[i] = (int16_t)(3000 * sin(ph)); ph += 2 * M_PI * 440 / 8000; }
        live_push_audio(pcm, 256);                        /* 32 ms of 8 kHz audio, the camera's capture period */
        int16_t out[256];
        speaker_fill(out, 256);                           /* the playback side of audio.c: drain at 8 kHz */
        next += 32;
        int64_t d = next - mono_ms();
        if (d > 0) usleep((useconds_t)d * 1000);
    }
    return NULL;
}

/* Simulated detector events for the tests: lines written to the FIFO
 * CFG_DIR/sim_events ("motion 1", "motion 0", "sound 1 -30", "tamper 2")
 * go through events.c like the real detectors (MQTT, webhook, recorder,
 * notifications). Opened read-write so it never sees EOF; raw read(), so no
 * stdio lock is held while blocked (exit() flushes every FILE). */
static void *sim_events(void *a)
{
    (void)a;
    const char *path = CFG_DIR "/sim_events";
    unlink(path);
    if (mkfifo(path, 0600) != 0) return NULL;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return NULL;
    char buf[256];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = 0;
        char *nl;
        while ((nl = strchr(buf, '\n'))) {
            *nl = 0;
            char kind[16] = "";
            int on = 0, level = 0;
            if (sscanf(buf, "%15s %d %d", kind, &on, &level) >= 2) {
                if (!strcmp(kind, "motion")) events_motion(on);
                else if (!strcmp(kind, "sound")) events_sound(on, level);
                else if (!strcmp(kind, "tamper")) events_tamper(on);
                else if (!strcmp(kind, "line")) events_line(on, level, 1);     /* level: 1 left, 2 right */
                else if (!strcmp(kind, "loiter")) events_loiter(on, level);    /* level: zone */
            }
            len -= (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, len + 1);
        }
        if (len == sizeof buf - 1) len = 0;               /* overlong line: drop */
    }
    close(fd);
    return NULL;
}

static int listen_tcp(int port)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) { perror("rtsp bind"); exit(2); }
    return s;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: host_server <video.h264>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); g_h264_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_h264 = malloc(g_h264_len);
    if (!g_h264 || fread(g_h264, 1, g_h264_len, f) != g_h264_len) return 2;
    fclose(f);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* main.c order, hardware steps left out */
    control_start();
    auth_init();
    osd_init();
    rtsp_register_params();
    sys_register_params();
    mqtt_init();
    acme_init();
    events_init();
    notify_init();
    sounddet_init();
    sched_init();
    web_register_params();
    hls_register_params();
    term_init();
    record_init();
    onvif_init();
    snapshot_init();
    resetkey_check_sd();                  /* mount() is wrapped: no card */
    control_load_ext_settings();
    sys_time_set_tz(TIME_DEFAULT_TZ);
    control_load_settings();
    sys_time_start();
    net_start();                          /* NETCFG_SIM */
    events_start();
    notify_start();
    mqtt_start();
    acme_start();
    sched_start();
    record_start();
    onvif_start();
    if (web_start() < 0) { fprintf(stderr, "web_start failed\n"); return 1; }

    speaker_set_ready(1);                 /* as after audio_start() on the camera: WAV / talk-back paths run (output discarded) */
    pthread_t t;
    pthread_create(&t, NULL, sim_events, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, feeder, NULL);
    int s = listen_tcp(RTSP_SERVER_PORT), st = listen_tcp(RTSPS_SERVER_PORT);
    printf("[host] ready\n");
    fflush(stdout);
    while (!g_stop) {
        struct pollfd p[2] = { { s, POLLIN, 0 }, { st, POLLIN, 0 } };
        if (poll(p, 2, 200) <= 0) continue;
        int tls = !(p[0].revents & POLLIN);
        struct sockaddr_storage ca;
        socklen_t cl = sizeof ca;
        int c = accept(tls ? st : s, (struct sockaddr *)&ca, &cl);
        if (c >= 0) rtsp_start_client(c, tls, &ca, cl);
    }
    printf("[host] stopping\n");
    fflush(stdout);
    pthread_join(t, NULL);
    hls_stop();                           /* HLS producer: unsubscribe, free its ring */
    free(g_h264);
    exit(0);        /* exit(), not _exit(): LeakSanitizer reports from an atexit hook */
}
