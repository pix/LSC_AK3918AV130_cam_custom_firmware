#include "onvif.h"
#include "record.h"
#include "night.h"
#include "substream.h"
#include "auth.h"
#include "sha1.h"
#include "system.h"
#include "monotime.h"
#include "ctrl_params.h"
#include "version_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include "mbedtls/base64.h"

static int s_enabled = 1;
CTRL_BOOL(p_en, s_enabled)

extern int venc_kbps_now(void);
extern int venc_gop_now(void);

/* ---- helpers ------------------------------------------------------------- */
static void mac_hex(char out[13], char sep_out[18])
{
    char m[32] = "";
    FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    if (f) { if (!fgets(m, sizeof m, f)) m[0] = 0; fclose(f); }
    int k = 0;
    for (char *p = m; *p && k < 12; p++) if (isxdigit((unsigned char)*p)) out[k++] = (char)tolower((unsigned char)*p);
    while (k < 12) out[k++] = '0';
    out[12] = 0;
    if (sep_out) snprintf(sep_out, 18, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", out, out + 2, out + 4, out + 6, out + 8, out + 10);
}

static void device_uuid(char out[48])
{
    char m[13];
    mac_hex(m, NULL);
    snprintf(out, 48, "urn:uuid:5ec7a7e0-4c53-4c43-a3e1-%s", m);
}

static void first_ip(char out[16])
{
    struct ifaddrs *ifa = NULL;
    out[0] = 0;
    if (getifaddrs(&ifa) != 0) return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        struct in_addr a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        if ((ntohl(a.s_addr) >> 24) == 127) continue;
        inet_ntop(AF_INET, &a, out, 16);
        break;
    }
    freeifaddrs(ifa);
}

/* host part of a Host header (no port, no IPv6) */
static void host_only(const char *host, char *out, size_t cap)
{
    snprintf(out, cap, "%s", host && *host ? host : "");
    char *c = strchr(out, ':');
    if (c && !strchr(c + 1, ':')) *c = 0;
    for (char *p = out; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-')) { *p = 0; break; }
    if (!out[0]) { char ip[16]; first_ip(ip); snprintf(out, cap, "%s", ip); }
}

static void iso_time(time_t t, char out[32])
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* <prefix:Name ...> or <Name ...> -> pointer after '>' (NULL if absent). */
static const char *find_el(const char *xml, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = strchr(xml, '<'); p; p = strchr(p + 1, '<')) {
        const char *q = p + 1;
        if (*q == '/' || *q == '?' || *q == '!') continue;
        const char *colon = q, *end = q;
        while (*end && !isspace((unsigned char)*end) && *end != '>' && *end != '/') end++;
        for (const char *c = q; c < end; c++) if (*c == ':') colon = c + 1;
        if ((size_t)(end - colon) == nl && !strncmp(colon, name, nl)) {
            const char *gt = strchr(end, '>');
            return gt ? gt + 1 : NULL;
        }
    }
    return NULL;
}

/* text content of the first <..:name> element */
static int el_text(const char *xml, const char *name, char *out, size_t cap)
{
    const char *s = find_el(xml, name);
    out[0] = 0;
    if (!s) return -1;
    const char *e = strchr(s, '<');
    if (!e) return -1;
    while (s < e && isspace((unsigned char)*s)) s++;
    size_t n = (size_t)(e - s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n); out[n] = 0;
    return 0;
}

/* first Body child element name -> action */
static void body_action(const char *xml, char *out, size_t cap)
{
    out[0] = 0;
    const char *b = find_el(xml, "Body");
    if (!b) return;
    const char *p = strchr(b, '<');
    while (p && (p[1] == '!' || p[1] == '?')) p = strchr(p + 1, '<');
    if (!p || p[1] == '/') return;
    const char *q = p + 1, *end = q, *colon = q;
    while (*end && !isspace((unsigned char)*end) && *end != '>' && *end != '/') end++;
    for (const char *c = q; c < end; c++) if (*c == ':') colon = c + 1;
    size_t n = (size_t)(end - colon);
    if (n >= cap) n = cap - 1;
    memcpy(out, colon, n); out[n] = 0;
}

/* ---- authentication ------------------------------------------------------ */
/* Created = "YYYY-MM-DDThh:mm:ss[.fff]Z" -> Unix time, 0 if unreadable */
static time_t parse_created(const char *c)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(c, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    int64_t days = 0;                                       /* timegm without TZ games */
    for (int y = 1970; y < tm.tm_year + 1900; y++) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    static const int md[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y = tm.tm_year + 1900, leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int m = 0; m < tm.tm_mon && m < 12; m++) days += md[m] + (m == 1 && leap);
    days += tm.tm_mday - 1;
    return (time_t)(days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
}

/* nonces of accepted UsernameTokens in the last 10 minutes (replay check) */
#define WS_SEEN 64
static struct { uint8_t h[8]; time_t t; } g_ws_seen[WS_SEEN];
static pthread_mutex_t g_ws_lock = PTHREAD_MUTEX_INITIALIZER;
static int ws_nonce_fresh(const unsigned char *nonce, size_t nl, const char *created)
{
    uint8_t buf[200], h[20];
    if (nl + strlen(created) > sizeof buf) return 0;
    memcpy(buf, nonce, nl); memcpy(buf + nl, created, strlen(created));
    sha1(buf, nl + strlen(created), h);
    time_t now = (time_t)mono_s();
    int fresh = 1, oldest = 0;
    pthread_mutex_lock(&g_ws_lock);
    for (int i = 0; i < WS_SEEN; i++) {
        if (g_ws_seen[i].t && now - g_ws_seen[i].t < 600 && !memcmp(g_ws_seen[i].h, h, 8)) { fresh = 0; break; }
        if (g_ws_seen[i].t < g_ws_seen[oldest].t) oldest = i;
    }
    if (fresh) { memcpy(g_ws_seen[oldest].h, h, 8); g_ws_seen[oldest].t = now ? now : 1; }
    pthread_mutex_unlock(&g_ws_lock);
    return fresh;
}

/* 1 ok, 0 bad or missing */
static int ws_auth_ok(const char *xml, int is_tls)
{
    if (!find_el(xml, "UsernameToken")) return 0;
    char user[48], pw[200], nonce_b64[128], created[64], cur[40];
    if (el_text(xml, "Username", user, sizeof user) || el_text(xml, "Password", pw, sizeof pw)) return 0;
    auth_get_user(cur, sizeof cur);
    if (strcmp(user, cur) != 0) return 0;
    const char *pe = find_el(xml, "Password");
    const char *tag = pe;
    while (tag > xml && *tag != '<') tag--;
    int digest = tag && strstr(tag, "PasswordDigest") && strstr(tag, "PasswordDigest") < pe;
    if (!digest) return is_tls && auth_check_plain(user, pw);    /* PasswordText only inside TLS */
    if (el_text(xml, "Nonce", nonce_b64, sizeof nonce_b64) || el_text(xml, "Created", created, sizeof created)) return 0;
    if (sys_time_synced()) {                                       /* reject old (replayed) tokens */
        time_t ct = parse_created(created), now = time(NULL);
        if (!ct || ct < now - 300 || ct > now + 300) return 0;
    }
    unsigned char nonce[96];
    size_t nl = 0;
    if (mbedtls_base64_decode(nonce, sizeof nonce, &nl, (const unsigned char *)nonce_b64, strlen(nonce_b64)) != 0) return 0;
    char plain[80];
    if (auth_onvif_password(plain, sizeof plain) != 0) return 0;
    uint8_t buf[300], h[20];
    size_t bl = 0;
    if (nl + strlen(created) + strlen(plain) > sizeof buf) return 0;
    memcpy(buf, nonce, nl); bl += nl;
    memcpy(buf + bl, created, strlen(created)); bl += strlen(created);
    memcpy(buf + bl, plain, strlen(plain)); bl += strlen(plain);
    sha1(buf, bl, h);
    memset(plain, 0, sizeof plain);
    char expect[40];
    size_t el = 0;
    mbedtls_base64_encode((unsigned char *)expect, sizeof expect, &el, h, 20);
    expect[el] = 0;
    unsigned char d = (unsigned char)(strlen(expect) != strlen(pw));
    for (size_t i = 0; i < el && pw[i]; i++) d |= (unsigned char)(expect[i] ^ pw[i]);
    if (d != 0) return 0;
    return ws_nonce_fresh(nonce, nl, created);                     /* same token seen before: replay */
}

/* ---- SOAP envelope ---------------------------------------------------------- */
#define NS "xmlns:env=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" " \
           "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\" " \
           "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" " \
           "xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" xmlns:tns1=\"http://www.onvif.org/ver10/topics\" " \
           "xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\""

static void send_soap(conn_t *cn, int code, const char *body)
{
    size_t bl = strlen(body);
    char *x = malloc(bl + 1024);
    if (!x) return;
    int n = snprintf(x, bl + 1024, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><env:Envelope " NS "><env:Body>%s</env:Body></env:Envelope>", body);
    char h[256];
    int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: application/soap+xml; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                      code, code == 200 ? "OK" : code == 400 ? "Bad Request" : "Internal Server Error", n);
    conn_send_all(cn, h, (size_t)hl);
    conn_send_all(cn, x, (size_t)n);
    free(x);
}

static void fault(conn_t *cn, const char *sub, const char *reason)
{
    char b[700];
    snprintf(b, sizeof b, "<env:Fault xmlns:ter=\"http://www.onvif.org/ver10/error\"><env:Code><env:Value>env:Sender</env:Value>"
             "<env:Subcode><env:Value>ter:%s</env:Value></env:Subcode></env:Code>"
             "<env:Reason><env:Text xml:lang=\"en\">%s</env:Text></env:Reason></env:Fault>", sub, reason);
    send_soap(cn, 400, b);
}

/* ---- events (PullPoint) ------------------------------------------------------- */
#define MAX_SUBS 4
#define SUBQ 16
/* value: boolean 0/1 for state topics; a number for numeric topics (item Value);
 * now: stamp the sending time (Monitoring/OperatingTime below); src: 0 generic, 1 video, 2 audio */
typedef struct { char topic[72], item[24]; int value; time_t t; int init, now, src; } msg_t;
typedef struct { int used; unsigned id; time_t expires; msg_t q[SUBQ]; int qn; } sub_t;
static sub_t g_subs[MAX_SUBS];
static unsigned g_next_id = 1;
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ev_cond = PTHREAD_COND_INITIALIZER;
static int g_motion, g_sound, g_tamper;

static void sub_push_ex(sub_t *s, const char *topic, const char *item, int value, int init, int now, int src)
{
    if (s->qn == SUBQ) { memmove(s->q, s->q + 1, sizeof(msg_t) * (SUBQ - 1)); s->qn--; }
    msg_t *m = &s->q[s->qn++];
    snprintf(m->topic, sizeof m->topic, "%s", topic);
    snprintf(m->item, sizeof m->item, "%s", item);
    m->value = value; m->t = time(NULL); m->init = init; m->now = now;
    m->src = src ? src : (strstr(topic, "Audio") ? 2 : strstr(topic, "RuleEngine") ? 1 : 0);
}
static void sub_push(sub_t *s, const char *topic, const char *item, int value, int init)
{ sub_push_ex(s, topic, item, value, init, 0, 0); }

static void onvif_sys_push(sub_t *s, int init);

static void push_state(sub_t *s, int init)
{
    sub_push(s, "tns1:RuleEngine/CellMotionDetector/Motion", "IsMotion", g_motion, init);
    sub_push(s, "tns1:VideoSource/MotionAlarm", "State", g_motion, init);
    sub_push(s, "tns1:AudioAnalytics/Audio/DetectedSound", "IsSoundDetected", g_sound, init);
    sub_push(s, "tns1:VideoSource/GlobalSceneChange/ImagingService", "State", g_tamper, init);
    onvif_sys_push(s, init);
}

void onvif_event(const char *kind, int on)
{
    pthread_mutex_lock(&g_ev_lock);
    time_t now = time(NULL);
    if (!strcmp(kind, "motion")) g_motion = on; else if (!strcmp(kind, "sound")) g_sound = on; else if (!strcmp(kind, "tamper")) g_tamper = on;
    for (int i = 0; i < MAX_SUBS; i++) {
        sub_t *s = &g_subs[i];
        if (!s->used) continue;
        if (s->expires < now) { s->used = 0; continue; }
        if (!strcmp(kind, "motion")) {
            sub_push(s, "tns1:RuleEngine/CellMotionDetector/Motion", "IsMotion", on, 0);
            sub_push(s, "tns1:VideoSource/MotionAlarm", "State", on, 0);
        } else if (!strcmp(kind, "tamper")) {
            sub_push(s, "tns1:VideoSource/GlobalSceneChange/ImagingService", "State", on, 0);
            sub_push(s, "tns1:RuleEngine/CellMotionDetector/Tamper", "IsTamperDetected", on, 0);
            sub_push(s, "tns1:RuleEngine/TamperDetector/Tamper", "IsTamperDetected", on, 0);
        } else {
            sub_push(s, "tns1:AudioAnalytics/Audio/DetectedSound", "IsSoundDetected", on, 0);
        }
    }
    pthread_cond_broadcast(&g_ev_cond);
    pthread_mutex_unlock(&g_ev_lock);
}

/* ---- system state as events: what HA turns into diagnostic sensors ---- */
static int s_temp_c = -9999, s_sd, s_sd_known, s_total_mb, s_free_mb;
static int s_rec_on = -1, s_was_synced, s_cpu_pct;
static time_t s_last_sync;
static uint64_t s_cpu_prev;

static uint64_t boot_time(void)                     /* now - uptime */
{
    double up = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    return (uint64_t)(time(NULL) - (time_t)up);
}

static void to_events(const char *topic, const char *item, int value, int now, int src)
{
    pthread_mutex_lock(&g_ev_lock);
    for (int i = 0; i < MAX_SUBS; i++) {
        sub_t *s = &g_subs[i];
        if (!s->used || s->expires < time(NULL)) continue;
        sub_push_ex(s, topic, item, value, 0, now, src);
    }
    pthread_cond_broadcast(&g_ev_cond);
    pthread_mutex_unlock(&g_ev_lock);
}

static void onvif_sys_push(sub_t *s, int init);

static void onvif_sys_push(sub_t *s, int init)            /* initial values for a new subscription */
{
    sub_push_ex(s, "tns1:Monitoring/OperatingTime/LastReboot", "UtcTime", 0, init, 0, 0);
    sub_push_ex(s, "tns1:Monitoring/OperatingTime/LastClockSynchronization", "UtcTime", 0, init, 0, 0);
    sub_push_ex(s, "tns1:Monitoring/ProcessorUsage", "Value", s_cpu_pct, init, 0, 0);
    if (s_sd_known) sub_push_ex(s, "tns1:Device/HardwareFailure/StorageFailure", "Value", s_sd ? 0 : 1, init, 0, 0);
    if (s_rec_on >= 0) sub_push_ex(s, "tns1:RecordingConfig/JobState", "State", s_rec_on, init, 0, 0);
}

void onvif_sys_state(int temp_c10, int sd, uint64_t free_kb, uint64_t total_kb)
{
    s_temp_c = temp_c10;
    s_free_mb = (int)(free_kb / 1024); s_total_mb = (int)(total_kb / 1024);
    if (s_sd_known && sd != s_sd)
        to_events("tns1:Device/HardwareFailure/StorageFailure", "Value", sd ? 0 : 1, 0, 0);
    s_sd = sd; s_sd_known = 1;
}

void onvif_recording(int on)
{
    if (on == s_rec_on) return;
    s_rec_on = on;
    to_events("tns1:RecordingConfig/JobState", "State", on, 0, 0);
}

/* once a minute: processor load, and notice when NTP first set the clock */
static void sys_tick(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long u = 0, n = 0, x = 0;
        if (fscanf(f, "cpu %llu %llu %llu", &u, &n, &x) == 3) {
            unsigned long long busy = u + n + x, idle = x;
            if (s_cpu_prev) {
                unsigned long long tb = busy - (s_cpu_prev >> 32), ti = idle - (uint32_t)s_cpu_prev;
                int pct = tb ? (int)(100 - (ti * 100 + tb / 2) / tb) : 0;
                s_cpu_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
            }
            s_cpu_prev = (busy << 32) | (idle & 0xffffffffu);
        }
        fclose(f);
    }
    to_events("tns1:Monitoring/ProcessorUsage", "Value", s_cpu_pct, 0, 0);
    int synced = sys_time_synced();
    if (!s_was_synced && synced) {
        s_last_sync = time(NULL);
        to_events("tns1:Monitoring/OperatingTime/LastClockSynchronization", "UtcTime", 0, 1, 0);
    }
    s_was_synced = synced;
}

static long dur_seconds(const char *iso, long def)
{
    /* PT60S, PT1M, PT1H, PT10M30S ... */
    const char *p = strstr(iso, "PT");
    if (!p) return def;
    long total = 0, v = 0;
    for (p += 2; *p; p++) {
        if (isdigit((unsigned char)*p)) v = v * 10 + (*p - '0');
        else if (*p == 'H') { total += v * 3600; v = 0; }
        else if (*p == 'M') { total += v * 60; v = 0; }
        else if (*p == 'S' || *p == '.') { total += v; v = 0; if (*p == '.') break; }
    }
    return total > 0 ? total : def;
}

static int render_messages(sub_t *s, char *out, size_t cap, int limit)
{
    size_t o = 0;
    int n = s->qn < limit ? s->qn : limit;
    for (int i = 0; i < n && o < cap; i++) {
        msg_t *m = &s->q[i];
        char ts[32], vs[256] = "";
        iso_time(m->now ? time(NULL) : m->t, ts);
        if (m->src == 2)
            snprintf(vs, sizeof vs, "<tt:SimpleItem Name=\"AudioSourceConfigurationToken\" Value=\"asc\"/>");
        else if (m->src == 1)
            snprintf(vs, sizeof vs, "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"vsc\"/>"
                                    "<tt:SimpleItem Name=\"VideoAnalyticsConfigurationToken\" Value=\"vac\"/>"
                                    "<tt:SimpleItem Name=\"Rule\" Value=\"MotionRule\"/>");
        const char *src = vs[0] ? vs : "<tt:SimpleItem Name=\"Source\" Value=\"vs\"/><tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"vsc\"/>";
        char val[40];
        if (!strcmp(m->item, "UtcTime")) {                               /* xsd:dateTime */
            if (!strcmp(m->topic, "tns1:Monitoring/OperatingTime/LastReboot")) iso_time(boot_time(), val);
            else if (!strcmp(m->topic, "tns1:Monitoring/OperatingTime/LastClockSynchronization") && s_last_sync)
                iso_time(s_last_sync, val);
            else iso_time(m->now ? time(NULL) : m->t, val);
        }
        else if (!strcmp(m->item, "Value"))
            snprintf(val, sizeof val, "%d", m->value);                    /* numbers (tenths/percent) */
        else if (!strcmp(m->item, "State"))
            snprintf(val, sizeof val, "%s", m->value ? "Active" : "Idle");
        else
            snprintf(val, sizeof val, "%s", m->value ? "true" : "false");
        o += (size_t)snprintf(out + o, cap - o,
            "<wsnt:NotificationMessage><wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">%s</wsnt:Topic>"
            "<wsnt:Message><tt:Message UtcTime=\"%s\" PropertyOperation=\"%s\"><tt:Source>%s</tt:Source>"
            "<tt:Data><tt:SimpleItem Name=\"%s\" Value=\"%s\"/></tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>",
            m->topic, ts, m->init ? "Initialized" : "Changed", src, m->item, val);
    }
    memmove(s->q, s->q + n, sizeof(msg_t) * (size_t)(s->qn - n));
    s->qn -= n;
    return (int)o;
}

typedef struct { conn_t *cn; unsigned id; long timeout; int limit; } pull_t;

static void pull_respond(conn_t *cn, unsigned id, int limit)
{
    char *msgs = malloc(12000), *body = malloc(13000);
    if (!msgs || !body) { free(msgs); free(body); return; }
    msgs[0] = 0;
    time_t term = 0;
    pthread_mutex_lock(&g_ev_lock);
    for (int i = 0; i < MAX_SUBS; i++)
        if (g_subs[i].used && g_subs[i].id == id) { render_messages(&g_subs[i], msgs, 12000, limit); term = g_subs[i].expires; }
    pthread_mutex_unlock(&g_ev_lock);
    char now[32], tt[32];
    iso_time(time(NULL), now); iso_time(term, tt);
    snprintf(body, 13000, "<tev:PullMessagesResponse><tev:CurrentTime>%s</tev:CurrentTime><tev:TerminationTime>%s</tev:TerminationTime>%s</tev:PullMessagesResponse>", now, tt, msgs);
    send_soap(cn, 200, body);
    free(msgs); free(body);
}

#define MAX_PULLS 8
static int g_pulls;
static pthread_mutex_t g_pull_lock = PTHREAD_MUTEX_INITIALIZER;
static int pulls_add(int d) { pthread_mutex_lock(&g_pull_lock); g_pulls += d; int v = g_pulls; pthread_mutex_unlock(&g_pull_lock); return v; }
static void *pull_thread(void *arg)
{
    pull_t *p = arg;
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += p->timeout;
    pthread_mutex_lock(&g_ev_lock);
    for (;;) {
        sub_t *s = NULL;
        for (int i = 0; i < MAX_SUBS; i++) if (g_subs[i].used && g_subs[i].id == p->id) s = &g_subs[i];
        if (!s || s->qn) break;
        if (pthread_cond_timedwait(&g_ev_cond, &g_ev_lock, &dl) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g_ev_lock);
    pull_respond(p->cn, p->id, p->limit);
    conn_close(p->cn);
    free(p);
    pulls_add(-1);
    return NULL;
}

/* ---- request handling ------------------------------------------------------------ */
static int pre_auth(const char *a)
{
    return !strcmp(a, "GetSystemDateAndTime") || !strcmp(a, "GetCapabilities") || !strcmp(a, "GetServices") ||
           !strcmp(a, "GetServiceCapabilities") || !strcmp(a, "GetWsdlUrl") || !strcmp(a, "GetEndpointReference") ||
           !strcmp(a, "GetImagingSettings") || !strcmp(a, "GetOptions");
}

/* ProfileToken in a request body is "sub" (second stream) */
static int token_is_sub(const char *body)
{
    const char *p = body ? strstr(body, "ProfileToken>") : NULL;
    if (!p) return 0;
    p += strlen("ProfileToken>");
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return !strncmp(p, "sub", 3) && (p[3] == '<' || p[3] == ' ');
}

static void profile_xml_one(char *o, size_t cap, int sub);
static void profile_xml(char *o, size_t cap)
{
    profile_xml_one(o, cap, 0);
    if (substream_running()) { size_t n = strlen(o); profile_xml_one(o + n, cap - n, 1); }
}

static void profile_xml_one(char *o, size_t cap, int sub)
{
    snprintf(o, cap,
        "<trt:Profiles token=\"%s\" fixed=\"true\"><tt:Name>%s</tt:Name>"
        "<tt:VideoSourceConfiguration token=\"vsc\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>vs</tt:SourceToken>"
        "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/></tt:VideoSourceConfiguration>"
        "<tt:AudioSourceConfiguration token=\"asc\"><tt:Name>AudioSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>as</tt:SourceToken></tt:AudioSourceConfiguration>"
        "<tt:VideoEncoderConfiguration token=\"%s\"><tt:Name>H264</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>"
        "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality>"
        "<tt:RateControl><tt:FrameRateLimit>15</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>%d</tt:BitrateLimit></tt:RateControl>"
        "<tt:H264><tt:GovLength>%d</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>"
        "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
        "<tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:VideoEncoderConfiguration>"
        "<tt:AudioEncoderConfiguration token=\"aec\"><tt:Name>G711</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>G711</tt:Encoding>"
        "<tt:Bitrate>64</tt:Bitrate><tt:SampleRate>8</tt:SampleRate>"
        "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
        "<tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:AudioEncoderConfiguration>"
        "</trt:Profiles>", sub ? "sub" : "main", sub ? "SubStream" : "MainStream", sub ? "vec2" : "vec",
        sub ? 640 : 1920, sub ? 360 : 1080, sub ? 512 : venc_kbps_now(), sub ? 30 : venc_gop_now());
}

int onvif_handle(conn_t *cn, const char *path, const char *body, const char *host, const char *authz,
                 const struct sockaddr *peer, int is_tls, int *penalty)
{
    if (!s_enabled) { fault(cn, "ActionNotSupported", "ONVIF is disabled"); return 0; }
    char action[64], h[96], b[7000];
    body_action(body, action, sizeof action);
    host_only(host, h, sizeof h);
    if (!action[0]) { fault(cn, "InvalidArgVal", "no SOAP action"); return 0; }

    int has_token = find_el(body, "UsernameToken") != NULL, has_authz = authz && *authz;
    int dv = has_authz ? auth_digest_verify_ex(authz, "POST", path) : 0;
    int authed = (has_token && ws_auth_ok(body, is_tls)) || dv == 1;
    if (has_token || (has_authz && dv != -1)) {
        auth_record(peer, authed);
        if (!authed) *penalty = 1;
    }
    if (!authed && !pre_auth(action)) {
        send_soap(cn, 400, "<env:Fault xmlns:ter=\"http://www.onvif.org/ver10/error\"><env:Code><env:Value>env:Sender</env:Value>"
                  "<env:Subcode><env:Value>ter:NotAuthorized</env:Value></env:Subcode></env:Code>"
                  "<env:Reason><env:Text xml:lang=\"en\">Sender not authorized</env:Text></env:Reason></env:Fault>");
        return 0;
    }
    char base[128];
    snprintf(base, sizeof base, "http://%s", h);
    char now[32];
    iso_time(time(NULL), now);

    /* ---- device ---- */
    if (!strcmp(action, "GetSystemDateAndTime")) {
        time_t t = time(NULL);
        struct tm g;
        gmtime_r(&t, &g);
        snprintf(b, sizeof b, "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>NTP</tt:DateTimeType>"
                 "<tt:DaylightSavings>false</tt:DaylightSavings><tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone>"
                 "<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
                 "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime>"
                 "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
                 g.tm_hour, g.tm_min, g.tm_sec, g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    } else if (!strcmp(action, "GetDeviceInformation")) {
        char m[13];
        mac_hex(m, NULL);
        snprintf(b, sizeof b, "<tds:GetDeviceInformationResponse><tds:Manufacturer>LSC</tds:Manufacturer><tds:Model>Smart Connect indoor (ak_rtsp)</tds:Model>"
                 "<tds:FirmwareVersion>ak_rtsp %s</tds:FirmwareVersion><tds:SerialNumber>%s</tds:SerialNumber><tds:HardwareId>AK3918AV130</tds:HardwareId>"
                 "</tds:GetDeviceInformationResponse>", GIT_HASH, m);
    } else if (!strcmp(action, "GetCapabilities")) {
        snprintf(b, sizeof b, "<tds:GetCapabilitiesResponse><tds:Capabilities>"
                 "<tt:Device><tt:XAddr>%s/onvif/device_service</tt:XAddr><tt:Network><tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
                 "<tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network><tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve>"
                 "<tt:DiscoveryBye>false</tt:DiscoveryBye><tt:RemoteDiscovery>false</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup>"
                 "<tt:SystemLogging>false</tt:SystemLogging><tt:FirmwareUpgrade>false</tt:FirmwareUpgrade><tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>40</tt:Minor></tt:SupportedVersions></tt:System>"
                 "<tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2><tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration>"
                 "<tt:AccessPolicyConfig>false</tt:AccessPolicyConfig><tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken>"
                 "<tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken></tt:Security></tt:Device>"
                 "<tt:Events><tt:XAddr>%s/onvif/events_service</tt:XAddr><tt:WSSubscriptionPolicySupport>false</tt:WSSubscriptionPolicySupport>"
                 "<tt:WSPullPointSupport>true</tt:WSPullPointSupport><tt:WSPausableSubscriptionManagerInterfaceSupport>false</tt:WSPausableSubscriptionManagerInterfaceSupport></tt:Events>"
                 "<tt:Imaging><tt:XAddr>%s/onvif/imaging_service</tt:XAddr></tt:Imaging>"
                 "<tt:Media><tt:XAddr>%s/onvif/media_service</tt:XAddr><tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
                 "<tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media>"
                 "</tds:Capabilities></tds:GetCapabilitiesResponse>", base, base, base, base);
    } else if (!strcmp(action, "GetServices")) {
        snprintf(b, sizeof b, "<tds:GetServicesResponse>"
                 "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace><tds:XAddr>%s/onvif/device_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>40</tt:Minor></tds:Version></tds:Service>"
                 "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>%s/onvif/media_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>40</tt:Minor></tds:Version></tds:Service>"
                 "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/events/wsdl</tds:Namespace><tds:XAddr>%s/onvif/events_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>40</tt:Minor></tds:Version></tds:Service>"
                 "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace><tds:XAddr>%s/onvif/imaging_service</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version></tds:Service>"
                 "</tds:GetServicesResponse>", base, base, base, base);
    } else if (!strcmp(action, "GetServiceCapabilities")) {
        if (strstr(path, "events"))
            snprintf(b, sizeof b, "<tev:GetServiceCapabilitiesResponse><tev:Capabilities WSSubscriptionPolicySupport=\"false\" WSPullPointSupport=\"true\" "
                     "WSPausableSubscriptionManagerInterfaceSupport=\"false\" MaxNotificationProducers=\"0\" MaxPullPoints=\"%d\"/></tev:GetServiceCapabilitiesResponse>", MAX_SUBS);
        else if (strstr(path, "imaging"))
            snprintf(b, sizeof b, "<timg:GetServiceCapabilitiesResponse><timg:Capabilities ImageStabilization=\"false\">"
                     "</timg:Capabilities></timg:GetServiceCapabilitiesResponse>");
        else if (strstr(path, "media"))
            snprintf(b, sizeof b, "<trt:GetServiceCapabilitiesResponse><trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" OSD=\"true\">"
                     "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/><trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\"/>"
                     "</trt:Capabilities></trt:GetServiceCapabilitiesResponse>");
        else
            snprintf(b, sizeof b, "<tds:GetServiceCapabilitiesResponse><tds:Capabilities><tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"/>"
                     "<tds:Security UsernameToken=\"true\" HttpDigest=\"true\"/><tds:System DiscoveryResolve=\"false\" DiscoveryBye=\"false\" RemoteDiscovery=\"false\" SystemBackup=\"false\" "
                     "SystemLogging=\"false\" FirmwareUpgrade=\"false\"/></tds:Capabilities></tds:GetServiceCapabilitiesResponse>");
    } else if (!strcmp(action, "GetScopes")) {
        snprintf(b, sizeof b, "<tds:GetScopesResponse>"
                 "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/type/video_encoder</tt:ScopeItem></tds:Scopes>"
                 "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/Profile/Streaming</tt:ScopeItem></tds:Scopes>"
                 "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/hardware/AK3918</tt:ScopeItem></tds:Scopes>"
                 "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/%s</tt:ScopeItem></tds:Scopes>"
                 "</tds:GetScopesResponse>", sys_device_name_cn());
    } else if (!strcmp(action, "GetHostname")) {
        snprintf(b, sizeof b, "<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>%s</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>", sys_device_name_cn());
    } else if (!strcmp(action, "GetNetworkInterfaces")) {
        char m[13], ms[18], ip[16];
        mac_hex(m, ms);
        first_ip(ip);
        snprintf(b, sizeof b, "<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"wlan0\"><tt:Enabled>true</tt:Enabled>"
                 "<tt:Info><tt:Name>wlan0</tt:Name><tt:HwAddress>%s</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info>"
                 "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:FromDHCP><tt:Address>%s</tt:Address><tt:PrefixLength>24</tt:PrefixLength></tt:FromDHCP>"
                 "<tt:DHCP>true</tt:DHCP></tt:Config></tt:IPv4></tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>", ms, ip);
    } else if (!strcmp(action, "GetNTP")) {
        snprintf(b, sizeof b, "<tds:GetNTPResponse><tds:NTPInformation><tt:FromDHCP>false</tt:FromDHCP><tt:NTPManual><tt:Type>DNS</tt:Type><tt:DNSname>%s</tt:DNSname></tt:NTPManual></tds:NTPInformation></tds:GetNTPResponse>", sys_time_server());
    } else if (!strcmp(action, "SystemReboot")) {
        send_soap(cn, 200, "<tds:SystemRebootResponse><tds:Message>Rebooting</tds:Message></tds:SystemRebootResponse>");
        conn_shutdown_wr(cn);
        sync(); sleep(1);
        reboot(RB_AUTOBOOT);
        return 0;
    /* ---- media ---- */
    } else if (!strcmp(action, "GetProfiles")) {
        char p[5600];
        profile_xml(p, sizeof p);
        snprintf(b, sizeof b, "<trt:GetProfilesResponse>%s</trt:GetProfilesResponse>", p);
    } else if (!strcmp(action, "GetProfile")) {
        char p[5600];
        profile_xml_one(p, sizeof p, token_is_sub(body) && substream_running());
        /* single profile: rename the element */
        char *s1 = strstr(p, "<trt:Profiles"); if (s1) memcpy(s1, "<trt:Profile  ", 14);
        char *e1 = strstr(p, "</trt:Profiles>"); if (e1) memcpy(e1, "</trt:Profile >", 15);
        snprintf(b, sizeof b, "<trt:GetProfileResponse>%s</trt:GetProfileResponse>", p);
    } else if (!strcmp(action, "GetVideoSources")) {
        snprintf(b, sizeof b, "<trt:GetVideoSourcesResponse><trt:VideoSources token=\"vs\"><tt:Framerate>15</tt:Framerate>"
                 "<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution></trt:VideoSources></trt:GetVideoSourcesResponse>");
    } else if (!strcmp(action, "GetAudioSources")) {
        snprintf(b, sizeof b, "<trt:GetAudioSourcesResponse><trt:AudioSources token=\"as\"><tt:Channels>1</tt:Channels></trt:AudioSources></trt:GetAudioSourcesResponse>");
    } else if (!strcmp(action, "GetVideoSourceConfigurations") || !strcmp(action, "GetVideoSourceConfiguration")) {
        int one = !strcmp(action, "GetVideoSourceConfiguration");
        snprintf(b, sizeof b, "<trt:%sResponse><trt:Configuration%s token=\"vsc\"><tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>vs</tt:SourceToken>"
                 "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/></trt:Configuration%s></trt:%sResponse>", action, one ? "" : "s", one ? "" : "s", action);
    } else if (!strcmp(action, "GetVideoEncoderConfigurations") || !strcmp(action, "GetVideoEncoderConfiguration")) {
        char p[5600], *v1, *v2;
        profile_xml(p, sizeof p);
        v1 = strstr(p, "<tt:VideoEncoderConfiguration"); v2 = strstr(p, "</tt:VideoEncoderConfiguration>");
        if (v1 && v2) { v2[0] = 0; v1 += strlen("<tt:VideoEncoderConfiguration"); }
        int one = !strcmp(action, "GetVideoEncoderConfiguration");
        snprintf(b, sizeof b, "<trt:%sResponse><trt:Configuration%s%s</trt:Configuration%s></trt:%sResponse>", action, one ? "" : "s", v1 ? v1 : ">", one ? "" : "s", action);
    } else if (!strcmp(action, "GetAudioEncoderConfigurations") || !strcmp(action, "GetAudioSourceConfigurations")) {
        int enc = !strcmp(action, "GetAudioEncoderConfigurations");
        snprintf(b, sizeof b, enc
            ? "<trt:%sResponse><trt:Configurations token=\"aec\"><tt:Name>G711</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>G711</tt:Encoding><tt:Bitrate>64</tt:Bitrate><tt:SampleRate>8</tt:SampleRate>"
              "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
              "<tt:SessionTimeout>PT60S</tt:SessionTimeout></trt:Configurations></trt:%sResponse>"
            : "<trt:%sResponse><trt:Configurations token=\"asc\"><tt:Name>AudioSource</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>as</tt:SourceToken></trt:Configurations></trt:%sResponse>",
            action, action);
    } else if (!strcmp(action, "GetStreamUri")) {
        snprintf(b, sizeof b, "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://%s:554/%s</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                 "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>", h,
                 token_is_sub(body) && substream_running() ? "sub" : "");
    /* ---- imaging: HA's IR-lamp switch and brightness/saturation/contrast ---- */
    } else if (!strcmp(action, "GetImagingSettings") || !strcmp(action, "SetImagingSettings") ||
               !strcmp(action, "GetOptions")) {
        /* ONVIF schema order (Brightness, ColorSaturation, Contrast, ..., IrCutFilter):
         * zeep drops out-of-order elements, HA then sees them as None. */
        static const struct { const char *name, *key; } pic[] = {
            { "Brightness", "isp.brightness" }, { "ColorSaturation", "isp.saturation" }, { "Contrast", "isp.contrast" },
        };
        if (!strcmp(action, "GetOptions")) {
            snprintf(b, sizeof b, "<timg:GetOptionsResponse><timg:ImagingOptions>"
                     "<tt:Brightness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Brightness>"
                     "<tt:ColorSaturation><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:ColorSaturation>"
                     "<tt:Contrast><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Contrast>"
                     "</timg:ImagingOptions></timg:GetOptionsResponse>");
        } else if (!strcmp(action, "SetImagingSettings")) {
            for (size_t k = 0; k < sizeof pic / sizeof pic[0]; k++) {
                const char *p = strstr(body, pic[k].name);
                int v;
                if (p && (p = strchr(p, '>')) && sscanf(p + 1, "%d", &v) == 1 && v >= 0 && v <= 100) {
                    char line[64];                       /* our ISP sliders run -50..50 */
                    snprintf(line, sizeof line, "SET %s %d", pic[k].key, (v * 2 - 100) / 2);
                    control_exec(line, b, 64);
                }
            }
            const char *p = strstr(body, "IrCutFilter");
            if (p && (p = strchr(p, '>'))) {             /* ON = colour/day, OFF = IR/night */
                if (!strncmp(p + 1, "OFF", 3)) control_exec("SET night.mode night", b, 64);
                else if (!strncmp(p + 1, "ON", 2)) control_exec("SET night.mode day", b, 64);
                else if (!strncmp(p + 1, "AUTO", 4)) control_exec("SET night.mode auto", b, 64);
            }
            snprintf(b, sizeof b, "<timg:SetImagingSettingsResponse/>");
        } else {
            char fields[256] = "";
            size_t o = 0;
            for (size_t k = 0; k < sizeof pic / sizeof pic[0]; k++) {
                char line[64], out[64] = "";
                int isp = 0;
                snprintf(line, sizeof line, "GET %s", pic[k].key);
                control_exec(line, out, sizeof out);
                if (sscanf(out, "%*[^=]=%d", &isp) != 1) isp = 0;
                if (isp < -50) isp = -50; if (isp > 50) isp = 50;
                o += (size_t)snprintf(fields + o, sizeof fields - o, "<tt:%s>%d</tt:%s>",
                                      pic[k].name, isp + 50, pic[k].name);   /* inverse of the setter: -50..50 -> 0..100 */
            }
            night_tuning_t nt;
            night_get_tuning(&nt);
            snprintf(b, sizeof b, "<timg:GetImagingSettingsResponse><timg:ImagingSettings>%s<tt:IrCutFilter>%s</tt:IrCutFilter>"
                     "</timg:ImagingSettings></timg:GetImagingSettingsResponse>", fields,
                     nt.override == NIGHT_MODE_FORCE_DAY ? "ON" : nt.override == NIGHT_MODE_FORCE_NIGHT ? "OFF" : "AUTO");
        }
    } else if (!strcmp(action, "GetSnapshotUri")) {
        snprintf(b, sizeof b, "<trt:GetSnapshotUriResponse><trt:MediaUri><tt:Uri>%s/snapshot.jpg</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                 "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetSnapshotUriResponse>", base);
    /* ---- events ---- */
    } else if (!strcmp(action, "GetEventProperties")) {
        snprintf(b, sizeof b, "<tev:GetEventPropertiesResponse>"
                 "<tev:TopicNamespaceLocation>http://www.onvif.org/onvif/ver10/topics/topicns.xml</tev:TopicNamespaceLocation>"
                 "<wsnt:FixedTopicSet>true</wsnt:FixedTopicSet><wstop:TopicSet>"
                 "<tns1:RuleEngine><CellMotionDetector><Motion wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source>"
                 "<tt:SimpleItemDescription Name=\"VideoSourceConfigurationToken\" Type=\"tt:ReferenceToken\"/><tt:SimpleItemDescription Name=\"VideoAnalyticsConfigurationToken\" Type=\"tt:ReferenceToken\"/>"
                 "<tt:SimpleItemDescription Name=\"Rule\" Type=\"xsd:string\"/></tt:Source><tt:Data><tt:SimpleItemDescription Name=\"IsMotion\" Type=\"xsd:boolean\"/></tt:Data>"
                 "</tt:MessageDescription></Motion></CellMotionDetector></tns1:RuleEngine>"
                 "<tns1:VideoSource><MotionAlarm wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source>"
                 "<tt:Data><tt:SimpleItemDescription Name=\"State\" Type=\"xsd:boolean\"/></tt:Data></tt:MessageDescription></MotionAlarm>"
                 "<GlobalSceneChange><ImagingService wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source>"
                 "<tt:Data><tt:SimpleItemDescription Name=\"State\" Type=\"xsd:boolean\"/></tt:Data></tt:MessageDescription></ImagingService></GlobalSceneChange></tns1:VideoSource>"
                 "<tns1:AudioAnalytics><Audio><DetectedSound wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription Name=\"AudioSourceConfigurationToken\" Type=\"tt:ReferenceToken\"/></tt:Source>"
                 "<tt:Data><tt:SimpleItemDescription Name=\"IsSoundDetected\" Type=\"xsd:boolean\"/></tt:Data></tt:MessageDescription></DetectedSound></Audio></tns1:AudioAnalytics>"
                 "<tns1:Monitoring><ProcessorUsage wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source>"
                 "<tt:Data><tt:SimpleItemDescription Name=\"Value\" Type=\"xsd:float\"/></tt:Data></tt:MessageDescription></ProcessorUsage>"
                 "<Temperature wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source>"
                 "<tt:Data><tt:SimpleItemDescription Name=\"Value\" Type=\"xsd:float\"/></tt:Data></tt:MessageDescription></Temperature>"
                 "<OperatingTime><LastReboot wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Data><tt:SimpleItemDescription Name=\"UtcTime\" Type=\"xsd:dateTime\"/></tt:Data></tt:MessageDescription></LastReboot>"
                 "<LastClockSynchronization wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Data><tt:SimpleItemDescription Name=\"UtcTime\" Type=\"xsd:dateTime\"/></tt:Data></tt:MessageDescription></LastClockSynchronization>"
                 "</OperatingTime></tns1:Monitoring>"
                 "<tns1:Device><HardwareFailure><StorageFailure wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source>"
                 "<tt:Data><tt:SimpleItemDescription Name=\"Value\" Type=\"xsd:boolean\"/></tt:Data></tt:MessageDescription></StorageFailure></HardwareFailure></tns1:Device>"
                 "<tns1:RecordingConfig><JobState wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/></tt:Source>"
                 "<tt:Data><tt:SimpleItemDescription Name=\"State\" Type=\"tt:RecordingJobState\"/></tt:Data></tt:MessageDescription></JobState></tns1:RecordingConfig>"
                 "</wstop:TopicSet>"
                 "<wsnt:TopicExpressionDialect>http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet</wsnt:TopicExpressionDialect>"
                 "<wsnt:TopicExpressionDialect>http://docs.oasis-open.org/wsn/t-1/TopicExpression/Concrete</wsnt:TopicExpressionDialect>"
                 "<tev:MessageContentFilterDialect>http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter</tev:MessageContentFilterDialect>"
                 "<tev:MessageContentSchemaLocation>http://www.onvif.org/onvif/ver10/schema/onvif.xsd</tev:MessageContentSchemaLocation>"
                 "</tev:GetEventPropertiesResponse>");
    } else if (!strcmp(action, "CreatePullPointSubscription")) {
        char itt[40] = "";
        el_text(body, "InitialTerminationTime", itt, sizeof itt);
        long life = dur_seconds(itt, 60);
        if (life > 3600) life = 3600;
        pthread_mutex_lock(&g_ev_lock);
        time_t now_t = time(NULL);
        sub_t *s = NULL;
        for (int i = 0; i < MAX_SUBS && !s; i++) if (!g_subs[i].used || g_subs[i].expires < now_t) s = &g_subs[i];
        if (!s) {                                       /* replace the one expiring first */
            s = &g_subs[0];
            for (int i = 1; i < MAX_SUBS; i++) if (g_subs[i].expires < s->expires) s = &g_subs[i];
        }
        memset(s, 0, sizeof *s);
        s->used = 1; s->id = g_next_id++; s->expires = now_t + life;
        push_state(s, 1);
        unsigned id = s->id;
        time_t term = s->expires;
        pthread_mutex_unlock(&g_ev_lock);
        char tt[32];
        iso_time(term, tt);
        snprintf(b, sizeof b, "<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference><wsa:Address>%s/onvif/subscription?id=%u</wsa:Address></tev:SubscriptionReference>"
                 "<wsnt:CurrentTime>%s</wsnt:CurrentTime><wsnt:TerminationTime>%s</wsnt:TerminationTime></tev:CreatePullPointSubscriptionResponse>", base, id, now, tt);
    } else if (!strcmp(action, "PullMessages") || !strcmp(action, "Renew") || !strcmp(action, "Unsubscribe") || !strcmp(action, "SetSynchronizationPoint")) {
        const char *q = strstr(path, "id=");
        unsigned id = q ? (unsigned)strtoul(q + 3, NULL, 10) : 0;
        pthread_mutex_lock(&g_ev_lock);
        sub_t *s = NULL;
        for (int i = 0; i < MAX_SUBS; i++) if (g_subs[i].used && g_subs[i].id == id && g_subs[i].expires >= time(NULL)) s = &g_subs[i];
        if (!s) { pthread_mutex_unlock(&g_ev_lock); fault(cn, "InvalidArgVal", "unknown or expired subscription"); return 0; }
        if (!strcmp(action, "Unsubscribe")) {
            s->used = 0;
            pthread_mutex_unlock(&g_ev_lock);
            send_soap(cn, 200, "<wsnt:UnsubscribeResponse/>");
            return 0;
        }
        if (!strcmp(action, "SetSynchronizationPoint")) {
            push_state(s, 1);
            pthread_cond_broadcast(&g_ev_cond);
            pthread_mutex_unlock(&g_ev_lock);
            send_soap(cn, 200, "<tev:SetSynchronizationPointResponse/>");
            return 0;
        }
        if (!strcmp(action, "Renew")) {
            char tt_s[40] = "";
            el_text(body, "TerminationTime", tt_s, sizeof tt_s);
            long life = dur_seconds(tt_s, 60);
            if (life > 3600) life = 3600;
            s->expires = time(NULL) + life;
            char tt[32];
            iso_time(s->expires, tt);
            pthread_mutex_unlock(&g_ev_lock);
            snprintf(b, sizeof b, "<wsnt:RenewResponse><wsnt:TerminationTime>%s</wsnt:TerminationTime><wsnt:CurrentTime>%s</wsnt:CurrentTime></wsnt:RenewResponse>", tt, now);
            send_soap(cn, 200, b);
            return 0;
        }
        /* PullMessages: a pull also extends the subscription a little */
        if (s->expires < time(NULL) + 60) s->expires = time(NULL) + 60;
        pthread_mutex_unlock(&g_ev_lock);
        char tos[40] = "", lim[16] = "";
        el_text(body, "Timeout", tos, sizeof tos);
        el_text(body, "MessageLimit", lim, sizeof lim);
        pull_t *p = calloc(1, sizeof *p);
        if (!p) { fault(cn, "Action", "out of memory"); return 0; }
        p->cn = cn; p->id = id; p->timeout = dur_seconds(tos, 10); p->limit = atoi(lim) > 0 ? atoi(lim) : 10;
        if (p->timeout > 20) p->timeout = 20;            /* keep long polls short */
        if (pulls_add(1) > MAX_PULLS) {   /* answer now instead of waiting */
            pulls_add(-1);
            pull_respond(cn, id, 10); free(p); return 0;
        }
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 96 * 1024);
        int rc = pthread_create(&t, &at, pull_thread, p);
        pthread_attr_destroy(&at);
        if (rc != 0) { pulls_add(-1); free(p); pull_respond(cn, id, 10); return 0; }
        return 1;
    } else {
        fault(cn, "ActionNotSupported", "action not supported");
        return 0;
    }
    send_soap(cn, 200, b);
    return 0;
}

/* ---- WS-Discovery ------------------------------------------------------------------- */
static void discovery_msg(char *out, size_t cap, const char *action, const char *relates, const char *ip)
{
    char uuid[48], msgid[64], m[13];
    device_uuid(uuid);
    mac_hex(m, NULL);
    snprintf(msgid, sizeof msgid, "urn:uuid:%08lx-4c53-4c43-a3e1-%s", (unsigned long)(mono_ms() & 0xffffffff), m);
    int match = !strcmp(action, "ProbeMatches");
    snprintf(out, cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><env:Envelope xmlns:env=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<env:Header><wsa:MessageID>%s</wsa:MessageID>%s%s%s<wsa:To>%s</wsa:To>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/%s</wsa:Action>"
        "<d:AppSequence InstanceId=\"1\" MessageNumber=\"1\"/></env:Header><env:Body>%s"
        "<d:%s><wsa:EndpointReference><wsa:Address>%s</wsa:Address></wsa:EndpointReference>"
        "<d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>"
        "<d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/hardware/AK3918 onvif://www.onvif.org/name/%s</d:Scopes>"
        "<d:XAddrs>http://%s/onvif/device_service</d:XAddrs><d:MetadataVersion>1</d:MetadataVersion></d:%s>%s</env:Body></env:Envelope>",
        msgid, relates ? "<wsa:RelatesTo>" : "", relates ? relates : "", relates ? "</wsa:RelatesTo>" : "",
        match ? "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous" : "urn:schemas-xmlsoap-org:ws:2005:04:discovery",
        action, match ? "<d:ProbeMatches>" : "", match ? "ProbeMatch" : "Hello", uuid, sys_device_name_cn(), ip,
        match ? "ProbeMatch" : "Hello", match ? "</d:ProbeMatches>" : "");
}

static void *sys_thread(void *arg)
{
    (void)arg;
    int tick = 0;
    for (;;) {
        if (s_enabled) {
            char rs[512];
            if (record_status_json(rs, sizeof rs) > 0) {
                char *p;
                uint64_t tot = 0, fr = 0;
                if ((p = strstr(rs, "\"total_mb\":"))) tot = strtoull(p + 11, NULL, 10);
                if ((p = strstr(rs, "\"free_mb\":"))) fr = strtoull(p + 10, NULL, 10);
                onvif_sys_state((int)sys_temp_c10(), strstr(rs, "\"sd\":true") != NULL, fr, tot);
                onvif_recording(p && (p = strstr(rs, "\"recording\":\"")) && p[13] != '"');
            }
            if (++tick >= 60) { tick = 0; sys_tick(); }
        }
        sleep(1);
    }
    return NULL;
}

static void *discovery_thread(void *arg)
{
    (void)arg;
    int fd = -1, said_hello = 0;
    char ip[16] = "", buf[4096], out[4096];
    for (;;) {
        if (!s_enabled) { if (fd >= 0) { close(fd); fd = -1; } said_hello = 0; sleep(2); continue; }
        if (fd < 0) {
            first_ip(ip);
            if (!ip[0]) { sleep(5); continue; }
            fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            struct sockaddr_in a = { 0 };
            a.sin_family = AF_INET; a.sin_port = htons(3702); a.sin_addr.s_addr = INADDR_ANY;
            struct ip_mreq mr;
            mr.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
            inet_pton(AF_INET, ip, &mr.imr_interface);
            if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0) {
                perror("[onvif] discovery socket");
                close(fd); fd = -1; sleep(10); continue;
            }
            struct timeval tv = { 5, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            printf("[onvif] WS-Discovery on %s:3702\n", ip);
        }
        if (!said_hello) {
            discovery_msg(out, sizeof out, "Hello", NULL, ip);
            struct sockaddr_in to = { 0 };
            to.sin_family = AF_INET; to.sin_port = htons(3702); to.sin_addr.s_addr = inet_addr("239.255.255.250");
            sendto(fd, out, strlen(out), 0, (struct sockaddr *)&to, sizeof to);
            said_hello = 1;
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            char now_ip[16];
            first_ip(now_ip);
            if (strcmp(now_ip, ip)) { close(fd); fd = -1; said_hello = 0; }   /* address changed: rejoin */
            continue;
        }
        buf[n] = 0;
        {   /* no reflection: private/link-local senders, at most ~10 replies a second */
            uint32_t a = ntohl(from.sin_addr.s_addr);
            int local = (a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8 || (a >> 16) == 0xa9fe;
            static uint32_t win; static int cnt;
            uint32_t now_s = mono_s();
            if (now_s != win) { win = now_s; cnt = 0; }
            if (!local || ++cnt > 10) continue;
        }
        if (!find_el(buf, "Probe") || !find_el(buf, "MessageID")) continue;
        char types[200] = "";
        el_text(buf, "Types", types, sizeof types);
        if (types[0] && !strstr(types, "NetworkVideoTransmitter") && !strstr(types, "Device")) continue;
        char msgid[128] = "";
        el_text(buf, "MessageID", msgid, sizeof msgid);
        for (char *p = msgid; *p; p++) if (*p == '<' || *p == '&') *p = '_';
        discovery_msg(out, sizeof out, "ProbeMatches", msgid, ip);
        sendto(fd, out, strlen(out), 0, (struct sockaddr *)&from, fl);
    }
    return NULL;
}

void onvif_init(void) { CTRL_REG("onvif.enabled", p_en, CTRL_PERSIST); }

void onvif_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, discovery_thread, NULL);
    pthread_create(&t, &a, sys_thread, NULL);
    pthread_attr_destroy(&a);
}
