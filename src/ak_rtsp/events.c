#include "events.h"
#include "mqtt.h"
#include "ctrl_params.h"
#include "control.h"
#include "snapshot.h"
#include "speaker.h"
#include "system.h"
#include "record.h"
#include "onvif.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

static char s_webhook[256];
static int  s_snapshot = 1;

static int url_ok(const char *v)
{
    if (!*v) return 1;
    if (strncmp(v, "http://", 7)) return 0;
    for (const char *p = v; *p; p++) if ((unsigned char)*p <= 0x20 || *p == '"' || *p == '\\') return 0;
    return strlen(v) > 7;
}
CTRL_STR(p_hook, s_webhook, url_ok, "must be an http:// URL (no spaces); HTTPS is not supported")
CTRL_BOOL(p_snap, s_snapshot)

/* ---- queue ---------------------------------------------------------------- */
typedef struct { char kind[8]; int on, level; time_t t; } ev_t;
#define QN 16
static ev_t g_q[QN];
static int g_qh, g_qn;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_motion, g_sound, g_tamper;
static time_t g_last_motion, g_last_sound, g_last_tamper;
static char g_hook_status[96] = "";
static void (*g_hook)(const char *, int);

void events_set_hook(void (*fn)(const char *kind, int on)) { g_hook = fn; }
int  events_motion_active(void) { return g_motion; }

static void push(const char *kind, int on, int level)
{
    pthread_mutex_lock(&g_lock);
    if (g_qn < QN) {
        ev_t *e = &g_q[(g_qh + g_qn) % QN];
        snprintf(e->kind, sizeof e->kind, "%s", kind);
        e->on = on; e->level = level; e->t = time(NULL);
        g_qn++;
        pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
}

void events_motion(int on)
{
    if (on == g_motion || (on && !sched_events_ok())) return;
    g_motion = on;
    if (on) g_last_motion = time(NULL);
    printf("[events] motion %s\n", on ? "ON" : "off");
    onvif_event("motion", on);
    push("motion", on, 0);
}

void events_sound(int on, int level_db)
{
    if (on == g_sound || (on && !sched_events_ok())) return;
    g_sound = on;
    if (on) g_last_sound = time(NULL);
    printf("[events] sound %s (%d dBFS)\n", on ? "ON" : "off", level_db);
    onvif_event("sound", on);
    push("sound", on, level_db);
}

void events_tamper(int state)
{
    if (state == g_tamper || (state && !g_tamper && !sched_events_ok())) return;
    int on = state != 0;
    g_tamper = state;
    if (on) g_last_tamper = time(NULL);
    printf("[events] tamper %s\n", state == 1 ? "ON (lens covered)" : state == 2 ? "ON (camera moved)" : "off");
    onvif_event("tamper", on);
    push("tamper", on, state);
}

/* ---- outgoing webhook (plain HTTP POST, JSON) ------------------------------- */
static void webhook(const ev_t *e)
{
    if (!s_webhook[0]) return;
    char host[128], path[200] = "/";
    int port = 80;
    const char *h = s_webhook + 7, *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl >= sizeof host) return;
    memcpy(host, h, hl); host[hl] = 0;
    if (slash) snprintf(path, sizeof path, "%s", slash);
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    char ts[32];
    struct tm tm;
    localtime_r(&e->t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    char body[256];
    int bl = snprintf(body, sizeof body, "{\"camera\":\"%s\",\"event\":\"%s\",\"state\":\"%s\",\"time\":\"%s\"}",
                      sys_device_name(), e->kind, e->on ? "on" : "off", ts);
    if (!strcmp(e->kind, "sound"))
        bl = snprintf(body, sizeof body, "{\"camera\":\"%s\",\"event\":\"sound\",\"state\":\"%s\",\"time\":\"%s\",\"level_db\":%d}",
                      sys_device_name(), e->on ? "on" : "off", ts, e->level);
    if (!strcmp(e->kind, "tamper"))
        bl = snprintf(body, sizeof body, "{\"camera\":\"%s\",\"event\":\"tamper\",\"state\":\"%s\",\"time\":\"%s\",\"reason\":\"%s\"}",
                      sys_device_name(), e->on ? "on" : "off", ts, e->level == 1 ? "covered" : e->level == 2 ? "moved" : "");
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(g_hook_status, sizeof g_hook_status, "error: cannot resolve %s", host); return; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(g_hook_status, sizeof g_hook_status, "error: cannot connect to %s:%d", host, port);
        if (fd >= 0) close(fd);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);
    char req[600];
    int rl = snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nConnection: close\r\nUser-Agent: ak_rtsp\r\n\r\n%s", path, host, bl, body);
    char resp[64] = "";
    if (send(fd, req, (size_t)rl, MSG_NOSIGNAL) == rl) {
        ssize_t n = recv(fd, resp, sizeof resp - 1, 0);
        if (n > 0) resp[n] = 0;
    }
    close(fd);
    int code = 0;
    sscanf(resp, "HTTP/%*s %d", &code);
    if (code >= 200 && code < 300) snprintf(g_hook_status, sizeof g_hook_status, "ok (%d)", code);
    else snprintf(g_hook_status, sizeof g_hook_status, "error: HTTP %d", code);
}

static void *worker(void *arg)
{
    (void)arg;
    static uint8_t jpg[200 * 1024];
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_qn) pthread_cond_wait(&g_cond, &g_lock);
        ev_t e = g_q[g_qh];
        g_qh = (g_qh + 1) % QN; g_qn--;
        pthread_mutex_unlock(&g_lock);

        if (g_hook) g_hook(e.kind, e.on);
        mqtt_publish_str(e.kind, e.on ? "ON" : "OFF", 1);
        if (!strcmp(e.kind, "motion") && e.on && s_snapshot && mqtt_connected()) {
            int n = snapshot_jpeg(jpg, sizeof jpg, 0);
            if (n > 0) mqtt_publish("snapshot", jpg, (size_t)n, 1);
        }
        if (e.on) record_event_snapshot(e.kind);
        webhook(&e);
    }
    return NULL;
}

/* ---- MQTT commands ------------------------------------------------------------ */
static int token_ok(const char *v)
{
    if (!*v || strlen(v) > 16) return 0;
    for (; *v; v++) if (!isalnum((unsigned char)*v)) return 0;
    return 1;
}

static void on_command(const char *name, const char *payload)
{
    if (!token_ok(payload)) return;
    {   /* at most 10 commands per 10 s from the broker */
        static uint32_t win; static int n;
        uint32_t now = (uint32_t)time(NULL) / 10;
        if (now != win) { win = now; n = 0; }
        if (++n > 10) return;
    }
    char cmd[96], out[160];
    const char *err;
    if (!strcmp(name, "sound")) { if (speaker_ready()) speaker_play_builtin(payload); return; }
    if (!strcmp(name, "ir"))     snprintf(cmd, sizeof cmd, "SET ir.mode %s", payload);
    else if (!strcmp(name, "night"))  snprintf(cmd, sizeof cmd, "SET night.mode %s", payload);
    else if (!strcmp(name, "led"))    snprintf(cmd, sizeof cmd, "SET led.status %s", payload);
    else if (!strcmp(name, "volume")) snprintf(cmd, sizeof cmd, "SET speaker.volume %s", payload);
    else if (!strcmp(name, "motion_detection")) { control_set_value("md.enabled", payload, &err); return; }
    else if (!strcmp(name, "recording"))        { control_set_value("record.mode", payload, &err); return; }
    else return;
    control_exec(cmd, out, sizeof out);
}

int events_status_json(char *out, size_t cap)
{
    char m[400], lm[32] = "", ls[32] = "", lt[32] = "";
    struct tm tm;
    if (g_last_motion) { localtime_r(&g_last_motion, &tm); strftime(lm, sizeof lm, "%Y-%m-%d %H:%M:%S", &tm); }
    if (g_last_sound)  { localtime_r(&g_last_sound, &tm);  strftime(ls, sizeof ls, "%Y-%m-%d %H:%M:%S", &tm); }
    if (g_last_tamper) { localtime_r(&g_last_tamper, &tm); strftime(lt, sizeof lt, "%Y-%m-%d %H:%M:%S", &tm); }
    mqtt_status_json(m, sizeof m);
    return snprintf(out, cap, "{\"motion\":%s,\"sound\":%s,\"tamper\":%s,\"last_motion\":\"%s\",\"last_sound\":\"%s\",\"last_tamper\":\"%s\",\"webhook\":\"%s\",\"mqtt\":%s}",
                    g_motion ? "true" : "false", g_sound ? "true" : "false", g_tamper ? "true" : "false", lm, ls, lt, g_hook_status, m);
}

void events_init(void)
{
    CTRL_REG("events.webhook", p_hook, CTRL_PERSIST);
    CTRL_REG("events.snapshot", p_snap, CTRL_PERSIST);
}

void events_start(void)
{
    mqtt_set_command_handler(on_command);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, worker, NULL);
    pthread_attr_destroy(&a);
}
