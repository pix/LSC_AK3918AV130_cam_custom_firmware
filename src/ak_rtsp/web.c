#include "web.h"
#include "auth.h"
#include "control.h"
#include "night.h"
#include "jpeg_enc.h"
#include "vi.h"
#include "monotime.h"
#include "system.h"
#include "netcfg.h"
#include "qrwifi.h"
#include "live.h"
#include "sha1.h"
#include "speaker.h"
#include "osd.h"
#include "osd_hw.h"
#include "conn.h"
#include "tls.h"
#include "events.h"
#include "record.h"
#include "onvif.h"
#include "snapshot.h"
#include "venc.h"
#include "rtsp.h"
#include <fcntl.h>
#include "mqtt.h"
#include "md.h"
#include "tamper.h"
#include "substream.h"
#include "backup.h"
#include "term.h"
#include "version_info.h"
#include "sounddet.h"
#include "isp.h"
#include "npu.h"
#include "aec.h"
#include "ctrl_params.h"
#include <arpa/inet.h>
#include <netinet/in.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <netinet/in.h>

#define MAX_HDR  8192
#define MAX_BODY 4096

extern const char web_index_html[];   /* web_page.c */
extern const char web_recordings_html[];   /* web_rec_page.c */
extern const char web_terminal_html[];     /* web_term_page.c */

typedef struct {
    char method[8];
    char path[256];
    char authz[1024];
    int  has_xrw;
    int  upgrade_ws;
    char ws_key[64];
    char ws_version[8];
    char origin[128];
    char host[128];
    long content_length;
    char body[MAX_BODY + 1];
    uint8_t *big;          /* /hook/ uploads larger than MAX_BODY (malloc'd) */
    size_t   big_len;
    char bearer[80];
    char range[64];
    char ctype[64];
    char xff[128], xfproto[8], xfhost[128];   /* X-Forwarded-* (used only from a trusted proxy) */
    size_t body_len;                          /* bytes in body[] (binary bodies may contain NULs) */
    int  via_proxy;                           /* set by serve(): peer is in web.proxies */
} req_t;

#define HOOK_MAX_BODY (768 * 1024)
#ifndef HOOK_TOKEN_FILE
#define HOOK_TOKEN_FILE "/etc/config/ak_rtsp_hook"
#endif

/* /api/osd: settings plus the burned-in (video) overlay state. */
static int osd_json_full(char *out, size_t cap)
{
    int n = osd_json(out, cap);
    if (n < 2 || (size_t)n >= cap) return n;
    n--;                                   /* drop the closing brace */
    n += snprintf(out + n, cap - (size_t)n, ",\"video\":\"%s\"}", osd_hw_state());
    return (size_t)n < cap ? n : (int)cap - 1;
}


/* ---- low-level I/O -------------------------------------------------------- */
static void respond(conn_t *cn, int code, const char *reason, const char *ctype,
                    const char *extra_hdr, const void *body, size_t blen)
{
    char h[768];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Frame-Options: DENY\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "%s"
        "Connection: close\r\n\r\n",
        code, reason, ctype, blen, extra_hdr ? extra_hdr : "");
    if (n < 0) return;
    if ((size_t)n >= sizeof(h)) n = (int)sizeof(h) - 1;
    if (conn_send_all(cn, h, (size_t)n) == 0 && blen)
        conn_send_all(cn, body, blen);
}

static void respond_text(conn_t *cn, int code, const char *reason, const char *text)
{
    respond(cn, code, reason, "text/plain; charset=utf-8", NULL, text, strlen(text));
}

static void respond_401(conn_t *cn, int stale)
{
    char chal[256], hdr[300];
    auth_digest_challenge(chal, sizeof(chal), 1, stale);
    snprintf(hdr, sizeof(hdr), "WWW-Authenticate: %s\r\n", chal);
    const char *msg = "Authentication required\n";
    respond(cn, 401, "Unauthorized", "text/plain; charset=utf-8", hdr, msg, strlen(msg));
}

/* Reads headers + body. Returns 0 ok, -1 on error/oversize. */
static int read_request_buf(conn_t *cn, req_t *r, char *buf);
static int read_request(conn_t *cn, req_t *r)
{
    char *buf = malloc(MAX_HDR + MAX_BODY + 1);
    if (!buf) return -1;
    int rc = read_request_buf(cn, r, buf);
    free(buf);
    return rc;
}

/* A request must arrive within an overall time, not just per recv (a client
 * trickling bytes would otherwise hold a connection slot for hours). */
#define HDR_DEADLINE_MS  10000
static int read_request_buf(conn_t *cn, req_t *r, char *buf)
{
    size_t len = 0;
    char *hend = NULL;
    int64_t deadline = mono_ms() + HDR_DEADLINE_MS;

    if (r->big) free(r->big);   /* defensive: never leak a previous upload */
    memset(r, 0, sizeof(*r));
    while (!hend) {
        if (len >= MAX_HDR || mono_ms() > deadline) return -1;
        ssize_t n = conn_recv(cn, buf + len, MAX_HDR - len);
        if (n <= 0) return -1;
        len += (size_t)n;
        buf[len] = 0;
        hend = strstr(buf, "\r\n\r\n");
    }
    *hend = 0;
    char *body = hend + 4;
    size_t have_body = len - (size_t)(body - buf);

    char *save = NULL;
    char *line = strtok_r(buf, "\r\n", &save);
    if (!line) return -1;
    char ver[16];
    if (sscanf(line, "%7s %255s %15s", r->method, r->path, ver) != 3) return -1;

    while ((line = strtok_r(NULL, "\r\n", &save))) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(line, "Authorization")) {
            snprintf(r->authz, sizeof(r->authz), "%s", v);
            if (!strncasecmp(v, "Bearer ", 7)) snprintf(r->bearer, sizeof(r->bearer), "%s", v + 7);
        }
        else if (!strcasecmp(line, "Content-Length"))
            r->content_length = strtol(v, NULL, 10);
        else if (!strcasecmp(line, "X-Requested-With") && !strcmp(v, "lsc"))
            r->has_xrw = 1;
        else if (!strcasecmp(line, "Upgrade") && !strcasecmp(v, "websocket"))
            r->upgrade_ws = 1;
        else if (!strcasecmp(line, "Sec-WebSocket-Key"))
            snprintf(r->ws_key, sizeof(r->ws_key), "%s", v);
        else if (!strcasecmp(line, "Sec-WebSocket-Version"))
            snprintf(r->ws_version, sizeof(r->ws_version), "%s", v);
        else if (!strcasecmp(line, "Origin"))
            snprintf(r->origin, sizeof(r->origin), "%s", v);
        else if (!strcasecmp(line, "Host"))
            snprintf(r->host, sizeof(r->host), "%s", v);
        else if (!strcasecmp(line, "Range"))
            snprintf(r->range, sizeof(r->range), "%s", v);
        else if (!strcasecmp(line, "Content-Type"))
            snprintf(r->ctype, sizeof(r->ctype), "%s", v);
        else if (!strcasecmp(line, "X-Forwarded-For")) {
            const char *last = strrchr(v, ',');               /* the proxy appends the client it saw */
            last = last ? last + 1 : v;
            while (*last == ' ') last++;
            snprintf(r->xff, sizeof(r->xff), "%s", last);
        }
        else if (!strcasecmp(line, "X-Forwarded-Proto"))
            snprintf(r->xfproto, sizeof(r->xfproto), "%s", v);
        else if (!strcasecmp(line, "X-Forwarded-Host"))
            snprintf(r->xfhost, sizeof(r->xfhost), "%s", v);
    }
    deadline = mono_ms() + HDR_DEADLINE_MS + (int64_t)(r->content_length > 0 ? r->content_length / 8 : 0);  /* >= 8 KB/s */

    if (r->content_length < 0) return -1;
    size_t want = (size_t)r->content_length;
    if (want > MAX_BODY) {
        /* only webhook uploads may be large */
        if ((strncmp(r->path, "/hook/", 6) != 0 && strncmp(r->path, "/api/mqtt/upload", 16) != 0 &&
             strncmp(r->path, "/onvif/", 7) != 0 && strncmp(r->path, "/api/rec/", 9) != 0 &&
             strncmp(r->path, "/api/restore", 12) != 0) ||
            want > HOOK_MAX_BODY || (!strncmp(r->path, "/onvif/", 7) && want > 16384)) return -1;
        r->big = malloc(want + 1);
        if (!r->big) return -1;
        size_t got = have_body > want ? want : have_body;
        memcpy(r->big, body, got);
        while (got < want) {
            if (mono_ms() > deadline) { free(r->big); r->big = NULL; return -1; }
            ssize_t n = conn_recv(cn, r->big + got, want - got);
            if (n <= 0) { free(r->big); r->big = NULL; return -1; }
            got += (size_t)n;
        }
        r->big_len = want;
        r->big[want] = 0;                      /* text bodies (SOAP) are used as C strings */
        r->body[0] = 0;
        return 0;
    }
    if (have_body > want) have_body = want;
    memcpy(r->body, body, have_body);
    while (have_body < want) {
        if (mono_ms() > deadline) return -1;
        ssize_t n = conn_recv(cn, r->body + have_body, want - have_body);
        if (n <= 0) return -1;
        have_body += (size_t)n;
    }
    r->body[have_body] = 0;
    r->body_len = have_body;
    return 0;
}

/* application/x-www-form-urlencoded field lookup + decode. */
static int form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            p += klen + 1;
            size_t n = 0;
            while (*p && *p != '&') {
                int c = (unsigned char)*p;
                if (c == '+') c = ' ';
                else if (c == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    char hx[3] = { p[1], p[2], 0 };
                    c = (int)strtol(hx, NULL, 16);
                    p += 2;
                }
                if (n + 1 < cap) out[n++] = (char)c;
                p++;
            }
            out[n] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

static int safe_token(const char *s)  /* param names / values for SET */
{
    if (!*s || strlen(s) > 48) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '.' || *s == '_' || *s == '-' || *s == '/')) return 0;
    return 1;
}

/* ---- handlers --------------------------------------------------------------- */
static void h_snapshot(conn_t *cn, int zoom)
{
    static uint8_t nv12[640 * 368 * 3 / 2];
    static uint8_t jpg[200 * 1024];
    int w, h;
    if (zoom) {   /* QR scanner's 3x centre view (luminance only) */
        int n = vi_copy_td_luma(nv12, sizeof(nv12) * 2 / 3, &w, &h);   /* leave room for chroma */
        if (n > 0 && (size_t)n + (size_t)n / 2 <= sizeof(nv12)) memset(nv12 + n, 128, (size_t)n / 2);
        else zoom = 0;
    }
    if (!zoom && vi_copy_sub_frame(nv12, sizeof(nv12), &w, &h) < 0) {
        respond_text(cn, 503, "Unavailable", "no frame\n");
        return;
    }
    int n = jpeg_encode_nv12(nv12, w, h, zoom ? 80 : snapshot_quality(), jpg, (int)sizeof(jpg));
    if (n < 0) { respond_text(cn, 500, "Error", "encode failed\n"); return; }
    respond(cn, 200, "OK", "image/jpeg", NULL, jpg, (size_t)n);
}

static void h_status(conn_t *cn)
{
    char user[40], json[512];
    auth_get_user(user, sizeof(user));
    snprintf(json, sizeof(json),
             "{\"name\":\"%s\",\"user\":\"%s\",\"default_login\":%s,\"night\":\"%s\",\"uptime_s\":%ld,"
             "\"kbps_now\":%d,\"kbps_set\":%d,\"gop_set\":%d,\"venc_note\":\"%s\",\"ev\":%d,\"light\":%d,\"non_ir\":%d,\"fps\":%d,\"smart\":%d}",
             sys_device_name(), user, auth_is_default() ? "true" : "false",
             night_is_night() ? "night" : "day", (long)mono_s(), rtsp_measured_kbps(), venc_kbps_now(), venc_gop_now(), venc_note(), night_ev(), night_light_pct(), night_non_ir_ratio(), night_fps_now(), venc_smart_now());
    respond(cn, 200, "OK", "application/json", NULL, json, strlen(json));
}

static void h_params(conn_t *cn)
{
    char out[1536];
    int n = control_exec("LIST", out, sizeof(out));
    respond(cn, 200, "OK", "text/plain; charset=utf-8", NULL, out, (size_t)n);
}

static void h_set(conn_t *cn, const req_t *r)
{
    char name[64], value[64], cmd[160], out[256];
    if (!form_get(r->body, "name", name, sizeof(name)) ||
        !form_get(r->body, "value", value, sizeof(value)) ||
        !safe_token(name) || !safe_token(value)) {
        respond_text(cn, 400, "Bad Request", "ERR bad name/value\n");
        return;
    }
    snprintf(cmd, sizeof(cmd), "SET %s %s", name, value);
    int n = control_exec(cmd, out, sizeof(out));
    int ok = !strncmp(out, "OK", 2);
    respond(cn, ok ? 200 : 400, ok ? "OK" : "Bad Request",
            "text/plain; charset=utf-8", NULL, out, (size_t)n);
}

static int g_penalty;                       /* set under g_web_lock; serve() sleeps after unlocking */
static const struct sockaddr *g_peer;       /* client address of the request being handled */

static void h_credentials(conn_t *cn, const req_t *r)
{
    char cur[80], user[48], pass[80], curuser[40];
    const char *err = "missing fields";
    if (!form_get(r->body, "current", cur, sizeof(cur)) ||
        !form_get(r->body, "user", user, sizeof(user)) ||
        !form_get(r->body, "pass", pass, sizeof(pass))) {
        respond_text(cn, 400, "Bad Request", err);
        return;
    }
    auth_get_user(curuser, sizeof(curuser));
    if (!auth_check_plain(curuser, cur)) {
        auth_record(g_peer, 0);
        g_penalty = 1;
        respond_text(cn, 403, "Forbidden", "current password is wrong");
        return;
    }
    if (auth_set(user, pass, &err) != 0) {
        respond_text(cn, 400, "Bad Request", err);
        return;
    }
    respond_text(cn, 200, "OK", "credentials changed");
}

static void h_json(conn_t *cn, int (*fn)(char *, size_t))
{
    static char buf[6144];
    int n = fn(buf, sizeof(buf));
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    respond(cn, 200, "OK", "application/json", NULL, buf, (size_t)n);
}

static void h_wifi_connect(conn_t *cn, const req_t *r)
{
    char ssid[64], psk[96];
    const char *err = "missing ssid";
    if (!form_get(r->body, "ssid", ssid, sizeof(ssid))) { respond_text(cn, 400, "Bad Request", err); return; }
    if (!form_get(r->body, "psk", psk, sizeof(psk))) psk[0] = 0;
    if (net_connect(ssid, psk, &err) != 0) { respond_text(cn, 400, "Bad Request", err); return; }
    memset(psk, 0, sizeof(psk));
    respond_text(cn, 202, "Accepted",
        "Trying the new network for up to 45 s. If it fails, the previous network is restored.");
}

static void h_ap_start(conn_t *cn, const req_t *r)
{
    char m[8] = "15";
    form_get(r->body, "minutes", m, sizeof(m));
    if (net_ap_start(atoi(m), "started from web page") != 0) {
        respond_text(cn, 409, "Conflict", "a network change is already in progress");
        return;
    }
    respond_text(cn, 202, "Accepted", "Access point starting; this page will stop responding on your home network.");
}

static void h_ap_password(conn_t *cn, const req_t *r)
{
    char pass[96];
    const char *err = "missing password";
    if (!form_get(r->body, "password", pass, sizeof(pass)) || net_ap_set_password(pass, &err) != 0) {
        respond_text(cn, 400, "Bad Request", err);
        return;
    }
    respond_text(cn, 200, "OK", "AP password saved");
}

/* ---- webhook token (stored 0600 on CONFIG) ------------------------------- */
static int hook_token(char out[33], int regenerate)
{
    if (!regenerate) {
        FILE *f = fopen(HOOK_TOKEN_FILE, "r");
        if (f) {
            char b[64] = "";
            if (!fgets(b, sizeof(b), f)) b[0] = 0;
            fclose(f);
            b[strcspn(b, "\r\n")] = 0;
            if (strlen(b) == 32) { memcpy(out, b, 33); return 0; }
        }
    }
    if (live_token_new(out) != 0) return -1;       /* reuse its /dev/urandom hex generator */
    live_token_take(out);                          /* don't leave it valid as a live token */
    char tmp[] = HOOK_TOKEN_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", out);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    chmod(tmp, 0600);
    return rename(tmp, HOOK_TOKEN_FILE);
}

static int hook_token_ok(const char *given)
{
    char t[33];
    if (!given || strlen(given) != 32 || hook_token(t, 0) != 0) return 0;
    unsigned char d = 0;
    for (int i = 0; i < 32; i++) d |= (unsigned char)(t[i] ^ given[i]);
    return d == 0;
}

static void h_hook_sound(conn_t *cn, req_t *r)
{
    char qtok[40] = "", name[16] = "";
    const char *q = strchr(r->path, '?');
    if (q) {
        form_get(q + 1, "token", qtok, sizeof(qtok));
        form_get(q + 1, "name", name, sizeof(name));
    }
    if (!name[0] && r->body[0]) form_get(r->body, "name", name, sizeof(name));
    if (!hook_token_ok(r->bearer[0] ? r->bearer : qtok)) {
        g_penalty = 1;
        respond_text(cn, 403, "Forbidden", "bad or missing token\n");
        return;
    }
    if (!speaker_ready()) { respond_text(cn, 503, "Unavailable", "speaker not available\n"); return; }
    if (r->big || (r->body[0] && !strncmp(r->body, "RIFF", 4))) {
        const uint8_t *d = r->big ? r->big : (const uint8_t *)r->body;
        size_t n = r->big ? r->big_len : (size_t)r->content_length;
        const char *err;
        speaker_stop();
        if (speaker_play_wav(d, n, &err) != 0) { respond_text(cn, 400, "Bad Request", err); return; }
        respond_text(cn, 200, "OK", "playing uploaded WAV\n");
        return;
    }
    if (!name[0]) snprintf(name, sizeof(name), "chime");
    speaker_stop();
    if (speaker_play_builtin(name) != 0) { respond_text(cn, 404, "Not Found", "unknown sound (chime, custom, tone)\n"); return; }
    respond_text(cn, 200, "OK", "playing\n");
}

/* ---- recordings: files are sent from their own thread (with HTTP Range) so a
 * long download doesn't hold up the rest of the admin page ---- */
typedef struct { conn_t *cn; int fd; long long start, len, total; int partial, attach; char ctype[24], name[48]; } send_job_t;
static int g_senders;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

static void *file_sender(void *arg)
{
    send_job_t *j = arg;
    char h[512];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\n"
        "Cache-Control: private, max-age=3600\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Disposition: %s; filename=\"%s\"\r\n",
        j->partial ? "206 Partial Content" : "200 OK", j->ctype, j->len, j->attach ? "attachment" : "inline", j->name);
    if (j->partial) n += snprintf(h + n, sizeof(h) - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", j->start, j->start + j->len - 1, j->total);
    n += snprintf(h + n, sizeof(h) - (size_t)n, "Connection: close\r\n\r\n");
    if ((size_t)n >= sizeof(h)) n = (int)sizeof(h) - 1;
    struct timeval tv = { 15, 0 };
    setsockopt(j->cn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (conn_send_all(j->cn, h, (size_t)n) == 0) {
        static __thread char buf[32768];
        lseek(j->fd, (off_t)j->start, SEEK_SET);
        long long left = j->len;
        while (left > 0) {
            ssize_t r = read(j->fd, buf, left < (long long)sizeof(buf) ? (size_t)left : sizeof(buf));
            if (r <= 0 || conn_send_all(j->cn, buf, (size_t)r) != 0) break;
            left -= r;
        }
    }
    close(j->fd);
    conn_close(j->cn);
    free(j);
    pthread_mutex_lock(&g_send_lock); g_senders--; pthread_mutex_unlock(&g_send_lock);
    return NULL;
}

/* Returns 1 if the connection was handed to a sender thread. */
static int send_recording(conn_t *cn, const char *day, const char *name, const char *range, int attach)
{
    char path[256];
    if (record_path(day, name, path, sizeof(path)) != 0) { respond_text(cn, 404, "Not Found", "not found\n"); return 0; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); respond_text(cn, 404, "Not Found", "not found\n"); return 0; }
    long long total = (long long)st.st_size, start = 0, end = total - 1;
    int partial = 0;
    /* small files (event pictures, thumbnails) go out right here: a page full of
     * thumbnails must not run into the limit on concurrent video downloads */
    if (total <= 300 * 1024 && !range[0]) {
        char *buf = malloc((size_t)total + 1);
        ssize_t got = buf ? read(fd, buf, (size_t)total) : -1;
        close(fd);
        if (got != (ssize_t)total) { free(buf); respond_text(cn, 500, "Error", "read failed\n"); return 0; }
        char hdr[200];
        snprintf(hdr, sizeof(hdr), "Content-Disposition: %s; filename=\"%s_%s\"\r\nAccept-Ranges: bytes\r\n",
                 attach ? "attachment" : "inline", day, name);
        respond(cn, 200, "OK", strstr(name, ".jpg") ? "image/jpeg" : "video/mp4", hdr, buf, (size_t)total);
        free(buf);
        return 0;
    }
    if (range[0] && !strncmp(range, "bytes=", 6)) {
        long long a = -1, b = -1;
        if (sscanf(range + 6, "%lld-%lld", &a, &b) >= 1 && a >= 0 && a < total) {
            start = a; if (b >= a && b < total) end = b;
            partial = 1;
        } else if (range[6] == '-' && sscanf(range + 7, "%lld", &b) == 1 && b > 0) {
            start = total > b ? total - b : 0; partial = 1;
        } else {
            close(fd);
            respond(cn, 416, "Range Not Satisfiable", "text/plain", NULL, "bad range\n", 10);
            return 0;
        }
    }
    pthread_mutex_lock(&g_send_lock);
    int ok = g_senders < 3;
    if (ok) g_senders++;
    pthread_mutex_unlock(&g_send_lock);
    send_job_t *j = ok ? calloc(1, sizeof *j) : NULL;
    if (!j) {
        if (ok) { pthread_mutex_lock(&g_send_lock); g_senders--; pthread_mutex_unlock(&g_send_lock); }
        close(fd);
        respond_text(cn, 503, "Busy", "too many downloads, try again\n");
        return 0;
    }
    j->cn = cn; j->fd = fd; j->start = start; j->len = end - start + 1; j->total = total; j->partial = partial; j->attach = attach;
    snprintf(j->ctype, sizeof(j->ctype), "%s", strstr(name, ".jpg") ? "image/jpeg" : "video/mp4");
    snprintf(j->name, sizeof(j->name), "%s_%s", day, name);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int rc = pthread_create(&t, &at, file_sender, j);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        close(fd); free(j);
        pthread_mutex_lock(&g_send_lock); g_senders--; pthread_mutex_unlock(&g_send_lock);
        respond_text(cn, 503, "Busy", "cannot start download\n");
        return 0;
    }
    return 1;
}

/* ---- several recordings as one .tar (POST the list -> one-time token -> GET) ---- */
typedef struct { char tok[33]; time_t exp; int n; char (*path)[48]; long long bytes; } tarjob_t;
static tarjob_t g_tar;                       /* one pending archive at a time */

static long long tar_size_of(long long sz) { return 512 + (sz + 511) / 512 * 512; }

static void tar_header(char h[512], const char *name, long long size, time_t mtime)
{
    memset(h, 0, 512);
    snprintf(h, 100, "%s", name);
    memcpy(h + 100, "0000644", 8);
    memcpy(h + 108, "0000000", 8);
    memcpy(h + 116, "0000000", 8);
    snprintf(h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf(h + 136, 12, "%011llo", (unsigned long long)mtime);
    memset(h + 148, ' ', 8);
    h[156] = '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memcpy(h + 265, "root", 5);
    memcpy(h + 297, "root", 5);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)h[i];
    snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
}

typedef struct { conn_t *cn; tarjob_t job; } tar_send_t;
static void *tar_sender(void *arg)
{
    tar_send_t *t = arg;
    char h[512];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 200 OK\r\nContent-Type: application/x-tar\r\nContent-Length: %lld\r\n"
        "Content-Disposition: attachment; filename=\"camera-recordings.tar\"\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", t->job.bytes);
    struct timeval tv = { 30, 0 };
    setsockopt(t->cn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int ok = conn_send_all(t->cn, h, (size_t)n) == 0;
    static __thread char buf[32768];
    for (int i = 0; ok && i < t->job.n; i++) {
        char full[256], day[16], *slash = strchr(t->job.path[i], '/');
        snprintf(day, sizeof(day), "%.*s", (int)(slash - t->job.path[i]), t->job.path[i]);
        record_path(day, slash + 1, full, sizeof(full));
        int fd = open(full, O_RDONLY | O_CLOEXEC);
        struct stat st;
        long long sz = 0;
        time_t mt = time(NULL);
        if (fd >= 0 && fstat(fd, &st) == 0) { sz = (long long)st.st_size; mt = st.st_mtime; }
        /* sizes were fixed when the token was made; if a file changed since
         * (deleted to free space), its stored entry keeps that size, zero-filled */
        long long want = *(long long *)(t->job.path[i] + 40);
        tar_header(h, t->job.path[i], want, mt);
        ok = conn_send_all(t->cn, h, 512) == 0;
        long long left = want;
        while (ok && left > 0) {
            size_t k = left < (long long)sizeof(buf) ? (size_t)left : sizeof(buf);
            ssize_t r = fd >= 0 && sz > 0 ? read(fd, buf, k) : 0;
            if (r <= 0) { memset(buf, 0, k); r = (ssize_t)k; }
            ok = conn_send_all(t->cn, buf, (size_t)r) == 0;
            left -= r;
        }
        if (fd >= 0) close(fd);
        long long pad = (512 - want % 512) % 512;
        if (ok && pad) { memset(buf, 0, (size_t)pad); ok = conn_send_all(t->cn, buf, (size_t)pad) == 0; }
    }
    if (ok) { memset(buf, 0, 1024); conn_send_all(t->cn, buf, 1024); }
    conn_close(t->cn);
    free(t->job.path);
    free(t);
    pthread_mutex_lock(&g_send_lock); g_senders--; pthread_mutex_unlock(&g_send_lock);
    return NULL;
}

/* body: one "YYYY-MM-DD/name" per line; returns entries kept (validated, not live) */
static int parse_rec_list(const char *body, char (**out)[48], int max)
{
    *out = calloc((size_t)max, 48);
    if (!*out) return 0;
    int n = 0;
    const char *p = body;
    while (*p && n < max) {
        const char *e = p + strcspn(p, "\r\n,");
        char line[64];
        size_t l = (size_t)(e - p);
        if (l > 0 && l < 40) {
            memcpy(line, p, l); line[l] = 0;
            char *slash = strchr(line, '/');
            char full[256];
            if (slash) {
                *slash = 0;
                if (record_path(line, slash + 1, full, sizeof(full)) == 0 && !record_is_live(line, slash + 1)) {
                    *slash = '/';
                    snprintf((*out)[n], 40, "%s", line);
                    n++;
                }
            }
        }
        p = *e ? e + 1 : e;
    }
    return n;
}

static void do_reboot(void)
{
    sync();
    sleep(1);
    reboot(RB_AUTOBOOT);
}

/* web.hosts: extra host names the admin page answers to (e.g. a reverse
 * proxy's public name), comma separated. IP literals, the camera's DHCP host
 * name (+ .local / .lan) and the HTTPS certificate names are always allowed. */
static char s_hosts[256];
static int hosts_ok(const char *v)
{
    for (const char *p = v; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == ',' || *p == ' ')) return 0;
    return 1;
}
CTRL_STR(p_hosts, s_hosts, hosts_ok, "host names separated by commas (letters, digits, dots, dashes)")
/* web.proxies: IPv4 addresses of reverse proxies (e.g. Traefik). Requests from
 * them are attributed to the X-Forwarded-For client (login backoff), are not
 * subject to the per-address connection cap or the Host check (the proxy
 * routes by host name), and are never redirected to HTTPS. */
static char s_proxies[128];
static int proxies_ok(const char *v)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", v);
    for (char *t = strtok(tmp, ", "); t; t = strtok(NULL, ", ")) {
        struct in_addr a;
        if (inet_pton(AF_INET, t, &a) != 1) return 0;
    }
    return 1;
}
CTRL_STR(p_proxies, s_proxies, proxies_ok, "IPv4 addresses separated by commas")
static int is_proxy(uint32_t ip_net);
/* web.overlay: the page draws its own date/time overlay on the live view
 * when the camera does not burn one in; off = never */
static int s_web_overlay = 1;
CTRL_BOOL(p_webosd, s_web_overlay)

void web_register_params(void)
{
    CTRL_REG("web.hosts", p_hosts, CTRL_PERSIST);
    CTRL_REG("web.proxies", p_proxies, CTRL_PERSIST);
    CTRL_REG("web.overlay", p_webosd, CTRL_PERSIST);
}
int web_is_proxy(uint32_t ip_net) { return is_proxy(ip_net); }
static int is_proxy(uint32_t ip_net)
{
    if (!ip_net || !s_proxies[0]) return 0;
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", s_proxies);
    for (char *save = NULL, *t = strtok_r(tmp, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        struct in_addr a;
        if (inet_pton(AF_INET, t, &a) == 1 && a.s_addr == ip_net) return 1;
    }
    return 0;
}

static int list_has(const char *list, const char *h)
{
    size_t hl = strlen(h);
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == hl && !strncasecmp(p, h, hl)) return 1;
        p = e;
    }
    return 0;
}

/* Host header check against DNS rebinding: a page on an attacker's domain
 * that re-points that domain to the camera would otherwise be same-origin. */
static int host_allowed(const char *host)
{
    if (!host[0]) return 1;                                 /* HTTP/1.0 clients */
    char h[128];
    snprintf(h, sizeof h, "%s", host);
    if (h[0] == '[') return 1;                              /* IPv6 literal */
    char *colon = strchr(h, ':');
    if (colon) *colon = 0;
    size_t n = strlen(h);
    if (n && h[n - 1] == '.') h[--n] = 0;
    struct in_addr a;
    if (inet_pton(AF_INET, h, &a) == 1) return 1;
    if (!strcasecmp(h, "localhost") || tls_name_ok(h) || list_has(s_hosts, h)) return 1;
    char hn[64], alt[80];
    net_hostname(hn, sizeof hn);
    if (!strcasecmp(h, hn)) return 1;
    snprintf(alt, sizeof alt, "%s.local", hn); if (!strcasecmp(h, alt)) return 1;
    snprintf(alt, sizeof alt, "%s.lan", hn);   if (!strcasecmp(h, alt)) return 1;
    return 0;
}

static int handle_conn(conn_t *cn, req_t *in, const struct sockaddr *peer)
{
    static req_t r;   /* handlers run one at a time (g_web_lock) */
    free(r.big);
    memcpy(&r, in, sizeof r);
    in->big = NULL;                            /* the upload now belongs to r */
    g_peer = peer;

    /* Path without query string, for routing. */
    char path[256];
    snprintf(path, sizeof(path), "%s", r.path);
    char *q = strchr(path, '?');
    if (q) *q = 0;

    int is_get = !strcmp(r.method, "GET"), is_post = !strcmp(r.method, "POST");
    if (!is_get && !is_post) {
        respond_text(cn, 405, "Method Not Allowed", "method not allowed\n");
        return 0;
    }

    if (!r.via_proxy && strncmp(path, "/hook/", 6) != 0 && strncmp(path, "/onvif/", 7) != 0 && !host_allowed(r.host)) {
        respond_text(cn, 421, "Misdirected Request",
                     "unknown host name; add it to web.hosts (Security tab) to use it\n");
        return 0;
    }

    /* Optional HTTP -> HTTPS redirect for browsers. The CA certificate stays
     * reachable over HTTP (a new device fetches it before trusting HTTPS) and
     * so do webhooks (automations would not follow a POST redirect). */
    if (!conn_is_tls(cn) && !r.via_proxy && tls_redirect() && strcmp(path, "/api/tls/ca.crt") != 0 &&
        strncmp(path, "/hook/", 6) != 0 && r.host[0]) {
        char host[128], loc[512];
        snprintf(host, sizeof(host), "%s", r.host);
        char *colon = strchr(host, ':');
        if (colon && !strchr(colon + 1, ':')) *colon = 0;          /* drop :80 (not IPv6) */
        snprintf(loc, sizeof(loc), "Location: https://%s%s\r\n", host, r.path);
        respond(cn, 301, "Moved Permanently", "text/plain; charset=utf-8", loc, "HTTPS\n", 6);
        return 0;
    }

    /* Live video WebSocket: authorised by a one-time token obtained through
     * an authenticated request (browsers don't reliably send Digest on WS). */
    if (is_get && !strcmp(path, "/api/live") && r.upgrade_ws) {
        const char *t = strstr(r.path, "t=");
        char tok[40] = "";
        if (t) snprintf(tok, sizeof(tok), "%.32s", t + 2);
        /* Same-origin only; https:// too, for an HTTPS reverse proxy in front
         * (the proxy must forward the original Host header). */
        char o_http[160], o_https[160];
        snprintf(o_http, sizeof(o_http), "http://%s", r.host);
        snprintf(o_https, sizeof(o_https), "https://%s", r.host);
        char o_fwd[160] = "";
        if (r.via_proxy && r.xfhost[0])
            snprintf(o_fwd, sizeof(o_fwd), "%s://%s", r.xfproto[0] ? r.xfproto : "https", r.xfhost);
        if (r.origin[0] && strcmp(r.origin, o_http) != 0 && strcmp(r.origin, o_https) != 0 &&
            (!o_fwd[0] || strcmp(r.origin, o_fwd) != 0)) {
            respond_text(cn, 403, "Forbidden", "bad origin\n");
            return 0;
        }
        if (!r.ws_key[0] || strcmp(r.ws_version, "13") != 0 || !live_token_take(tok)) {
            respond_text(cn, 403, "Forbidden", "invalid live token\n");
            return 0;
        }
        if (live_viewers() >= LIVE_MAX_VIEWERS) {
            respond_text(cn, 503, "Busy", "too many live viewers\n");
            return 0;
        }
        char acc[32], hdr[256];
        ws_accept_key(r.ws_key, acc);
        int n = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
        if (conn_send_all(cn, hdr, (size_t)n) != 0) return 0;
        struct timeval none = { 0, 0 };
        setsockopt(cn->fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
        return live_ws_start(cn) == 0 ? 1 : 0;
    }

    /* Development terminal WebSocket (dev.terminal): one-time token from an
     * authenticated request, and the page's own Origin is required (a
     * browser always sends it for WebSockets). */
    if (is_get && !strcmp(path, "/api/term") && r.upgrade_ws) {
        const char *t = strstr(r.path, "t=");
        char tok[40] = "";
        if (t) snprintf(tok, sizeof(tok), "%.32s", t + 2);
        char o_http[160], o_https[160], o_fwd[160] = "";
        snprintf(o_http, sizeof(o_http), "http://%s", r.host);
        snprintf(o_https, sizeof(o_https), "https://%s", r.host);
        if (r.via_proxy && r.xfhost[0])
            snprintf(o_fwd, sizeof(o_fwd), "%s://%s", r.xfproto[0] ? r.xfproto : "https", r.xfhost);
        if (!r.origin[0] || (strcmp(r.origin, o_http) != 0 && strcmp(r.origin, o_https) != 0 &&
                             (!o_fwd[0] || strcmp(r.origin, o_fwd) != 0))) {
            respond_text(cn, 403, "Forbidden", "bad origin\n");
            return 0;
        }
        if (!term_enabled()) { respond_text(cn, 403, "Forbidden", "the terminal is off\n"); return 0; }
        if (!r.ws_key[0] || strcmp(r.ws_version, "13") != 0 || !term_token_take(tok)) {
            respond_text(cn, 403, "Forbidden", "invalid terminal token\n");
            return 0;
        }
        char acc[32], hdr[256], who[48] = "?";
        ws_accept_key(r.ws_key, acc);
        int n = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
        if (conn_send_all(cn, hdr, (size_t)n) != 0) return 0;
        struct timeval none = { 0, 0 };
        setsockopt(cn->fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
        if (peer && peer->sa_family == AF_INET)
            inet_ntop(AF_INET, &((const struct sockaddr_in *)peer)->sin_addr, who, sizeof who);
        return term_ws_start(cn, who) == 0 ? 1 : 0;
    }

    /* ONVIF: SOAP with its own login (WS-Security or HTTP Digest, checked inside). */
    if (is_post && !strncmp(path, "/onvif/", 7)) {
        /* SOAP clients send an XML content type and no Origin; a cross-site form
         * can do neither without a CORS preflight, which this server refuses. */
        if (r.origin[0] || (strncasecmp(r.ctype, "application/soap+xml", 20) && strncasecmp(r.ctype, "text/xml", 8) &&
                            strncasecmp(r.ctype, "application/xml", 15))) {
            respond_text(cn, 415, "Unsupported Media Type", "SOAP only\n");
            return 0;
        }
        if (auth_throttled(peer)) { respond_text(cn, 429, "Too Many Requests", "too many failed logins, try later\n"); return 0; }
        const char *soap = r.big ? (const char *)r.big : r.body;
        int handed = onvif_handle(cn, r.path, soap, r.host, r.authz, peer, conn_is_tls(cn), &g_penalty);
        free(r.big); r.big = NULL; r.big_len = 0;
        return handed;
    }

    /* Webhook: token instead of Digest (for Home Assistant automations). */
    if ((is_post || is_get) && !strcmp(path, "/hook/sound")) {
        h_hook_sound(cn, &r);
        free(r.big);
        r.big = NULL;
        r.big_len = 0;
        return 0;
    }

#ifdef WEB_TEST_NOAUTH   /* host browser tests only: never defined in camera builds */
    int v = 1;
#else
    if (r.authz[0] && auth_throttled(peer)) {
        respond_text(cn, 429, "Too Many Requests", "too many failed logins, try again later\n");
        return 0;
    }
    int v = r.authz[0] ? auth_digest_verify_ex(r.authz, r.method, r.path) : 0;
    if (r.authz[0] && v != -1) auth_record(peer, v == 1);
#endif
    if (v != 1) {
        if (r.authz[0] && v == 0) g_penalty = 1;   /* slow down guessing (after the lock) */
        respond_401(cn, v == -1);
        return 0;
    }

    if ((is_post || (is_get && (!strcmp(path, "/api/wifi/scan") || !strcmp(path, "/api/hook/token") ||
                                 !strcmp(path, "/api/live/token") || !strcmp(path, "/api/term/token")))) && !r.has_xrw) {
        respond_text(cn, 403, "Forbidden", "missing X-Requested-With\n");
        return 0;
    }

    if (is_get && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
        respond(cn, 200, "OK", "text/html; charset=utf-8",
                "Content-Security-Policy: default-src 'self'; img-src 'self' blob:; media-src 'self' blob:; connect-src 'self'; "
                "style-src 'unsafe-inline'; script-src 'unsafe-inline'\r\n",
                web_index_html, strlen(web_index_html));
    } else if (is_get && !strcmp(path, "/snapshot.jpg")) {
        h_snapshot(cn, strstr(r.path, "zoom=1") != NULL);
    } else if (is_get && !strcmp(path, "/api/status")) {
        h_status(cn);
    } else if (is_get && !strcmp(path, "/api/osd")) {
        char json[320];
        int n = osd_json_full(json, sizeof(json));
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_post && !strcmp(path, "/api/osd")) {
        osd_cfg_t c;
        char b[8];
        const char *err;
        osd_get(&c);
        if (form_get(r.body, "enabled", b, sizeof(b)))   c.enabled = b[0] == '1';
        if (form_get(r.body, "show_time", b, sizeof(b))) c.show_time = b[0] == '1';
        if (form_get(r.body, "show_mode", b, sizeof(b))) c.show_mode = b[0] == '1';
        if (form_get(r.body, "show_fps", b, sizeof(b)))  c.show_fps = b[0] == '1';
        if (form_get(r.body, "pos", b, sizeof(b)))       snprintf(c.pos, sizeof(c.pos), "%.2s", b);
        if (form_get(r.body, "size", b, sizeof(b)))      c.size = (b[0] == '1' || b[0] == '2') && !b[1] ? b[0] - '0' : 0;
        char t[96];
        if (form_get(r.body, "prefix", t, sizeof(t))) { if (strlen(t) > OSD_TEXT_MAX) { respond_text(cn, 400, "Bad Request", "text too long (max 32)\n"); return 0; } snprintf(c.prefix, sizeof(c.prefix), "%s", t); }
        if (form_get(r.body, "suffix", t, sizeof(t))) { if (strlen(t) > OSD_TEXT_MAX) { respond_text(cn, 400, "Bad Request", "text too long (max 32)\n"); return 0; } snprintf(c.suffix, sizeof(c.suffix), "%s", t); }
        if (osd_set(&c, &err) != 0) { respond_text(cn, 400, "Bad Request", err); return 0; }
        osd_hw_clear_guard();   /* saving the overlay re-arms the video overlay after a guard skip */
        char json[320];
        int n = osd_json_full(json, sizeof(json));
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/hook/token")) {
        char t[33], json[80];
        if (hook_token(t, 0) != 0) { respond_text(cn, 500, "Error", "no token\n"); return 0; }
        int n = snprintf(json, sizeof(json), "{\"token\":\"%s\"}", t);
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_post && !strcmp(path, "/api/hook/token")) {
        char t[33], json[80];
        if (hook_token(t, 1) != 0) { respond_text(cn, 500, "Error", "cannot create token\n"); return 0; }
        int n = snprintf(json, sizeof(json), "{\"token\":\"%s\"}", t);
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/live/token")) {
        char tok[33], json[64];
        if (live_token_new(tok) != 0) { respond_text(cn, 500, "Error", "no token\n"); return 0; }
        int n = snprintf(json, sizeof(json), "{\"token\":\"%s\"}", tok);
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/term/token")) {
        char tok[33], json[64];
        if (!term_enabled()) { respond_text(cn, 403, "Forbidden", "the development terminal is off\n"); return 0; }
        if (term_token_new(tok) != 0) { respond_text(cn, 500, "Error", "no token\n"); return 0; }
        int n = snprintf(json, sizeof(json), "{\"token\":\"%s\"}", tok);
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/health")) {
        h_json(cn, sys_health_json);
    } else if (is_get && !strcmp(path, "/api/wifi")) {
        h_json(cn, net_status_json);
    } else if (is_get && !strcmp(path, "/api/config")) {
        /* registered settings; ?prefix=mqtt. (secrets masked) */
        static char json[6144];
        char prefix[32] = "";
        const char *qs = strchr(r.path, '?');
        if (qs) form_get(qs + 1, "prefix", prefix, sizeof(prefix));
        int n = control_params_json(prefix, json, sizeof(json));
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_post && !strcmp(path, "/api/config")) {
        /* name=value&name2=value2 ... ; stops at the first invalid value */
        char body_copy[MAX_BODY + 1];
        snprintf(body_copy, sizeof(body_copy), "%s", r.body);
        char *save = NULL;
        for (char *kv = strtok_r(body_copy, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
            char *eq = strchr(kv, '=');
            if (!eq) continue;
            *eq = 0;
            char name[48], value[300];
            if (strlen(kv) >= sizeof(name)) { respond_text(cn, 400, "Bad Request", "bad name"); return 0; }
            snprintf(name, sizeof(name), "%s", kv);
            char one[400];
            snprintf(one, sizeof(one), "v=%s", eq + 1);          /* reuse form_get's URL decoding */
            if (!form_get(one, "v", value, sizeof(value))) value[0] = 0;
            const char *err = "invalid";
            if (control_set_value(name, value, &err) != 0) {
                char msg[160];
                snprintf(msg, sizeof(msg), "%s: %s", name, err);
                respond_text(cn, 400, "Bad Request", msg);
                return 0;
            }
        }
        respond_text(cn, 200, "OK", "saved");
    } else if (is_get && !strcmp(path, "/api/mqtt/tls")) {
        h_json(cn, tls_mqtt_info_json);
    } else if (is_post && !strcmp(path, "/api/mqtt/upload")) {
        /* raw PEM body; ?what=ca|cert|key */
        char what[8] = "";
        const char *qs = strchr(r.path, '?');
        if (qs) form_get(qs + 1, "what", what, sizeof(what));
        const char *body = r.big ? (const char *)r.big : r.body;
        size_t n = r.big ? r.big_len : strlen(r.body);
        const char *err = "";
        int rc = n > 32768 ? (err = "too large", -1) : tls_mqtt_upload(what, body, n, &err);
        if (r.big) { memset(r.big, 0, r.big_len); free(r.big); r.big = NULL; r.big_len = 0; }
        if (rc != 0) respond_text(cn, 400, "Bad Request", err);
        else { mqtt_reconnect(); h_json(cn, tls_mqtt_info_json); }
    } else if (is_post && !strcmp(path, "/api/mqtt/fetch")) {
        /* host/port from the form (unsaved values in the page), else the saved ones */
        char host[128] = "", port[8] = "", err[120] = "";
        form_get(r.body, "host", host, sizeof(host));
        form_get(r.body, "port", port, sizeof(port));
        for (char *p = host; *p; p++)
            if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == ':')) { respond_text(cn, 400, "Bad Request", "bad host"); return 0; }
        if (!host[0]) control_get_value("mqtt.host", host, sizeof(host), 0);
        if (!host[0]) { respond_text(cn, 400, "Bad Request", "enter the broker address first"); return 0; }
        int fd = mqtt_open_tcp(host, atoi(port), err, sizeof(err));
        static char json[6144];
        int n = fd < 0 ? -1 : tls_mqtt_fetch(fd, host, json, sizeof(json), err, sizeof(err));
        if (n < 0) respond_text(cn, 502, "Bad Gateway", err[0] ? err : "cannot reach the broker");
        else respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_post && !strcmp(path, "/api/mqtt/trust")) {
        char what[8] = "", expect[80] = "";
        const char *err = "";
        form_get(r.body, "what", what, sizeof(what));
        form_get(r.body, "expect", expect, sizeof(expect));
        if (tls_mqtt_trust(what, expect, &err) != 0) respond_text(cn, 400, "Bad Request", err);
        else {
            control_set_value("mqtt.tls_verify", !strcmp(what, "ca") ? "on" : "pin", &err);   /* use what was trusted */
            mqtt_reconnect();
            h_json(cn, tls_mqtt_info_json);
        }
    } else if (is_post && !strcmp(path, "/api/mqtt/clear")) {
        char what[8] = "";
        form_get(r.body, "what", what, sizeof(what));
        tls_mqtt_clear(what);
        mqtt_reconnect();
        h_json(cn, tls_mqtt_info_json);
    } else if (is_post && !strcmp(path, "/api/mqtt/csr")) {
        static char pem[2048];
        int n = tls_mqtt_csr(sys_device_name_cn(), pem, sizeof(pem));
        if (n <= 0) respond_text(cn, 500, "Error", "cannot create the key / CSR");
        else respond(cn, 200, "OK", "application/pkcs10",
                     "Content-Disposition: attachment; filename=\"camera-mqtt.csr\"\r\n", pem, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/mqtt/camera.crt")) {
        static char pem[4096];
        int n = tls_mqtt_camera_cert_pem(sys_device_name_cn(), pem, sizeof(pem));
        if (n <= 0) respond_text(cn, 500, "Error", "cannot issue the certificate");
        else respond(cn, 200, "OK", "application/x-x509-user-cert",
                     "Content-Disposition: attachment; filename=\"camera-mqtt-client.crt\"\r\n", pem, (size_t)n);
    } else if (is_get && !strcmp(path, "/terminal")) {
        respond(cn, 200, "OK", "text/html; charset=utf-8",
                "Content-Security-Policy: default-src 'self'; connect-src 'self'; "
                "style-src 'unsafe-inline'; script-src 'unsafe-inline'\r\n",
                web_terminal_html, strlen(web_terminal_html));
    } else if (is_get && (!strcmp(path, "/recordings") || !strcmp(path, "/recordings.html"))) {
        respond(cn, 200, "OK", "text/html; charset=utf-8",
                "Content-Security-Policy: default-src 'self'; img-src 'self' blob:; media-src 'self' blob:; connect-src 'self'; "
                "style-src 'unsafe-inline'; script-src 'unsafe-inline'\r\n",
                web_recordings_html, strlen(web_recordings_html));
    } else if (is_get && !strcmp(path, "/api/rec/all")) {
        size_t n = 0;
        char *json = record_list_all_json(&n);
        if (!json) respond_text(cn, 500, "Error", "out of memory");
        else { respond(cn, 200, "OK", "application/json", NULL, json, n); free(json); }
    } else if (is_post && (!strcmp(path, "/api/rec/tar") || !strcmp(path, "/api/rec/delete-many"))) {
        const char *body = r.big ? (const char *)r.big : r.body;
        char (*list)[48] = NULL;
        int n = parse_rec_list(body, &list, 5000);
        free(r.big); r.big = NULL; r.big_len = 0;
        if (!strcmp(path, "/api/rec/delete-many")) {
            int done = 0;
            for (int i = 0; i < n; i++) {
                char *slash = strchr(list[i], '/');
                *slash = 0;
                if (record_delete(list[i], slash + 1) == 0) done++;
            }
            free(list);
            char msg[64];
            snprintf(msg, sizeof(msg), "{\"deleted\":%d}", done);
            respond(cn, 200, "OK", "application/json", NULL, msg, strlen(msg));
            return 0;
        }
        if (!n) { free(list); respond_text(cn, 400, "Bad Request", "no recordings selected"); return 0; }
        long long bytes = 1024;
        for (int i = 0; i < n; i++) {
            char full[256], day[16], *slash = strchr(list[i], '/');
            snprintf(day, sizeof(day), "%.*s", (int)(slash - list[i]), list[i]);
            record_path(day, slash + 1, full, sizeof(full));
            struct stat st;
            long long sz = stat(full, &st) == 0 ? (long long)st.st_size : 0;
            memcpy(list[i] + 40, &sz, sizeof(sz));          /* size kept next to the name */
            bytes += tar_size_of(sz);
        }
        free(g_tar.path);
        memset(&g_tar, 0, sizeof(g_tar));
        if (live_token_new(g_tar.tok) != 0) { free(list); respond_text(cn, 500, "Error", "no token"); return 0; }
        live_token_take(g_tar.tok);
        g_tar.exp = time(NULL) + 300; g_tar.n = n; g_tar.path = list; g_tar.bytes = bytes;
        char msg[160];
        snprintf(msg, sizeof(msg), "{\"token\":\"%s\",\"files\":%d,\"bytes\":%lld}", g_tar.tok, n, bytes);
        respond(cn, 200, "OK", "application/json", NULL, msg, strlen(msg));
    } else if (is_get && !strncmp(path, "/rec-tar/", 9)) {
        if (!g_tar.path || time(NULL) > g_tar.exp || strcmp(path + 9, g_tar.tok) != 0) {
            respond_text(cn, 404, "Not Found", "download expired, select the files again\n");
            return 0;
        }
        pthread_mutex_lock(&g_send_lock);
        int ok = g_senders < 3;
        if (ok) g_senders++;
        pthread_mutex_unlock(&g_send_lock);
        tar_send_t *t = ok ? calloc(1, sizeof *t) : NULL;
        if (!t) {
            if (ok) { pthread_mutex_lock(&g_send_lock); g_senders--; pthread_mutex_unlock(&g_send_lock); }
            respond_text(cn, 503, "Busy", "too many downloads, try again\n");
            return 0;
        }
        t->cn = cn; t->job = g_tar;
        memset(&g_tar, 0, sizeof(g_tar));                 /* one-time */
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 64 * 1024);
        int rc = pthread_create(&th, &at, tar_sender, t);
        pthread_attr_destroy(&at);
        if (rc != 0) {
            free(t->job.path); free(t);
            pthread_mutex_lock(&g_send_lock); g_senders--; pthread_mutex_unlock(&g_send_lock);
            respond_text(cn, 503, "Busy", "cannot start download\n");
            return 0;
        }
        return 1;
    } else if (is_get && !strncmp(path, "/rec/", 5)) {
        char day[16] = "", name[64] = "";
        const char *slash = strchr(path + 5, '/');
        if (!slash || (size_t)(slash - (path + 5)) >= sizeof(day)) { respond_text(cn, 404, "Not Found", "not found\n"); return 0; }
        memcpy(day, path + 5, (size_t)(slash - (path + 5)));
        snprintf(name, sizeof(name), "%s", slash + 1);
        return send_recording(cn, day, name, r.range, strstr(r.path, "dl=1") != NULL);
    } else if (is_get && !strcmp(path, "/api/rec")) {
        h_json(cn, record_status_json);
    } else if (is_get && !strcmp(path, "/api/rec/list")) {
        static char json[65536];
        char day[16] = "";
        const char *qs = strchr(r.path, '?');
        if (qs) form_get(qs + 1, "day", day, sizeof(day));
        int n = record_list_json(day, json, sizeof(json));
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_post && !strcmp(path, "/api/rec/delete")) {
        char day[16] = "", name[64] = "";
        form_get(r.body, "day", day, sizeof(day));
        form_get(r.body, "name", name, sizeof(name));
        if (record_delete(day, name) != 0) respond_text(cn, 400, "Bad Request", "cannot delete (in use or not found)");
        else respond_text(cn, 200, "OK", "deleted");
    } else if (is_get && !strcmp(path, "/api/sub")) {
        h_json(cn, substream_status_json);
    } else if (is_post && (!strcmp(path, "/api/npu/test") || !strcmp(path, "/api/npu/create"))) {
        if (!r.has_xrw) { respond_text(cn, 403, "Forbidden", "missing X-Requested-With\n"); return 0; }
        int rc = strstr(r.path, "create") ? npu_create_async() : npu_test();
        respond_text(cn, rc == 0 ? 200 : 500, rc == 0 ? "OK" : "Error", rc == 0 ? "npu init ok\n" : "npu init failed (see log)\n");
    } else if (is_get && !strcmp(path, "/api/audio/aec")) {
        char json[512];
        int n = aec_status_json(json, sizeof json);
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/audio/aecdump")) {
        /* debug: N s of (raw mic, aligned reference, output), s16le x3 */
        char ss[8] = "";
        const char *qs = strchr(r.path, '?');
        if (qs) form_get(qs + 1, "s", ss, sizeof ss);
        size_t len = 0;
        char raw[4] = "";
        if (qs) form_get(qs + 1, "raw", raw, sizeof raw);
        void *b = raw[0] == '1' ? aec_dump_raw(ss[0] ? atoi(ss) : 3, &len) : aec_dump(ss[0] ? atoi(ss) : 5, &len);
        if (!b) respond_text(cn, 409, "Conflict", "echo cancellation off, or a dump is running\n");
        else respond(cn, 200, "OK", "application/octet-stream", NULL, b, len);
        free(b);
    } else if (is_get && !strcmp(path, "/api/isp/ae")) {
        char json[2048];
        int n = isp_ae_debug_json(json, sizeof json);
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/isp/get")) {
        char m[16] = "", ls[16] = "";
        const char *qs = strchr(r.path, '?');
        if (qs) { form_get(qs + 1, "m", m, sizeof m); form_get(qs + 1, "len", ls, sizeof ls); }
        size_t len = ls[0] ? (size_t)strtoul(ls, NULL, 10) : 1024;
        char *hex = malloc(0x4001);
        int n = hex ? isp_debug_get_hex(m, len, hex, 0x4001) : -1;
        if (n < 0) respond_text(cn, 400, "Bad Request", "unknown attribute or ISP read failed\n");
        else respond(cn, 200, "OK", "text/plain", NULL, hex, (size_t)n);
        free(hex);
    } else if (is_get && !strcmp(path, "/api/md/raw")) {
        static char json[8192];
        int n = md_hw_raw_json(json, sizeof(json));
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/events")) {
        static char json[3072];
        char ev[900], md[1100];
        events_status_json(ev, sizeof(ev));
        md_status_json(md, sizeof(md));
        char tp[160];
        tamper_status_json(tp, sizeof tp);
        int n = snprintf(json, sizeof(json), "{\"events\":%s,\"md\":%s,\"tamper\":%s,\"sound_db\":%d}", ev, md, tp, sounddet_level_db());
        if (n < 0) n = 0;
        if ((size_t)n >= sizeof(json)) n = (int)sizeof(json) - 1;     /* never send past the buffer */
        respond(cn, 200, "OK", "application/json", NULL, json, (size_t)n);
    } else if (is_get && !strcmp(path, "/api/tls")) {
        h_json(cn, tls_info_json);
    } else if (is_get && (!strcmp(path, "/api/tls/ca.crt") || !strcmp(path, "/api/tls/server.crt") ||
                          !strcmp(path, "/api/tls/fullchain.pem"))) {
        /* certificates only (public); private keys never leave the camera */
        static char pem[8192];
        int n = -1;
        const char *fname = "lsc-camera-ca.crt", *ctype = "application/x-x509-ca-cert";
        if (!strcmp(path, "/api/tls/ca.crt")) n = tls_ca_pem(pem, sizeof(pem));
        else if (!strcmp(path, "/api/tls/server.crt")) {
            n = tls_server_pem(pem, sizeof(pem));
            fname = "lsc-camera-server.crt"; ctype = "application/x-x509-user-cert";
        } else {
            n = tls_server_pem(pem, sizeof(pem));
            int m = n > 0 ? tls_ca_pem(pem + n, sizeof(pem) - (size_t)n) : -1;
            n = m > 0 ? n + m : -1;
            fname = "lsc-camera-fullchain.pem"; ctype = "application/x-pem-file";
        }
        char disp[128];
        snprintf(disp, sizeof(disp), "Content-Disposition: attachment; filename=\"%s\"\r\n", fname);
        if (n <= 0) respond_text(cn, 404, "Not Found", "no certificate yet\n");
        else respond(cn, 200, "OK", ctype, disp, pem, (size_t)n);
    } else if (is_post && !strcmp(path, "/api/tls")) {
        char names[1100] = "", red[4] = "";
        const char *err;
        int have_names = form_get(r.body, "names", names, sizeof(names));
        int redirect = form_get(r.body, "redirect", red, sizeof(red)) ? red[0] == '1' : -1;
        if (tls_set_config(have_names ? names : NULL, redirect, &err) != 0) {
            respond_text(cn, 400, "Bad Request", err);
            return 0;
        }
        h_json(cn, tls_info_json);
    } else if (is_post && !strcmp(path, "/api/tls/regenerate-ca")) {
        const char *err;
        if (tls_regenerate_ca(&err) != 0) respond_text(cn, 500, "Error", err);
        else h_json(cn, tls_info_json);
    } else if (is_get && !strcmp(path, "/api/wifi/scan")) {
        h_json(cn, net_scan_json);
    } else if (is_get && !strcmp(path, "/api/tz")) {
        h_json(cn, sys_tz_list_json);
    } else if (is_post && !strcmp(path, "/api/wifi/connect")) {
        h_wifi_connect(cn, &r);
    } else if (is_post && !strcmp(path, "/api/sound/play")) {
        char name[16] = "";
        form_get(r.body, "name", name, sizeof(name));
        if (!speaker_ready()) respond_text(cn, 503, "Unavailable", "speaker output is not working on this camera yet\n");
        else if (speaker_play_builtin(name) == 0) respond_text(cn, 200, "OK", "playing\n");
        else respond_text(cn, 404, "Not Found", "unknown sound\n");
    } else if (is_post && !strcmp(path, "/api/wifi/qr")) {
        char sec[8] = "180";
        form_get(r.body, "seconds", sec, sizeof(sec));
        if (qrwifi_start(atoi(sec), "started from web page") == 0)
            respond_text(cn, 202, "Accepted", "Looking for a Wi-Fi QR code; hold your phone's Share Wi-Fi QR code in the middle of the picture, 40-60 cm from the lens.");
        else
            respond_text(cn, 500, "Error", "could not start QR scanning");
    } else if (is_post && !strcmp(path, "/api/ap/start")) {
        h_ap_start(cn, &r);
    } else if (is_post && !strcmp(path, "/api/ap/password")) {
        h_ap_password(cn, &r);
    } else if (is_get && !strcmp(path, "/api/params")) {
        h_params(cn);
    } else if (is_post && !strcmp(path, "/api/set")) {
        h_set(cn, &r);
    } else if (is_post && !strcmp(path, "/api/credentials")) {
        h_credentials(cn, &r);
    } else if (is_post && !strcmp(path, "/api/reboot")) {
        respond_text(cn, 200, "OK", "rebooting\n");
        conn_shutdown_wr(cn);
        printf("[web] reboot requested\n");
        do_reboot();
    } else if (is_post && !strcmp(path, "/api/backup")) {
        /* POST (not a link) so a cross-site page cannot make the browser download
         * secrets with the admin's credentials: X-Requested-With is required */
        char w[4] = "";
        form_get(r.body, "wifi", w, sizeof w);
        uint8_t *tar = NULL;
        size_t tl = 0;
        if (backup_build(&tar, &tl, !strcmp(w, "1"), sys_device_name(), GIT_HASH) != 0) {
            respond_text(cn, 500, "Error", "cannot build the backup\n");
        } else {
            char hdr[200], day[16], fn[40];
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(day, sizeof day, "%Y%m%d", &tm);
            snprintf(fn, sizeof fn, "%s", sys_device_name());          /* [A-Za-z0-9 _.-] */
            for (char *c = fn; *c; c++) if (*c == ' ') *c = '-';
            snprintf(hdr, sizeof hdr, "Content-Disposition: attachment; filename=\"%s-backup-%s.tar\"\r\n", fn, day);
            respond(cn, 200, "OK", "application/x-tar", hdr, tar, tl);
            free(tar);
        }
    } else if (is_post && !strcmp(path, "/api/restore")) {
        const uint8_t *d = r.big ? r.big : (const uint8_t *)r.body;
        size_t dl = r.big ? r.big_len : r.body_len;
        char err[120] = "";
        int n = 0;
        if (backup_restore(d, dl, &n, err, sizeof err) != 0) {
            respond_text(cn, 400, "Bad Request", err);
        } else {
            char msg[80];
            snprintf(msg, sizeof msg, "restored %d files, restarting\n", n);
            respond_text(cn, 200, "OK", msg);
            conn_shutdown_wr(cn);
            do_reboot();
        }
    } else if (is_post && !strcmp(path, "/api/reset-settings")) {
        control_reset_settings();
        respond_text(cn, 200, "OK", "settings reset, rebooting\n");
        conn_shutdown_wr(cn);
        printf("[web] settings reset + reboot requested\n");
        do_reboot();
    } else {
        respond_text(cn, 404, "Not Found", "not found\n");
    }
    return 0;
}

/* All request handling is serialised (handlers share static buffers); TLS
 * handshakes run outside the lock in per-connection threads. */
static pthread_mutex_t g_web_lock = PTHREAD_MUTEX_INITIALIZER;

static void serve(conn_t *cn)
{
    struct timeval tv = { 3, 0 };
    setsockopt(cn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct timeval tvs = { 10, 0 };
    setsockopt(cn->fd, SOL_SOCKET, SO_SNDTIMEO, &tvs, sizeof(tvs));
    struct sockaddr_storage peer;
    socklen_t pl = sizeof peer;
    memset(&peer, 0, sizeof peer);
    getpeername(cn->fd, (struct sockaddr *)&peer, &pl);

    req_t *rq = calloc(1, sizeof *rq);
    if (!rq) { conn_close(cn); return; }
    if (read_request(cn, rq) != 0) {           /* outside the lock: slow clients only cost their own slot */
        respond_text(cn, 400, "Bad Request", "bad request\n");
        free(rq->big); free(rq);
        conn_close(cn);
        return;
    }
    if (peer.ss_family == AF_INET && is_proxy(((struct sockaddr_in *)&peer)->sin_addr.s_addr)) {
        rq->via_proxy = 1;
        struct in_addr ca;
        if (rq->xff[0] && inet_pton(AF_INET, rq->xff, &ca) == 1)
            ((struct sockaddr_in *)&peer)->sin_addr = ca;   /* login backoff per real client */
    }
    pthread_mutex_lock(&g_web_lock);
    g_penalty = 0;
    int handed_off = handle_conn(cn, rq, (const struct sockaddr *)&peer);   /* 1 = handed to a live viewer thread */
    int penalty = g_penalty;
    g_peer = NULL;
    pthread_mutex_unlock(&g_web_lock);
    free(rq->big); free(rq);
    if (penalty) sleep(1);                     /* failed login: slows this client only */
    if (!handed_off) conn_close(cn);
}

/* Connection slots, shared by HTTP and HTTPS: at most CONN_MAX connections
 * at once. One address may have CONN_PER_IP being served (a browser opens up
 * to 6 per host) and CONN_WAIT_PER_IP more queued for up to CONN_WAIT_MS;
 * beyond that it is refused, so one host never holds more than 12 of 16.
 * (Before 2026-11-12 a 5th connection was reset at once, and pages that load
 * many settings in parallel saw random failures.) */
#define CONN_MAX         16
#define CONN_PER_IP      6
#define CONN_WAIT_PER_IP 6
#define CONN_WAIT_MS     10000
static struct { uint32_t ip; int n, w; } g_slots[CONN_MAX];
static int g_conn_active;                       /* threads: served + waiting */
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_conn_cond = PTHREAD_COND_INITIALIZER;

static uint32_t slot_key(uint32_t ip) { return is_proxy(ip) ? 0xffffffffu : ip; }   /* a proxy: global cap only */

static int slot_find(uint32_t ip)               /* g_conn_lock held; -1 if none */
{
    for (int i = 0; i < CONN_MAX; i++) if ((g_slots[i].n || g_slots[i].w) && g_slots[i].ip == ip) return i;
    return -1;
}

/* At accept time (g_conn_active < CONN_MAX was waited for): reserve a thread
 * for this address. 0 ok, -1 this address already has too many. */
static int conn_reserve(uint32_t ip)
{
    ip = slot_key(ip);
    pthread_mutex_lock(&g_conn_lock);
    int i = slot_find(ip);
    if (g_conn_active >= CONN_MAX ||           /* the HTTP and HTTPS accept loops can race past conn_wait_free */
        (i >= 0 && ip != 0xffffffffu && g_slots[i].n + g_slots[i].w >= CONN_PER_IP + CONN_WAIT_PER_IP)) {
        pthread_mutex_unlock(&g_conn_lock);
        return -1;
    }
    if (i < 0) {
        for (i = 0; i < CONN_MAX && (g_slots[i].n || g_slots[i].w); i++) {}
        if (i == CONN_MAX) { pthread_mutex_unlock(&g_conn_lock); return -1; }   /* cannot happen: entries <= threads */
        g_slots[i].ip = ip; g_slots[i].n = 0; g_slots[i].w = 0;
    }
    g_slots[i].w++;
    g_conn_active++;
    pthread_mutex_unlock(&g_conn_lock);
    return 0;
}

/* In the connection thread: wait (bounded) until the address is under
 * CONN_PER_IP, then count as served. 0 ok, -1 timed out (reservation kept:
 * conn_release() undoes it). */
static int conn_wait_turn(uint32_t ip)
{
    ip = slot_key(ip);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += CONN_WAIT_MS / 1000;
    pthread_mutex_lock(&g_conn_lock);
    int i = slot_find(ip), rc = 0;
    while (ip != 0xffffffffu && g_slots[i].n >= CONN_PER_IP) {
        if (pthread_cond_timedwait(&g_conn_cond, &g_conn_lock, &dl) != 0) { rc = -1; break; }
        i = slot_find(ip);                      /* entry index is stable while w > 0 */
    }
    if (rc == 0) { g_slots[i].w--; g_slots[i].n++; }
    pthread_mutex_unlock(&g_conn_lock);
    return rc;
}

static void conn_release(uint32_t ip, int served)
{
    ip = slot_key(ip);
    pthread_mutex_lock(&g_conn_lock);
    int i = slot_find(ip);
    if (i >= 0) { if (served) g_slots[i].n--; else g_slots[i].w--; }
    g_conn_active--;
    pthread_cond_broadcast(&g_conn_cond);       /* the accept loop and this address's waiters */
    pthread_mutex_unlock(&g_conn_lock);
}

static void conn_wait_free(void)
{
    pthread_mutex_lock(&g_conn_lock);
    while (g_conn_active >= CONN_MAX) pthread_cond_wait(&g_conn_cond, &g_conn_lock);
    pthread_mutex_unlock(&g_conn_lock);
}

typedef struct { int fd; int tls; uint32_t ip; } conn_job_t;

static void *conn_thread(void *arg)
{
    conn_job_t *j = arg;
    if (conn_wait_turn(j->ip) != 0) {           /* queued too long: refuse */
        close(j->fd);
        conn_release(j->ip, 0);
        free(j);
        return NULL;
    }
    conn_t *cn = j->tls ? tls_accept(j->fd, 10000) : conn_plain(j->fd);
    if (cn) serve(cn);
    else if (!j->tls) close(j->fd);
    conn_release(j->ip, 1);
    free(j);
    return NULL;
}

/* accept one connection on s and hand it to a slot thread */
static void accept_one(int s, int tls)
{
    conn_wait_free();
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, 60000) <= 0) return;
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int c = accept(s, (struct sockaddr *)&a, &al);
    if (c < 0) { if (errno != EINTR) usleep(100000); return; }
    uint32_t ip = al >= sizeof a ? a.sin_addr.s_addr : 0;
    if (conn_reserve(ip) != 0) { close(c); return; }
    conn_job_t *j = malloc(sizeof *j);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 96 * 1024);
    if (!j) { close(c); conn_release(ip, 0); }
    else {
        j->fd = c; j->tls = tls; j->ip = ip;
        if (pthread_create(&t, &at, conn_thread, j) != 0) { close(c); conn_release(ip, 0); free(j); }
    }
    pthread_attr_destroy(&at);
}

static int listen_on(int port)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 16) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static void *web_thread(void *arg)
{
    (void)arg;
    int s = listen_on(WEB_PORT);
    if (s < 0) { perror("[web] bind/listen"); return NULL; }
    printf("[web] admin page on port %d\n", WEB_PORT);
    for (;;) accept_one(s, 0);
    return NULL;
}

/* ---- HTTPS ------------------------------------------------------------------ */
static void *https_thread(void *arg)
{
    (void)arg;
    while (tls_init() != 0) sleep(20);        /* e.g. no IP address yet */
    int s = listen_on(WEB_HTTPS_PORT);
    if (s < 0) { perror("[web] https bind/listen"); return NULL; }
    printf("[web] admin page on port %d (HTTPS)\n", WEB_HTTPS_PORT);
    long last = (long)mono_s();
    for (;;) {
        if ((long)mono_s() - last >= 60) { last = (long)mono_s(); tls_maintain(); }
        accept_one(s, 1);
    }
    return NULL;
}

int web_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, web_thread, NULL) != 0) return -1;
    pthread_detach(t);
#ifndef WEB_NO_HTTPS
    if (pthread_create(&t, NULL, https_thread, NULL) == 0) pthread_detach(t);
#endif
    return 0;
}
