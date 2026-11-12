#include "mqtt.h"
#include "ctrl_params.h"
#include "control.h"
#include "monotime.h"
#include "system.h"
#include "netcfg.h"
#include "night.h"
#include "speaker.h"
#include "version_info.h"
#include "conn.h"
#include "tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* ---- settings ------------------------------------------------------------- */
static int  s_enabled, s_port = 1883, s_discovery = 1, s_tls, s_verify = 1, s_ccert;   /* ccert 0 none 1 camera 2 custom */
static char s_host[128], s_user[64], s_pass[128], s_topic[96];

static int host_ok(const char *v)
{
    for (; *v; v++) if (!(isalnum((unsigned char)*v) || *v == '.' || *v == '-' || *v == ':')) return 0;
    return 1;
}
static int topic_ok(const char *v)
{
    if (!*v) return 1;                                  /* empty = default */
    if (*v == '/' || v[strlen(v) - 1] == '/') return 0;
    for (; *v; v++) if (!(isalnum((unsigned char)*v) || *v == '_' || *v == '-' || *v == '/')) return 0;
    return 1;
}
static void kick(void);
CTRL_BOOL(p_en, s_enabled)
CTRL_INT(p_port, s_port, 1, 65535)
CTRL_BOOL(p_disc, s_discovery)
CTRL_STR(p_host, s_host, host_ok, "must be a hostname or IP address")
CTRL_STR(p_user, s_user, NULL, "too long")
CTRL_STR(p_pass, s_pass, NULL, "too long")
CTRL_STR(p_topic, s_topic, topic_ok, "letters, digits, '_', '-' and '/' only")
CTRL_BOOL(p_tls, s_tls)
/* broker verification: "on" (CA, as before), "pin" (pinned key), "off" */
static int p_verify_get(char *o, size_t c) { snprintf(o, c, "%s", s_verify == 1 ? "on" : s_verify == 2 ? "pin" : "off"); return 0; }
static int p_verify_set(const char *v, const char **e)
{
    if (!strcmp(v, "on") || !strcmp(v, "ca")) s_verify = 1; else if (!strcmp(v, "pin")) s_verify = 2; else if (!strcmp(v, "off")) s_verify = 0;
    else { *e = "must be on, pin or off"; return -1; }
    return 0;
}
static int p_ccert_get(char *o, size_t c) { snprintf(o, c, "%s", s_ccert == 1 ? "camera" : s_ccert == 2 ? "custom" : "none"); return 0; }
static int p_ccert_set(const char *v, const char **e)
{
    if (!strcmp(v, "none")) s_ccert = 0; else if (!strcmp(v, "camera")) s_ccert = 1; else if (!strcmp(v, "custom")) s_ccert = 2;
    else { *e = "must be none, camera or custom"; return -1; }
    return 0;
}
/* every mqtt.* change reconnects */
#define WRAP(fn) static int fn##_setk(const char *v, const char **e) { int r = fn##_set(v, e); if (!r) kick(); return r; }
WRAP(p_en) WRAP(p_port) WRAP(p_disc) WRAP(p_host) WRAP(p_user) WRAP(p_pass) WRAP(p_topic) WRAP(p_tls) WRAP(p_verify) WRAP(p_ccert)

/* ---- state ------------------------------------------------------------------ */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   /* socket writes */
static conn_t *g_conn;
static volatile int g_connected, g_reconnect, g_kick;
static char g_base[128], g_node[32], g_err[96];
static void (*g_cmd)(const char *, const char *);

void mqtt_set_command_handler(void (*fn)(const char *, const char *)) { g_cmd = fn; }
int  mqtt_connected(void) { return g_connected; }
static void kick(void) { g_reconnect = 1; }
void mqtt_kick(void) { g_kick = 1; }
void mqtt_reconnect(void) { g_reconnect = 1; }

static void node_id(void)
{
    char mac[32] = "000000000000";
    FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    if (f) {
        char m[32] = "";
        if (fgets(m, sizeof(m), f)) {
            int k = 0;
            for (char *p = m; *p && k < 12; p++) if (isxdigit((unsigned char)*p)) mac[k++] = (char)tolower((unsigned char)*p);
            mac[k] = 0;
        }
        fclose(f);
    }
    snprintf(g_node, sizeof(g_node), "lsc_%s", mac);
}

/* ---- wire format ---------------------------------------------------------------- */
typedef struct { uint8_t *b; size_t n, cap; } pkt_t;
static int put(pkt_t *p, const void *d, size_t n)
{
    if (p->n + n > p->cap) {
        size_t c = p->cap ? p->cap * 2 : 256;
        while (c < p->n + n) c *= 2;
        uint8_t *nb = realloc(p->b, c);
        if (!nb) return -1;
        p->b = nb; p->cap = c;
    }
    memcpy(p->b + p->n, d, n); p->n += n;
    return 0;
}
static int put_u16(pkt_t *p, unsigned v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; return put(p, b, 2); }
static int put_str(pkt_t *p, const char *s, size_t n) { return put_u16(p, (unsigned)n) || put(p, s, n); }


/* fixed header + remaining length + body, under g_lock */
static int send_packet(uint8_t type, const pkt_t *body)
{
    uint8_t h[5]; size_t hl = 0, len = body ? body->n : 0;
    h[hl++] = type;
    do { uint8_t d = len % 128; len /= 128; if (len) d |= 0x80; h[hl++] = d; } while (len && hl < 5);
    pthread_mutex_lock(&g_lock);
    /* one write per packet (header + body) so TLS doesn't split it in two records */
    int r = -1;
    if (g_conn) {
        size_t bn = body ? body->n : 0;
        uint8_t *all = malloc(hl + bn);
        if (all) {
            memcpy(all, h, hl);
            if (bn) memcpy(all + hl, body->b, bn);
            r = conn_send_all(g_conn, all, hl + bn);
            free(all);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static int publish_raw(const char *topic, const void *payload, size_t len, int retain)
{
    pkt_t p = { 0 };
    int r = put_str(&p, topic, strlen(topic)) || put(&p, payload, len) ? -1 : send_packet((uint8_t)(0x30 | (retain ? 1 : 0)), &p);
    free(p.b);
    return r;
}

int mqtt_publish(const char *sub, const void *payload, size_t len, int retain)
{
    if (!g_connected) return -1;
    char t[192];
    snprintf(t, sizeof(t), "%s/%s", g_base, sub);
    return publish_raw(t, payload, len, retain);
}
int mqtt_publish_str(const char *sub, const char *payload, int retain) { return mqtt_publish(sub, payload, strlen(payload), retain); }

/* ---- Home Assistant discovery ------------------------------------------------------- */
static void disc(const char *component, const char *obj, const char *name, const char *extra)
{
    char topic[192], json[1400];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", component, g_node, obj);
    snprintf(json, sizeof(json),
        "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"object_id\":\"%s_%s\","
        "\"availability_topic\":\"%s/availability\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"LSC (Anyka AK3918)\","
        "\"model\":\"ak_rtsp\",\"sw_version\":\"%s\"}%s%s}",
        name, g_node, obj, g_node, obj, g_base, g_node, sys_device_name(), GIT_HASH, extra[0] ? "," : "", extra);
    publish_raw(topic, json, strlen(json), 1);
}

static void publish_discovery(void)
{
    char e[700];
#define B g_base
    snprintf(e, sizeof e, "\"state_topic\":\"%s/motion\",\"device_class\":\"motion\"", B);           disc("binary_sensor", "motion", "Motion", e);
    snprintf(e, sizeof e, "\"state_topic\":\"%s/sound\",\"device_class\":\"sound\"", B);             disc("binary_sensor", "sound", "Sound", e);
    snprintf(e, sizeof e, "\"state_topic\":\"%s/tamper\",\"device_class\":\"tamper\"", B);           disc("binary_sensor", "tamper", "Tamper", e);
    snprintf(e, sizeof e, "\"state_topic\":\"%s/temperature\",\"device_class\":\"temperature\",\"unit_of_measurement\":\"°C\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\"", B);
    disc("sensor", "temperature", "Temperature", e);
    snprintf(e, sizeof e, "\"state_topic\":\"%s/light\",\"unit_of_measurement\":\"%%\",\"state_class\":\"measurement\",\"icon\":\"mdi:brightness-6\"", B);
    disc("sensor", "light", "Light level", e);
    snprintf(e, sizeof e, "\"state_topic\":\"%s/rssi\",\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\"", B);
    disc("sensor", "rssi", "Wi-Fi signal", e);
    snprintf(e, sizeof e, "\"command_topic\":\"%s/cmd/sound\",\"payload_press\":\"chime\"", B);      disc("button", "chime", "Play chime", e);
    snprintf(e, sizeof e, "\"command_topic\":\"%s/cmd/ir\",\"state_topic\":\"%s/ir\",\"options\":[\"auto\",\"on\",\"off\"],\"icon\":\"mdi:led-outline\"", B, B);
    disc("select", "ir", "Infrared lights", e);
    snprintf(e, sizeof e, "\"command_topic\":\"%s/cmd/night\",\"state_topic\":\"%s/night\",\"options\":[\"auto\",\"day\",\"night\"],\"icon\":\"mdi:theme-light-dark\"", B, B);
    disc("select", "night", "Day / night", e);
    snprintf(e, sizeof e, "\"command_topic\":\"%s/cmd/led\",\"state_topic\":\"%s/led\",\"payload_on\":\"on\",\"payload_off\":\"off\",\"entity_category\":\"config\"", B, B);
    disc("switch", "led", "Status LED", e);
    snprintf(e, sizeof e, "\"command_topic\":\"%s/cmd/volume\",\"state_topic\":\"%s/volume\",\"min\":0,\"max\":100,\"step\":5,\"icon\":\"mdi:volume-high\",\"entity_category\":\"config\"", B, B);
    disc("number", "volume", "Speaker volume", e);
    snprintf(e, sizeof e, "\"command_topic\":\"%s/cmd/motion_detection\",\"state_topic\":\"%s/motion_detection\",\"payload_on\":\"on\",\"payload_off\":\"off\",\"icon\":\"mdi:motion-sensor\",\"entity_category\":\"config\"", B, B);
    disc("switch", "motion_detection", "Motion detection", e);
    snprintf(e, sizeof e, "\"command_topic\":\"%s/cmd/recording\",\"state_topic\":\"%s/recording\",\"options\":[\"off\",\"motion\",\"continuous\"],\"icon\":\"mdi:record-rec\",\"entity_category\":\"config\"", B, B);
    disc("select", "recording", "SD recording", e);
    snprintf(e, sizeof e, "\"topic\":\"%s/snapshot\"", B);                                            disc("camera", "snapshot", "Last motion", e);
#undef B
}

/* current states of the controllable things (cheap; sent every 30 s and after commands) */
static void publish_states(void)
{
    char v[128];
    int t = sys_temp_c10();
    if (t != -9999) { snprintf(v, sizeof v, "%d.%d", t / 10, abs(t % 10)); mqtt_publish_str("temperature", v, 0); }
    int lp = night_light_pct();
    if (lp >= 0) { snprintf(v, sizeof v, "%d", lp); mqtt_publish_str("light", v, 0); }
    int r = net_rssi();
    if (r) { snprintf(v, sizeof v, "%d", r); mqtt_publish_str("rssi", v, 0); }
    static const char *const params[][2] = {
        { "ir.mode", "ir" }, { "night.mode", "night" }, { "led.status", "led" }, { "speaker.volume", "volume" },
        { "md.enabled", "motion_detection" }, { "record.mode", "recording" } };
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        char cmd[64], out[160];
        snprintf(cmd, sizeof cmd, "GET %s", params[i][0]);
        control_exec(cmd, out, sizeof out);
        char *eq = strchr(out, '=');
        if (!eq || !strncmp(out, "ERR", 3)) continue;
        eq[strcspn(eq, "\r\n")] = 0;
        mqtt_publish_str(params[i][1], eq + 1, 1);
    }
}

/* ---- connection ------------------------------------------------------------------------- */
static int tcp_connect(const char *host, int port)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(g_err, sizeof g_err, "cannot resolve %s", host); return -1; }
    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(g_err, sizeof g_err, "cannot connect to %s:%d (%s)", host, port, strerror(errno));
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int read_n(conn_t *c, uint8_t *b, size_t n)
{
    while (n) {
        ssize_t r = conn_recv(c, b, n);
        if (r <= 0) return -1;
        b += r; n -= (size_t)r;
    }
    return 0;
}

/* read one packet (type, body) — blocking with the socket's receive timeout */
static int read_packet(conn_t *fd, uint8_t *type, uint8_t **body, size_t *len)
{
    uint8_t h;
    if (read_n(fd, &h, 1)) return -1;
    size_t l = 0; int mul = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t d;
        if (read_n(fd, &d, 1)) return -1;
        l += (size_t)(d & 0x7f) * (size_t)mul; mul *= 128;
        if (!(d & 0x80)) break;
        if (i == 3) return -1;
    }
    if (l > 256 * 1024) return -1;
    uint8_t *b = malloc(l + 1);
    if (!b || (l && read_n(fd, b, l))) { free(b); return -1; }
    b[l] = 0;
    *type = h; *body = b; *len = l;
    return 0;
}

static int do_connect(void)
{
    if (s_topic[0]) snprintf(g_base, sizeof g_base, "%s", s_topic);
    else snprintf(g_base, sizeof g_base, "lsc_camera/%s", g_node + 4);
    int sfd = tcp_connect(s_host, s_port);
    if (sfd < 0) return -1;
    conn_t *fd;
    if (s_tls) {
        fd = tls_client_connect(sfd, s_host, s_verify, s_ccert, sys_device_name_cn(), 15000, g_err, sizeof g_err);
        if (!fd) return -1;
        struct timeval tv = { 5, 0 };
        setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    } else {
        fd = conn_plain(sfd);
        if (!fd) { close(sfd); return -1; }
    }

    char will[160];
    snprintf(will, sizeof will, "%s/availability", g_base);
    pkt_t p = { 0 };
    uint8_t flags = 0x02 | 0x04 | 0x20;                 /* clean session, will, will retain */
    if (s_user[0]) flags |= 0x80;
    if (s_user[0] && s_pass[0]) flags |= 0x40;
    put_str(&p, "MQTT", 4);
    uint8_t lv[2] = { 4, flags };
    put(&p, lv, 2);
    put_u16(&p, 60);                                    /* keep-alive */
    put_str(&p, g_node, strlen(g_node));
    put_str(&p, will, strlen(will));
    put_str(&p, "offline", 7);
    if (flags & 0x80) put_str(&p, s_user, strlen(s_user));
    if (flags & 0x40) put_str(&p, s_pass, strlen(s_pass));
    pthread_mutex_lock(&g_lock); g_conn = fd; pthread_mutex_unlock(&g_lock);
    int r = send_packet(0x10, &p);
    free(p.b);
    uint8_t t, *b = NULL; size_t l = 0;
    if (r || read_packet(fd, &t, &b, &l) || t != 0x20 || l < 2 || b[1] != 0) {
        static const char *const why[] = { "", "protocol refused", "client id refused", "server unavailable",
                                           "bad user name or password", "not authorised" };
        snprintf(g_err, sizeof g_err, "broker refused connection%s%s",
                 b && l >= 2 && b[1] < 6 ? ": " : "", b && l >= 2 && b[1] < 6 ? why[b[1]] : "");
        free(b);
        pthread_mutex_lock(&g_lock); g_conn = NULL; pthread_mutex_unlock(&g_lock);
        conn_close(fd);
        return -1;
    }
    free(b);
    /* subscribe to commands and to HA's birth message (re-announce on HA restart) */
    pkt_t s = { 0 };
    char filt[160];
    snprintf(filt, sizeof filt, "%s/cmd/+", g_base);
    put_u16(&s, 1);
    put_str(&s, filt, strlen(filt)); uint8_t q = 0; put(&s, &q, 1);
    put_str(&s, "homeassistant/status", 20); put(&s, &q, 1);
    send_packet(0x82, &s);
    free(s.b);
    g_connected = 1;
    g_err[0] = 0;
    char ta[160];
    snprintf(ta, sizeof ta, "%s/availability", g_base);
    publish_raw(ta, "online", 6, 1);
    if (s_discovery) publish_discovery();
    publish_states();
    printf("[mqtt] connected to %s:%d%s as %s (base topic %s)\n", s_host, s_port, s_tls ? " (TLS)" : "", g_node, g_base);
    return 0;
}

static void disconnect(int graceful)
{
    if (graceful && g_connected) send_packet(0xe0, NULL);
    g_connected = 0;
    pthread_mutex_lock(&g_lock);
    if (g_conn) conn_close(g_conn);
    g_conn = NULL;
    pthread_mutex_unlock(&g_lock);
}

static void handle_publish(uint8_t type, const uint8_t *b, size_t l)
{
    if (l < 2) return;
    size_t tl = (size_t)b[0] << 8 | b[1];
    if (2 + tl > l) return;
    char topic[192];
    snprintf(topic, sizeof topic, "%.*s", (int)tl, (const char *)b + 2);
    size_t off = 2 + tl + (((type >> 1) & 3) ? 2 : 0);   /* packet id if QoS > 0 */
    if (off > l) return;
    char payload[256];
    snprintf(payload, sizeof payload, "%.*s", (int)(l - off), (const char *)b + off);
    if (!strcmp(topic, "homeassistant/status")) {
        if (!strcmp(payload, "online") && s_discovery) { publish_discovery(); g_kick = 1; }
        return;
    }
    size_t bl = strlen(g_base);
    if (strncmp(topic, g_base, bl) || strncmp(topic + bl, "/cmd/", 5)) return;
    const char *name = topic + bl + 5;
    printf("[mqtt] command %s = %s\n", name, payload);
    if (g_cmd) g_cmd(name, payload);
    g_kick = 1;
}

static void *mqtt_thread(void *arg)
{
    (void)arg;
    node_id();
    int backoff = 2;
    for (;;) {
        if (!s_enabled || !s_host[0]) { g_reconnect = 0; if (!s_enabled) g_err[0] = 0; sleep(1); continue; }
        g_reconnect = 0;
        if (do_connect() != 0) {
            printf("[mqtt] %s; retry in %d s\n", g_err, backoff);
            for (int i = 0; i < backoff * 10 && !g_reconnect; i++) usleep(100000);
            if (g_reconnect) backoff = 2;              /* settings changed: try again right away */
            else if (backoff < 60) backoff *= 2;
            continue;
        }
        backoff = 2;
        int64_t last_ping = mono_ms(), last_state = mono_ms(), last_rx = mono_ms();
        while (!g_reconnect && s_enabled) {
            int pr = conn_readable(g_conn, 500);
            int64_t now = mono_ms();
            if (pr > 0) {
                uint8_t t, *b = NULL; size_t l = 0;
                if (read_packet(g_conn, &t, &b, &l)) { snprintf(g_err, sizeof g_err, "connection lost"); break; }
                last_rx = now;
                if ((t & 0xf0) == 0x30) handle_publish(t, b, l);
                free(b);
            }
            if (now - last_ping > 30000) { if (send_packet(0xc0, NULL)) break; last_ping = now; }
            if (now - last_rx > 95000) { snprintf(g_err, sizeof g_err, "broker stopped answering"); break; }
            if (g_kick || now - last_state > 30000) { g_kick = 0; last_state = now; publish_states(); }
        }
        printf("[mqtt] disconnected%s%s\n", g_err[0] ? ": " : "", g_err);
        disconnect(g_reconnect || !s_enabled);
    }
    return NULL;
}

int mqtt_status_json(char *out, size_t cap)
{
    char e[100];
    snprintf(e, sizeof e, "%s", g_err);
    for (char *p = e; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = '\'';
    return snprintf(out, cap, "{\"connected\":%s,\"base\":\"%s\",\"node\":\"%s\",\"error\":\"%s\"}",
                    g_connected ? "true" : "false", g_base, g_node, e);
}

void mqtt_init(void)
{
#define REGK(name, fn, fl) do { ctrl_param_t p_ = { name, fn##_get, fn##_setk, fl }; control_register(&p_); } while (0)
    REGK("mqtt.enabled", p_en, CTRL_PERSIST);
    REGK("mqtt.host", p_host, CTRL_PERSIST);
    REGK("mqtt.port", p_port, CTRL_PERSIST);
    REGK("mqtt.user", p_user, CTRL_PERSIST);
    REGK("mqtt.pass", p_pass, CTRL_PERSIST | CTRL_SECRET);
    REGK("mqtt.topic", p_topic, CTRL_PERSIST);
    REGK("mqtt.discovery", p_disc, CTRL_PERSIST);
    REGK("mqtt.tls", p_tls, CTRL_PERSIST);
    REGK("mqtt.tls_verify", p_verify, CTRL_PERSIST);
    REGK("mqtt.client_cert", p_ccert, CTRL_PERSIST);
#undef REGK
}

void mqtt_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 96 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, mqtt_thread, NULL);
    pthread_attr_destroy(&a);
}

/* TCP connection to the configured (or given) broker, for fetching its certificate. */
int mqtt_open_tcp(const char *host, int port, char *err, size_t errcap)
{
    int fd = tcp_connect(host && *host ? host : s_host, port > 0 ? port : s_port);
    if (fd < 0) snprintf(err, errcap, "%s", g_err);
    return fd;
}
