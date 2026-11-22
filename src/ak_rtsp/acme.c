/* ACME client (RFC 8555) for the HTTPS certificate. See acme.h.
 *
 * One issuance: directory -> account (cached) -> new order -> for each
 * authorization: publish the challenge (http-01 token on port 80, or a TXT
 * record through the DNS provider), tell the server, poll until valid,
 * remove the record -> finalize with a CSR for a new key -> download the
 * chain -> tls_custom_install(). Authorizations are done one at a time:
 * DuckDNS has a single TXT record per domain (example.duckdns.org and
 * *.example.duckdns.org need different values).
 *
 * Safety: failures retry after 1 h, doubling to 24 h; after a success the
 * automatic check waits at least 12 h (a bug in the "is a new certificate
 * needed" test can't burn the CA's rate limits). Secrets (API tokens) are
 * never logged. */
#include "acme.h"
#include "tls.h"
#include "conn.h"
#include "control.h"
#include "ctrl_params.h"
#include "monotime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/bignum.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/platform_util.h"

#define F_ACC_KEY  TLS_DIR "/ak_rtsp_acme_account.key"
#define F_ACC_URL  TLS_DIR "/ak_rtsp_acme_account"      /* directory URL \n account URL */
#define LE_PROD    "https://acme-v02.api.letsencrypt.org/directory"
#define LE_STAGING "https://acme-staging-v02.api.letsencrypt.org/directory"
#define RENEW_SECS (30L * 86400)
#define MIN_GAP_OK (12L * 3600)                         /* after a success */
#define MAX_DOM    4
#define RESP_MAX   (256 * 1024)
#define LOG_N      24
#define CUS_NAMES_LIST 4200                             /* tls.c: 16 names of up to 253 chars */
#define JT_API     1024                                 /* tokens for DNS API answers (zone objects are big) */

enum { CH_HTTP, CH_CLOUDFLARE, CH_DUCKDNS, CH_ACMEDNS };
static const char *const k_chal[] = { "http", "cloudflare", "duckdns", "acmedns" };

/* ---- settings ------------------------------------------------------------- */
static int  s_en, s_chal, s_wait;
static char s_server[200] = "letsencrypt";
static char s_email[100], s_domains[600];
static char s_api[200];             /* DNS API URL: acme-dns server; optional override for the others */
static char s_token[160];           /* Cloudflare API token / DuckDNS token / acme-dns password */
static char s_user[80], s_sub[80];  /* acme-dns account */
static char s_target[200];          /* acme-dns fulldomain: the CNAME target, shown in the page */

static volatile int g_kick, g_run, g_reg;

static int dns_name_ok(const char *s)
{
    size_t n = strlen(s), lab = 0;
    if (n < 1 || n > 253 || s[0] == '.' || s[n - 1] == '.' || s[n - 1] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') { if (lab == 0 || s[i - 1] == '-') return 0; lab = 0; continue; }
        if (!(isalnum((unsigned char)c) || c == '-')) return 0;
        if (c == '-' && lab == 0) return 0;
        if (++lab > 63) return 0;
    }
    return strchr(s, '.') != NULL;                     /* public names have a dot */
}

/* "a.example.org, *.example.org" -> lower case, validated, deduplicated. Count or -1. */
static int parse_domains(const char *in, char out[MAX_DOM][254])
{
    char buf[sizeof s_domains];
    if (strlen(in) >= sizeof buf) return -1;
    snprintf(buf, sizeof buf, "%s", in);
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, ", \t", &save); t; t = strtok_r(NULL, ", \t", &save)) {
        for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!((t[0] == '*' && t[1] == '.') ? dns_name_ok(t + 2) : dns_name_ok(t))) return -1;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= !strcmp(out[k], t);
        if (dup) continue;
        if (n == MAX_DOM) return -1;
        snprintf(out[n++], 254, "%s", t);
    }
    return n;
}
static int domains_ok(const char *v) { char d[MAX_DOM][254]; return parse_domains(v, d) >= 0; }

static int url_chars_ok(const char *v)
{
    for (; *v; v++) if (!(isalnum((unsigned char)*v) || strchr("-._~:/?&=%+[]@", *v))) return 0;
    return 1;
}
static int server_ok(const char *v)
{
    return !strcmp(v, "letsencrypt") || !strcmp(v, "staging") ||
           (!strncmp(v, "https://", 8) && v[8] && url_chars_ok(v));
}
static int api_ok(const char *v)          /* empty, or http(s) URL (http: a LAN acme-dns server) */
{
    return !*v || (((!strncmp(v, "https://", 8) && v[8]) || (!strncmp(v, "http://", 7) && v[7])) && url_chars_ok(v));
}
static int email_ok(const char *v)
{
    if (!*v) return 1;
    const char *at = strchr(v, '@');
    if (!at || at == v || strchr(at + 1, '@') || !strchr(at, '.')) return 0;
    for (; *v; v++) if (!(isalnum((unsigned char)*v) || strchr("._%+-@", *v))) return 0;
    return 1;
}
static int token_ok(const char *v)        /* goes into headers and URLs: a safe alphabet only */
{
    for (; *v; v++) if (!(isalnum((unsigned char)*v) || strchr("-._~", *v))) return 0;
    return 1;
}

CTRL_BOOL(p_en, s_en)
CTRL_STR(p_server, s_server, server_ok, "letsencrypt, staging or an https:// directory URL")
CTRL_STR(p_email, s_email, email_ok, "must be an e-mail address")
CTRL_STR(p_domains, s_domains, domains_ok, "up to 4 DNS names (e.g. cam.example.org, *.example.org)")
CTRL_STR(p_api, s_api, api_ok, "must be an http:// or https:// URL")
CTRL_STR(p_token, s_token, token_ok, "letters, digits, '-', '_', '.' and '~' only")
CTRL_STR(p_user, s_user, token_ok, "letters, digits, '-', '_', '.' and '~' only")
CTRL_STR(p_sub, s_sub, token_ok, "letters, digits, '-', '_', '.' and '~' only")
CTRL_STR(p_target, s_target, domains_ok, "must be a DNS name")
CTRL_INT(p_wait, s_wait, 0, 600)
static int p_chal_get(char *o, size_t c) { snprintf(o, c, "%s", k_chal[s_chal]); return 0; }
static int p_chal_set(const char *v, const char **e)
{
    for (int i = 0; i < 4; i++) if (!strcmp(v, k_chal[i])) { s_chal = i; return 0; }
    if (!strcmp(v, "acme-dns")) { s_chal = CH_ACMEDNS; return 0; }
    *e = "must be http, cloudflare, duckdns or acmedns";
    return -1;
}
#define WRAP(fn) static int fn##_setk(const char *v, const char **e) { int r = fn##_set(v, e); if (!r) g_kick = 1; return r; }
WRAP(p_en) WRAP(p_server) WRAP(p_email) WRAP(p_domains) WRAP(p_api) WRAP(p_token) WRAP(p_user) WRAP(p_sub)
WRAP(p_target) WRAP(p_wait) WRAP(p_chal)

/* Settings as one consistent copy for an issuance (the page may change them). */
typedef struct {
    int chal, wait;
    char server[200], email[100], domains[600], api[200], token[160], user[80], sub[80];
} cfg_t;
static void cfg_snapshot(cfg_t *c)
{
    char v[16];
    memset(c, 0, sizeof *c);
    control_get_value("acme.server", c->server, sizeof c->server, 1);
    control_get_value("acme.email", c->email, sizeof c->email, 1);
    control_get_value("acme.domains", c->domains, sizeof c->domains, 1);
    control_get_value("acme.dns_api", c->api, sizeof c->api, 1);
    control_get_value("acme.dns_token", c->token, sizeof c->token, 1);
    control_get_value("acme.dns_user", c->user, sizeof c->user, 1);
    control_get_value("acme.dns_sub", c->sub, sizeof c->sub, 1);
    control_get_value("acme.challenge", v, sizeof v, 1);
    c->chal = CH_HTTP;
    for (int i = 0; i < 4; i++) if (!strcmp(v, k_chal[i])) c->chal = i;
    control_get_value("acme.dns_wait", v, sizeof v, 1);
    c->wait = atoi(v);
    size_t l = strlen(c->api);
    while (l && c->api[l - 1] == '/') c->api[--l] = 0;
}

/* ---- status and log ---------------------------------------------------------- */
static pthread_mutex_t g_st = PTHREAD_MUTEX_INITIALIZER;
static char g_log[LOG_N][200];
static unsigned g_logn;
static char g_state[12] = "off", g_msg[240];
static time_t g_last_ok, g_next;
static int g_fail;

static void clean(char *s)                /* for JSON and logs */
{
    for (; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f) *s = '\'';
}

static void alog(const char *fmt, ...)
{
    char m[180];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    clean(m);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    pthread_mutex_lock(&g_st);
    snprintf(g_log[g_logn++ % LOG_N], sizeof g_log[0], "%02d:%02d:%02d %s", tm.tm_hour, tm.tm_min, tm.tm_sec, m);
    pthread_mutex_unlock(&g_st);
    printf("[acme] %s\n", m);
    fflush(stdout);
}

static void set_state(const char *st, const char *msg)
{
    pthread_mutex_lock(&g_st);
    snprintf(g_state, sizeof g_state, "%s", st);
    snprintf(g_msg, sizeof g_msg, "%s", msg);
    clean(g_msg);
    pthread_mutex_unlock(&g_st);
}

int acme_status_json(char *out, size_t cap)
{
    char ca[220];
    tls_acme_ca_desc(ca, sizeof ca);
    pthread_mutex_lock(&g_st);
    time_t now = time(NULL);
    long next = g_next > now ? (long)(g_next - now) : 0;
    char last[24] = "";
    if (g_last_ok) {
        struct tm tm;
        localtime_r(&g_last_ok, &tm);
        strftime(last, sizeof last, "%Y-%m-%d %H:%M", &tm);
    }
    size_t o = (size_t)snprintf(out, cap,
        "{\"enabled\":%s,\"state\":\"%s\",\"message\":\"%s\",\"last_ok\":\"%s\",\"next_in\":%ld,\"ca\":\"%s\",\"log\":[",
        s_en ? "true" : "false", g_state, g_msg, last, g_state[0] == 'e' ? next : 0, ca);
    unsigned first = g_logn > LOG_N ? g_logn - LOG_N : 0;
    for (unsigned i = first; i < g_logn && o < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%s\"%s\"", i == first ? "" : ",", g_log[i % LOG_N]);
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]}");
    pthread_mutex_unlock(&g_st);
    return (int)(o < cap ? o : cap - 1);
}

void acme_run_now(void) { g_run = 1; }
void acme_dns_register_start(void) { g_reg = 1; }

/* ---- http-01 tokens ------------------------------------------------------------ */
static pthread_mutex_t g_hl = PTHREAD_MUTEX_INITIALIZER;
static char g_htok[MAX_DOM][128], g_hka[MAX_DOM][256];

int acme_http01(const char *token, char *out, size_t cap)
{
    int n = -1;
    pthread_mutex_lock(&g_hl);
    for (int k = 0; k < MAX_DOM; k++)
        if (g_htok[k][0] && !strcmp(g_htok[k], token)) { n = snprintf(out, cap, "%s", g_hka[k]); break; }
    pthread_mutex_unlock(&g_hl);
    return n;
}

static void http01_set(int slot, const char *tok, const char *ka)
{
    pthread_mutex_lock(&g_hl);
    snprintf(g_htok[slot], sizeof g_htok[slot], "%s", tok ? tok : "");
    snprintf(g_hka[slot], sizeof g_hka[slot], "%s", ka ? ka : "");
    pthread_mutex_unlock(&g_hl);
}

/* ---- small encoders --------------------------------------------------------------- */
static size_t b64url(const void *in, size_t n, char *out, size_t cap)
{
    size_t ol = 0;
    if (mbedtls_base64_encode((unsigned char *)out, cap, &ol, in, n) != 0) { if (cap) out[0] = 0; return 0; }
    size_t w = 0;
    for (size_t i = 0; i < ol; i++) {
        char c = out[i];
        if (c == '=') break;
        out[w++] = c == '+' ? '-' : c == '/' ? '_' : c;
    }
    out[w] = 0;
    return w;
}

static int safe_str(const char *s)        /* server values copied into our JSON / requests */
{
    for (; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s <= 0x20 || (unsigned char)*s >= 0x7f) return 0;
    return 1;
}

static int save_file(const char *path, const char *data, size_t len, int mode)
{
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    int ok = write(fd, data, len) == (ssize_t)len && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* ---- JSON (a small tokenizer, enough for ACME and the DNS APIs) ------------------ */
enum { J_OBJ = 1, J_ARR, J_STR, J_PRIM };
typedef struct { unsigned char type; int start, end; } jt;   /* strings: start/end exclude quotes */

static int j_parse(const char *s, size_t len, jt *t, int max)
{
    int n = 0, st[32], sp = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '{' || c == '[') {
            if (n >= max || sp >= 32) return -1;
            t[n] = (jt){ c == '{' ? J_OBJ : J_ARR, (int)i, -1 };
            st[sp++] = n++;
        } else if (c == '}' || c == ']') {
            if (!sp || t[st[sp - 1]].type != (c == '}' ? J_OBJ : J_ARR)) return -1;
            t[st[--sp]].end = (int)i + 1;
        } else if (c == '"') {
            size_t j = i + 1;
            while (j < len && s[j] != '"') j += s[j] == '\\' ? 2 : 1;
            if (j >= len || n >= max) return -1;
            t[n++] = (jt){ J_STR, (int)i + 1, (int)j };
            i = j;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':' || c == ',') {
            continue;
        } else {
            size_t j = i;
            while (j < len && !strchr(" \t\r\n,:]}", s[j])) j++;
            if (n >= max) return -1;
            t[n++] = (jt){ J_PRIM, (int)i, (int)j };
            i = j - 1;
        }
        if (!sp && n && t[0].end >= 0 && t[0].type <= J_ARR) break;   /* end of the top value */
    }
    return sp ? -1 : n;
}

static int j_next(const jt *t, int n, int i)              /* token after i's subtree */
{
    int j = i + 1;
    while (j < n && t[j].start < t[i].end) j++;
    return j;
}

static int j_get(const char *s, const jt *t, int n, int obj, const char *key)
{
    if (obj < 0 || obj >= n || t[obj].type != J_OBJ) return -1;
    size_t kl = strlen(key);
    for (int i = obj + 1; i + 1 < n && t[i].start < t[obj].end; ) {
        int v = i + 1;
        if (t[i].type == J_STR && (size_t)(t[i].end - t[i].start) == kl && !strncmp(s + t[i].start, key, kl)) return v;
        i = j_next(t, n, v);
    }
    return -1;
}

static int j_str(const char *s, const jt *t, int i, char *out, size_t cap)   /* string or primitive */
{
    if (cap) out[0] = 0;
    if (i < 0 || (t[i].type != J_STR && t[i].type != J_PRIM)) return -1;
    size_t o = 0;
    for (int k = t[i].start; k < t[i].end && o + 1 < cap; k++) {
        char c = s[k];
        if (c == '\\' && k + 1 < t[i].end) {
            c = s[++k];
            if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; else if (c == 'r') c = '\r';
            else if (c == 'u') { c = '?'; k += 4; if (k >= t[i].end) break; }
        }
        out[o++] = c;
    }
    out[o] = 0;
    return 0;
}

static int j_strk(const char *s, const jt *t, int n, int obj, const char *key, char *out, size_t cap)
{
    return j_str(s, t, j_get(s, t, n, obj, key), out, cap);
}

/* ---- HTTP(S) client ------------------------------------------------------------- */
typedef struct {
    int status, retry_after;
    char *buf, *body;
    size_t len;
    char location[512], nonce[200], ctype[96];
} resp_t;

static void resp_free(resp_t *r) { free(r->buf); memset(r, 0, sizeof *r); }

typedef struct { int tls, port; char host[128], path[512]; } url_t;

static int url_parse(const char *u, url_t *o)
{
    const char *p;
    memset(o, 0, sizeof *o);
    if (!strncmp(u, "https://", 8)) { o->tls = 1; o->port = 443; p = u + 8; }
    else if (!strncmp(u, "http://", 7)) { o->port = 80; p = u + 7; }
    else return -1;
    size_t hl = strcspn(p, ":/?");
    if (!hl || hl >= sizeof o->host) return -1;
    memcpy(o->host, p, hl);
    for (size_t i = 0; i < hl; i++) if (!(isalnum((unsigned char)p[i]) || p[i] == '.' || p[i] == '-')) return -1;
    p += hl;
    if (*p == ':') {
        char *e;
        long port = strtol(p + 1, &e, 10);
        if (port < 1 || port > 65535) return -1;
        o->port = (int)port;
        p = e;
    }
    if (*p && *p != '/' && *p != '?') return -1;
    if (strlen(p) + 2 > sizeof o->path) return -1;
    snprintf(o->path, sizeof o->path, "%s%s", *p == '/' ? "" : "/", p);
    for (const char *q = o->path; *q; q++) if ((unsigned char)*q <= 0x20 || (unsigned char)*q >= 0x7f) return -1;
    return 0;
}

static int tcp_open(const char *host, int port, char *err, size_t cap)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(err, cap, "cannot resolve %s", host); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { freeaddrinfo(res); snprintf(err, cap, "socket: %s", strerror(errno)); return -1; }
    struct timeval tv = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(err, cap, "cannot connect to %s:%d (%s)", host, port, strerror(errno));
        close(fd); freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int resp_complete(const char *b, size_t len)
{
    const char *he = strstr(b, "\r\n\r\n");
    if (!he) return 0;
    size_t hlen = (size_t)(he - b) + 4, blen = len - hlen;
    for (const char *l = strstr(b, "\r\n"); l && l < he; l = strstr(l + 2, "\r\n")) {
        if (!strncasecmp(l + 2, "Content-Length:", 15)) return blen >= (size_t)strtoul(l + 17, NULL, 10);
        if (!strncasecmp(l + 2, "Transfer-Encoding:", 18) && strstr(l, "chunked") && strstr(l, "chunked") < strstr(l + 2, "\r\n"))
            return blen >= 5 && !memcmp(b + len - 5, "0\r\n\r\n", 5);
    }
    return 0;
}

/* One request on a new connection. Never logs the URL (it can carry a token). */
static int http_do(const char *method, const char *url, const char *hdrs, const char *ctype,
                   const char *body, size_t blen, resp_t *r, char *err, size_t cap)
{
    memset(r, 0, sizeof *r);
    url_t u;
    if (url_parse(url, &u) != 0) { snprintf(err, cap, "invalid URL"); return -1; }
    int fd = tcp_open(u.host, u.port, err, cap);
    if (fd < 0) return -1;
    conn_t *c;
    if (u.tls) c = tls_client_connect(fd, u.host, 3, 0, NULL, 20000, err, cap);
    else if (!(c = conn_plain(fd))) { close(fd); snprintf(err, cap, "out of memory"); }
    if (!c) return -1;
    if (!u.tls) {
        struct timeval tv = { 20, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    char hostport[140];
    if (u.port == (u.tls ? 443 : 80)) snprintf(hostport, sizeof hostport, "%s", u.host);
    else snprintf(hostport, sizeof hostport, "%s:%d", u.host, u.port);
    size_t qcap = 700 + strlen(u.path) + (hdrs ? strlen(hdrs) : 0);
    char *q = malloc(qcap);
    if (!q) { conn_close(c); snprintf(err, cap, "out of memory"); return -1; }
    int qn = snprintf(q, qcap, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ak_rtsp-acme/1\r\nAccept: */*\r\n"
                      "Connection: close\r\n%s", method, u.path, hostport, hdrs ? hdrs : "");
    if (ctype) qn += snprintf(q + qn, qcap - (size_t)qn, "Content-Type: %s\r\n", ctype);
    if (body || strcmp(method, "GET") != 0) qn += snprintf(q + qn, qcap - (size_t)qn, "Content-Length: %zu\r\n", blen);
    qn += snprintf(q + qn, qcap - (size_t)qn, "\r\n");
    int rc = conn_send_all(c, q, (size_t)qn);
    free(q);
    if (!rc && body && blen) rc = conn_send_all(c, body, blen);
    if (rc) { conn_close(c); snprintf(err, cap, "%s: send failed", u.host); return -1; }

    size_t bcap = 16384, len = 0;
    char *b = malloc(bcap + 1);
    if (!b) { conn_close(c); snprintf(err, cap, "out of memory"); return -1; }
    int64_t deadline = mono_ms() + 60000;              /* a server trickling bytes can't hold us longer */
    for (;;) {
        if (mono_ms() > deadline) { free(b); conn_close(c); snprintf(err, cap, "%s: response too slow", u.host); return -1; }
        if (len == bcap) {
            if (bcap >= RESP_MAX) { free(b); conn_close(c); snprintf(err, cap, "%s: response too large", u.host); return -1; }
            char *nb = realloc(b, bcap * 2 + 1);
            if (!nb) { free(b); conn_close(c); snprintf(err, cap, "out of memory"); return -1; }
            b = nb; bcap *= 2;
        }
        ssize_t n = conn_recv(c, b + len, bcap - len);
        if (n <= 0) break;
        len += (size_t)n;
        b[len] = 0;
        if (resp_complete(b, len)) break;
    }
    conn_close(c);
    b[len] = 0;
    char *he = strstr(b, "\r\n\r\n");
    if (!he || sscanf(b, "HTTP/%*s %d", &r->status) != 1) {
        free(b);
        snprintf(err, cap, "%s: %s", u.host, len ? "invalid HTTP response" : "no response");
        return -1;
    }
    *he = 0;
    char *bd = he + 4;
    size_t bl = len - (size_t)(bd - b);
    long clen = -1;
    int chunked = 0;
    char *save = NULL;
    strtok_r(b, "\r\n", &save);                        /* status line */
    for (char *l = strtok_r(NULL, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
        char *v = strchr(l, ':');
        if (!v) continue;
        *v++ = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(l, "Location")) snprintf(r->location, sizeof r->location, "%s", v);
        else if (!strcasecmp(l, "Replay-Nonce")) snprintf(r->nonce, sizeof r->nonce, "%s", v);
        else if (!strcasecmp(l, "Content-Type")) snprintf(r->ctype, sizeof r->ctype, "%s", v);
        else if (!strcasecmp(l, "Content-Length")) clen = strtol(v, NULL, 10);
        else if (!strcasecmp(l, "Retry-After")) r->retry_after = atoi(v);
        else if (!strcasecmp(l, "Transfer-Encoding") && strstr(v, "chunked")) chunked = 1;
    }
    if (chunked) {                                     /* de-chunk in place */
        char *in = bd, *out = bd, *end = bd + bl;
        for (;;) {
            char *e;
            unsigned long sz = strtoul(in, &e, 16);
            char *le = strstr(in, "\r\n");
            if (!le || le >= end) break;
            in = le + 2;
            if (!sz) break;
            if (sz > (size_t)(end - in)) sz = (size_t)(end - in);
            memmove(out, in, sz);
            out += sz;
            in += sz + 2;
            if (in >= end) break;
        }
        bl = (size_t)(out - bd);
    } else if (clen >= 0 && (size_t)clen < bl) bl = (size_t)clen;
    bd[bl] = 0;
    r->buf = b;
    r->body = bd;
    r->len = bl;
    return 0;
}

/* ---- account key, JWS ------------------------------------------------------------ */
static mbedtls_pk_context g_key;
static int g_key_ok;
static char g_jwk[200], g_thumb[48];
static char g_kid[400], g_nonce[200];
static char g_dir[256], g_u_nonce[400], g_u_acct[400], g_u_order[400];

static int account_key(char *err, size_t cap)
{
    if (g_key_ok) return 0;
    char pem[1024] = "";
    FILE *f = fopen(F_ACC_KEY, "r");
    if (f) { size_t n = fread(pem, 1, sizeof pem - 1, f); pem[n] = 0; fclose(f); }
    if (!pem[0]) {
        if (tls_key_new_pem(pem, sizeof pem) < 0 || save_file(F_ACC_KEY, pem, strlen(pem), 0600) != 0) {
            snprintf(err, cap, "cannot create the account key");
            return -1;
        }
        alog("created a new ACME account key");
    }
    mbedtls_pk_init(&g_key);
    int ret = mbedtls_pk_parse_key(&g_key, (unsigned char *)pem, strlen(pem) + 1, NULL, 0, tls_rng, NULL);
    mbedtls_platform_zeroize(pem, sizeof pem);
    unsigned char q[65];
    size_t ql = 0;
    if (!ret && (mbedtls_pk_get_type(&g_key) != MBEDTLS_PK_ECKEY ||
                 mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(g_key)) != MBEDTLS_ECP_DP_SECP256R1)) ret = -1;
    if (!ret) ret = mbedtls_ecp_write_public_key(mbedtls_pk_ec(g_key), MBEDTLS_ECP_PF_UNCOMPRESSED, &ql, q, sizeof q);
    if (ret || ql != 65) { mbedtls_pk_free(&g_key); snprintf(err, cap, "the account key is unusable"); return -1; }
    char x[48], y[48];
    b64url(q + 1, 32, x, sizeof x);
    b64url(q + 33, 32, y, sizeof y);
    /* members in lexicographic order: this exact text is the RFC 7638 thumbprint input */
    snprintf(g_jwk, sizeof g_jwk, "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", x, y);
    unsigned char h[32];
    mbedtls_sha256((unsigned char *)g_jwk, strlen(g_jwk), h, 0);
    b64url(h, 32, g_thumb, sizeof g_thumb);
    g_key_ok = 1;
    return 0;
}

static int es256(const char *in, size_t n, char *out, size_t cap)
{
    unsigned char h[32], sig[64];
    mbedtls_sha256((const unsigned char *)in, n, h, 0);
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d, r, s;
    mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    int ret = mbedtls_ecp_export(mbedtls_pk_ec(g_key), &grp, &d, &Q);
    if (!ret) ret = mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, h, 32, MBEDTLS_MD_SHA256, tls_rng, NULL);
    if (!ret) ret = mbedtls_mpi_write_binary(&r, sig, 32);
    if (!ret) ret = mbedtls_mpi_write_binary(&s, sig + 32, 32);
    mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d); mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
    if (ret) return -1;
    b64url(sig, 64, out, cap);
    return 0;
}

static int get_nonce(char *err, size_t cap)
{
    resp_t r;
    if (http_do("GET", g_u_nonce, NULL, NULL, NULL, 0, &r, err, cap) != 0) return -1;
    int ok = r.nonce[0] && safe_str(r.nonce);
    if (ok) snprintf(g_nonce, sizeof g_nonce, "%s", r.nonce);
    resp_free(&r);
    if (!ok) { snprintf(err, cap, "the ACME server gave no nonce"); return -1; }
    return 0;
}

/* POST a JWS (payload NULL = POST-as-GET). Retries a rejected nonce. */
static int jws_post(const char *url, const char *payload, resp_t *r, char *err, size_t cap)
{
    if (!safe_str(url) || strlen(url) >= 400) { snprintf(err, cap, "invalid URL from the ACME server"); return -1; }
    size_t pl = payload ? strlen(payload) : 0;
    size_t need = pl * 2 + 2400;
    char *prot = malloc(1200), *si = malloc(need), *body = malloc(need + 200);
    int ret = -1;
    if (!prot || !si || !body) { snprintf(err, cap, "out of memory"); goto out; }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!g_nonce[0] && get_nonce(err, cap)) goto out;
        int hl = g_kid[0]
            ? snprintf(prot, 1200, "{\"alg\":\"ES256\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}", g_kid, g_nonce, url)
            : snprintf(prot, 1200, "{\"alg\":\"ES256\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}", g_jwk, g_nonce, url);
        g_nonce[0] = 0;
        size_t o = b64url(prot, (size_t)hl, si, need);
        size_t dot = o;
        si[o++] = '.';
        o += b64url(payload ? payload : "", pl, si + o, need - o);
        char sig[100];
        if (es256(si, o, sig, sizeof sig) != 0) { snprintf(err, cap, "signing failed"); goto out; }
        si[dot] = 0;
        int bn = snprintf(body, need + 200, "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}", si, si + dot + 1, sig);
        if (http_do("POST", url, NULL, "application/jose+json", body, (size_t)bn, r, err, cap) != 0) goto out;
        if (r->nonce[0] && safe_str(r->nonce)) snprintf(g_nonce, sizeof g_nonce, "%s", r->nonce);
        if (r->status == 400 && strstr(r->body, "badNonce")) { resp_free(r); continue; }
        ret = 0;
        goto out;
    }
    snprintf(err, cap, "the ACME server kept rejecting the nonce");
out:
    free(prot); free(si); free(body);
    return ret;
}

/* "what: <problem type>: <detail> (HTTP n)" from an RFC 7807 problem document */
static void problem(const resp_t *r, const char *what, char *err, size_t cap)
{
    jt t[48];
    char type[120] = "", detail[200] = "";
    int n = r->body ? j_parse(r->body, r->len, t, 48) : -1;
    if (n > 0) { j_strk(r->body, t, n, 0, "type", type, sizeof type); j_strk(r->body, t, n, 0, "detail", detail, sizeof detail); }
    const char *ty = strncmp(type, "urn:ietf:params:acme:error:", 27) ? type : type + 27;
    snprintf(err, cap, "%s: %s%s%s (HTTP %d)", what, ty[0] ? ty : "error", detail[0] ? ": " : "", detail, r->status);
}

static int directory(const char *url, char *err, size_t cap)
{
    resp_t r;
    if (http_do("GET", url, NULL, NULL, NULL, 0, &r, err, cap) != 0) return -1;
    jt t[256];
    int st = r.status, n = st == 200 ? j_parse(r.body, r.len, t, 256) : -1;
    if (n > 0) {
        j_strk(r.body, t, n, 0, "newNonce", g_u_nonce, sizeof g_u_nonce);
        j_strk(r.body, t, n, 0, "newAccount", g_u_acct, sizeof g_u_acct);
        j_strk(r.body, t, n, 0, "newOrder", g_u_order, sizeof g_u_order);
    }
    resp_free(&r);
    if (n <= 0 || !g_u_nonce[0] || !g_u_acct[0] || !g_u_order[0]) {
        snprintf(err, cap, "not an ACME directory (HTTP %d)", st);
        return -1;
    }
    if (strcmp(g_dir, url) != 0) { g_kid[0] = 0; g_nonce[0] = 0; }
    snprintf(g_dir, sizeof g_dir, "%s", url);
    return 0;
}

static int account(const cfg_t *c, int fresh, char *err, size_t cap)
{
    if (!fresh) {
        char buf[700] = "";
        FILE *f = fopen(F_ACC_URL, "r");
        if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = 0;
            char *kid = nl + 1;
            kid[strcspn(kid, "\r\n")] = 0;
            if (!strcmp(buf, g_dir) && kid[0] && safe_str(kid) && strlen(kid) < sizeof g_kid) {
                snprintf(g_kid, sizeof g_kid, "%s", kid);
                return 0;
            }
        }
    }
    g_kid[0] = 0;
    char pl[300];
    if (c->email[0]) snprintf(pl, sizeof pl, "{\"termsOfServiceAgreed\":true,\"contact\":[\"mailto:%s\"]}", c->email);
    else snprintf(pl, sizeof pl, "{\"termsOfServiceAgreed\":true}");
    resp_t r;
    if (jws_post(g_u_acct, pl, &r, err, cap) != 0) return -1;
    if ((r.status != 200 && r.status != 201) || !r.location[0] || !safe_str(r.location) || strlen(r.location) >= sizeof g_kid) {
        problem(&r, "account", err, cap);
        resp_free(&r);
        return -1;
    }
    snprintf(g_kid, sizeof g_kid, "%s", r.location);
    alog("account %s", r.status == 201 ? "created" : "found");
    resp_free(&r);
    char buf[700];
    int n = snprintf(buf, sizeof buf, "%s\n%s\n", g_dir, g_kid);
    save_file(F_ACC_URL, buf, (size_t)n, 0644);
    return 0;
}

/* ---- DNS providers ---------------------------------------------------------------- */
typedef struct { char zone[80], rec[80]; } dnsrec_t;

static int id_ok(const char *s) { if (!*s) return 0; for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-') return 0; return 1; }

static int cf_call(const cfg_t *c, const char *method, const char *path, const char *body, resp_t *r, char *err, size_t cap)
{
    char url[700], hdr[260];
    snprintf(url, sizeof url, "%s%s", c->api[0] ? c->api : "https://api.cloudflare.com/client/v4", path);
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\n", c->token);
    if (http_do(method, url, hdr, body ? "application/json" : NULL, body, body ? strlen(body) : 0, r, err, cap) != 0) return -1;
    jt *t = malloc(JT_API * sizeof *t);                /* heap: keeps the thread's stack small */
    int n = t ? j_parse(r->body, r->len, t, JT_API) : -1;
    char ok[8] = "";
    if (n > 0) j_strk(r->body, t, n, 0, "success", ok, sizeof ok);
    if (r->status / 100 == 2 && !strcmp(ok, "true")) { free(t); return 0; }
    char msg[160] = "";
    int errs = n > 0 ? j_get(r->body, t, n, 0, "errors") : -1;
    if (errs >= 0 && t[errs].type == J_ARR && errs + 1 < n && t[errs + 1].start < t[errs].end)
        j_strk(r->body, t, n, errs + 1, "message", msg, sizeof msg);
    free(t);
    snprintf(err, cap, "Cloudflare: %s (HTTP %d)", msg[0] ? msg : "request refused", r->status);
    resp_free(r);
    return -1;
}

static int cf_set(const cfg_t *c, const char *base, const char *fqdn, const char *txt, dnsrec_t *d, char *err, size_t cap)
{
    char cand[254], path[400];
    snprintf(cand, sizeof cand, "%s", base);
    d->zone[0] = 0;
    while (strchr(cand, '.')) {                        /* cam.example.org, example.org */
        resp_t r;
        snprintf(path, sizeof path, "/zones?name=%s", cand);
        if (cf_call(c, "GET", path, NULL, &r, err, cap) != 0) return -1;
        jt *t = malloc(JT_API * sizeof *t);
        int n = t ? j_parse(r.body, r.len, t, JT_API) : -1;
        int res = n > 0 ? j_get(r.body, t, n, 0, "result") : -1;
        if (res >= 0 && t[res].type == J_ARR && res + 1 < n && t[res + 1].start < t[res].end)
            j_strk(r.body, t, n, res + 1, "id", d->zone, sizeof d->zone);
        free(t);
        resp_free(&r);
        if (d->zone[0]) break;
        memmove(cand, strchr(cand, '.') + 1, strlen(strchr(cand, '.') + 1) + 1);
    }
    if (!id_ok(d->zone)) {
        snprintf(err, cap, "no Cloudflare zone found for %s (the token needs Zone:Read and DNS:Edit)", base);
        return -1;
    }
    char body[400];
    snprintf(body, sizeof body, "{\"type\":\"TXT\",\"name\":\"%s\",\"content\":\"%s\",\"ttl\":60}", fqdn, txt);
    snprintf(path, sizeof path, "/zones/%s/dns_records", d->zone);
    resp_t r;
    if (cf_call(c, "POST", path, body, &r, err, cap) != 0) return -1;
    jt t[128];
    int n = j_parse(r.body, r.len, t, 128);
    int res = n > 0 ? j_get(r.body, t, n, 0, "result") : -1;
    j_strk(r.body, t, n, res, "id", d->rec, sizeof d->rec);
    resp_free(&r);
    if (!id_ok(d->rec)) { snprintf(err, cap, "Cloudflare did not return the record id"); return -1; }
    alog("Cloudflare: TXT %s created", fqdn);
    return 0;
}

static void cf_del(const cfg_t *c, const dnsrec_t *d)
{
    if (!id_ok(d->zone) || !id_ok(d->rec)) return;
    char path[300], err[200];
    snprintf(path, sizeof path, "/zones/%s/dns_records/%s", d->zone, d->rec);
    resp_t r;
    if (cf_call(c, "DELETE", path, NULL, &r, err, sizeof err) == 0) { resp_free(&r); alog("Cloudflare: TXT record removed"); }
    else alog("could not remove the TXT record: %s", err);
}

static int duck(const cfg_t *c, const char *base, const char *txt, int clear, char *err, size_t cap)
{
    size_t bl = strlen(base);
    if (bl <= 12 || strcmp(base + bl - 12, ".duckdns.org") != 0) {
        snprintf(err, cap, "%s is not a duckdns.org name", base);
        return -1;
    }
    const char *end = base + bl - 12, *st = end;
    while (st > base && st[-1] != '.') st--;           /* the DuckDNS domain: label before .duckdns.org */
    char url[600];
    snprintf(url, sizeof url, "%s/update?domains=%.*s&token=%s&txt=%s&verbose=true%s",
             c->api[0] ? c->api : "https://www.duckdns.org", (int)(end - st), st, c->token,
             clear ? "removed" : txt, clear ? "&clear=true" : "");
    resp_t r;
    if (http_do("GET", url, NULL, NULL, NULL, 0, &r, err, cap) != 0) return -1;
    int ok = r.status == 200 && !strncmp(r.body, "OK", 2);
    if (!ok) {
        char b[60];
        snprintf(b, sizeof b, "%.50s", r.body);
        clean(b);
        snprintf(err, cap, "DuckDNS refused the update (check the token and the domain): HTTP %d %s", r.status, b);
    }
    resp_free(&r);
    if (ok) alog("DuckDNS: TXT %s", clear ? "cleared" : "set");
    return ok ? 0 : -1;
}

static int acmedns_set(const cfg_t *c, const char *txt, char *err, size_t cap)
{
    if (!c->api[0] || !c->user[0] || !c->token[0] || !c->sub[0]) {
        snprintf(err, cap, "acme-dns: set the server and register first");
        return -1;
    }
    char url[300], hdr[300], body[200];
    snprintf(url, sizeof url, "%s/update", c->api);
    snprintf(hdr, sizeof hdr, "X-Api-User: %s\r\nX-Api-Key: %s\r\n", c->user, c->token);
    snprintf(body, sizeof body, "{\"subdomain\":\"%s\",\"txt\":\"%s\"}", c->sub, txt);
    resp_t r;
    if (http_do("POST", url, hdr, "application/json", body, strlen(body), &r, err, cap) != 0) return -1;
    int ok = r.status == 200;
    if (!ok) {
        char b[80];
        snprintf(b, sizeof b, "%.70s", r.body);
        clean(b);
        snprintf(err, cap, "acme-dns refused the update: HTTP %d %s", r.status, b);
    }
    resp_free(&r);
    if (ok) alog("acme-dns: TXT set");
    return ok ? 0 : -1;
}

int acme_dns_register(char *out, size_t cap, char *err, size_t errcap)
{
    cfg_t c;
    cfg_snapshot(&c);
    if (!c.api[0]) { snprintf(err, errcap, "enter the acme-dns server URL first"); return -1; }
    if (!tls_ready()) { snprintf(err, errcap, "HTTPS is not running yet"); return -1; }
    char url[300];
    snprintf(url, sizeof url, "%s/register", c.api);
    resp_t r;
    if (http_do("POST", url, NULL, NULL, NULL, 0, &r, err, errcap) != 0) return -1;
    char user[80] = "", pass[160] = "", full[200] = "", sub[80] = "";
    jt t[64];
    int n = r.status == 201 || r.status == 200 ? j_parse(r.body, r.len, t, 64) : -1;
    if (n > 0) {
        j_strk(r.body, t, n, 0, "username", user, sizeof user);
        j_strk(r.body, t, n, 0, "password", pass, sizeof pass);
        j_strk(r.body, t, n, 0, "fulldomain", full, sizeof full);
        j_strk(r.body, t, n, 0, "subdomain", sub, sizeof sub);
    }
    int st = r.status;
    resp_free(&r);
    const char *e = NULL;
    if (n <= 0 || !user[0] || !pass[0] || !full[0] || !sub[0]) { snprintf(err, errcap, "acme-dns registration failed (HTTP %d)", st); return -1; }
    if (control_set_value("acme.dns_user", user, &e) || control_set_value("acme.dns_token", pass, &e) ||
        control_set_value("acme.dns_sub", sub, &e) || control_set_value("acme.dns_target", full, &e)) {
        snprintf(err, errcap, "acme-dns returned unexpected values (%s)", e ? e : "?");
        return -1;
    }
    mbedtls_platform_zeroize(pass, sizeof pass);
    alog("acme-dns: registered, CNAME target %s", full);
    return snprintf(out, cap, "{\"fulldomain\":\"%s\"}", s_target);
}

/* ---- one issuance --------------------------------------------------------------------- */
static int poll_status(const char *url, char *status, size_t scap, resp_t *keep, char *err, size_t cap)
{
    /* POST-as-GET until the status is final; keep = last response (caller frees) */
    for (int i = 0; i < 60; i++) {
        if (jws_post(url, NULL, keep, err, cap) != 0) return -1;
        if (keep->status != 200) { problem(keep, "status", err, cap); resp_free(keep); return -1; }
        jt tt[512];
        int m = j_parse(keep->body, keep->len, tt, 512);
        j_strk(keep->body, tt, m, 0, "status", status, scap);
        if (strcmp(status, "pending") && strcmp(status, "processing")) return 0;
        int wait = keep->retry_after > 0 ? (keep->retry_after < 10 ? keep->retry_after : 10) : 2;   /* at most 10 min in all */
        resp_free(keep);
        sleep((unsigned)wait);
    }
    snprintf(err, cap, "the ACME server did not finish in time");
    return -1;
}

static int do_authz(const cfg_t *c, const char *url, int slot, char *err, size_t cap)
{
    resp_t r;
    if (jws_post(url, NULL, &r, err, cap) != 0) return -1;
    if (r.status != 200) { problem(&r, "authorization", err, cap); resp_free(&r); return -1; }
    jt t[512];
    int n = j_parse(r.body, r.len, t, 512);
    char status[24] = "", value[254] = "", ctok[128] = "", curl[400] = "";
    const char *want = c->chal == CH_HTTP ? "http-01" : "dns-01";
    if (n > 0) {
        j_strk(r.body, t, n, 0, "status", status, sizeof status);
        j_strk(r.body, t, n, j_get(r.body, t, n, 0, "identifier"), "value", value, sizeof value);
        int ch = j_get(r.body, t, n, 0, "challenges");
        for (int e = ch >= 0 && t[ch].type == J_ARR ? ch + 1 : n; e < n && t[e].start < t[ch].end; e = j_next(t, n, e)) {
            char type[24] = "";
            j_strk(r.body, t, n, e, "type", type, sizeof type);
            if (!strcmp(type, want)) {
                j_strk(r.body, t, n, e, "token", ctok, sizeof ctok);
                j_strk(r.body, t, n, e, "url", curl, sizeof curl);
            }
        }
    }
    resp_free(&r);
    if (!strcmp(status, "valid")) { alog("%s: already validated", value); return 0; }
    if (strcmp(status, "pending") != 0) { snprintf(err, cap, "authorization for %s is %s", value, status[0] ? status : "unreadable"); return -1; }
    if (!ctok[0] || !curl[0]) { snprintf(err, cap, "the server offers no %s challenge for %s", want, value); return -1; }
    for (const char *p = ctok; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) { snprintf(err, cap, "invalid challenge token"); return -1; }
    if (!dns_name_ok(value)) { snprintf(err, cap, "invalid identifier from the server"); return -1; }

    char ka[256], txt[48], fqdn[300];
    snprintf(ka, sizeof ka, "%s.%s", ctok, g_thumb);
    unsigned char h[32];
    mbedtls_sha256((unsigned char *)ka, strlen(ka), h, 0);
    b64url(h, 32, txt, sizeof txt);
    snprintf(fqdn, sizeof fqdn, "_acme-challenge.%s", value);
    dnsrec_t rec;
    memset(&rec, 0, sizeof rec);
    int ret = -1, published = 0;
    if (c->chal == CH_HTTP) {
        http01_set(slot, ctok, ka);
        alog("%s: http-01, answering on port 80", value);
    } else {
        int wait = c->wait;
        if (c->chal == CH_CLOUDFLARE) { if (cf_set(c, value, fqdn, txt, &rec, err, cap)) goto cleanup; if (!wait) wait = 20; }
        else if (c->chal == CH_DUCKDNS) { if (duck(c, value, txt, 0, err, cap)) goto cleanup; if (!wait) wait = 60; }
        else { if (acmedns_set(c, txt, err, cap)) goto cleanup; if (!wait) wait = 10; }
        published = 1;
        alog("%s: waiting %d s for DNS", value, wait);
        sleep((unsigned)wait);
    }
    if (jws_post(curl, "{}", &r, err, cap) != 0) goto cleanup;
    if (r.status != 200) { problem(&r, "challenge", err, cap); resp_free(&r); goto cleanup; }
    resp_free(&r);
    char st[24] = "";
    if (poll_status(url, st, sizeof st, &r, err, cap) != 0) goto cleanup;
    if (!strcmp(st, "valid")) {
        alog("%s: validated", value);
        ret = 0;
    } else {
        /* the failed challenge carries the reason */
        char detail[200] = "";
        n = j_parse(r.body, r.len, t, 512);
        int ch = n > 0 ? j_get(r.body, t, n, 0, "challenges") : -1;
        for (int e = ch >= 0 && t[ch].type == J_ARR ? ch + 1 : n; e < n && t[e].start < t[ch].end && !detail[0]; e = j_next(t, n, e))
            j_strk(r.body, t, n, j_get(r.body, t, n, e, "error"), "detail", detail, sizeof detail);
        snprintf(err, cap, "%s: validation failed: %s", value, detail[0] ? detail : st);
    }
    resp_free(&r);
cleanup:
    if (c->chal == CH_HTTP) http01_set(slot, NULL, NULL);
    else if (c->chal == CH_CLOUDFLARE) cf_del(c, &rec);
    else if (c->chal == CH_DUCKDNS && published) { char e2[200]; duck(c, value, "", 1, e2, sizeof e2); }
    /* acme-dns keeps only the two latest values: nothing to remove */
    return ret;
}

static int make_csr(const char *key_pem, char dom[MAX_DOM][254], int nd, char *out, size_t cap)
{
    mbedtls_pk_context k;
    mbedtls_x509write_csr w;
    mbedtls_pk_init(&k);
    mbedtls_x509write_csr_init(&w);
    int ret = mbedtls_pk_parse_key(&k, (const unsigned char *)key_pem, strlen(key_pem) + 1, NULL, 0, tls_rng, NULL);
    mbedtls_x509_san_list san[MAX_DOM];
    memset(san, 0, sizeof san);
    for (int i = 0; i < nd; i++) {
        san[i].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san[i].node.san.unstructured_name.p = (unsigned char *)dom[i];
        san[i].node.san.unstructured_name.len = strlen(dom[i]);
        san[i].next = i + 1 < nd ? &san[i + 1] : NULL;
    }
    if (!ret) {
        mbedtls_x509write_csr_set_md_alg(&w, MBEDTLS_MD_SHA256);
        mbedtls_x509write_csr_set_key(&w, &k);
        if (strlen(dom[0]) <= 64) {
            char subj[80];
            snprintf(subj, sizeof subj, "CN=%s", dom[0]);
            ret = mbedtls_x509write_csr_set_subject_name(&w, subj);
        }
    }
    if (!ret) ret = mbedtls_x509write_csr_set_subject_alternative_name(&w, san);
    unsigned char der[1500];
    int n = ret ? -1 : mbedtls_x509write_csr_der(&w, der, sizeof der, tls_rng, NULL);
    mbedtls_x509write_csr_free(&w);
    mbedtls_pk_free(&k);
    if (n <= 0) return -1;
    b64url(der + sizeof der - n, (size_t)n, out, cap);
    return 0;
}

static int issue(char *err, size_t cap)
{
    cfg_t c;
    cfg_snapshot(&c);
    char dom[MAX_DOM][254];
    int nd = parse_domains(c.domains, dom);
    if (nd <= 0) { snprintf(err, cap, "enter the domain names first"); return -1; }
    for (int i = 0; i < nd; i++) {
        if (c.chal == CH_HTTP && dom[i][0] == '*') { snprintf(err, cap, "wildcard names need a DNS challenge"); return -1; }
        if (c.chal == CH_DUCKDNS) {
            const char *b = dom[i][0] == '*' ? dom[i] + 2 : dom[i];
            size_t l = strlen(b);
            if (l <= 12 || strcmp(b + l - 12, ".duckdns.org")) { snprintf(err, cap, "%s is not a duckdns.org name", dom[i]); return -1; }
        }
    }
    if ((c.chal == CH_CLOUDFLARE || c.chal == CH_DUCKDNS) && !c.token[0]) { snprintf(err, cap, "enter the API token first"); return -1; }
    if (c.chal == CH_ACMEDNS && (!c.api[0] || !c.user[0] || !c.token[0] || !c.sub[0])) {
        snprintf(err, cap, "acme-dns: enter the server and register first");
        return -1;
    }
    const char *dir = !strcmp(c.server, "letsencrypt") ? LE_PROD : !strcmp(c.server, "staging") ? LE_STAGING : c.server;
    alog("requesting a certificate for %s%s (%s, %s)", dom[0], nd > 1 ? ",..." : "",
         !strcmp(c.server, "letsencrypt") ? "Let's Encrypt" : !strcmp(c.server, "staging") ? "Let's Encrypt staging" : "custom server",
         c.chal == CH_HTTP ? "http-01" : k_chal[c.chal]);
    if (directory(dir, err, cap) != 0 || account_key(err, cap) != 0) return -1;
    if (!g_kid[0] && account(&c, 0, err, cap) != 0) return -1;

    char order[700];
    size_t o = (size_t)snprintf(order, sizeof order, "{\"identifiers\":[");
    for (int i = 0; i < nd; i++)
        o += (size_t)snprintf(order + o, sizeof order - o, "%s{\"type\":\"dns\",\"value\":\"%s\"}", i ? "," : "", dom[i]);
    snprintf(order + o, sizeof order - o, "]}");
    resp_t r;
    for (int attempt = 0; ; attempt++) {
        if (jws_post(g_u_order, order, &r, err, cap) != 0) return -1;
        if (r.status == 201) break;
        /* cached account gone (e.g. another server at the same URL): register again, once */
        if (attempt == 0 && (strstr(r.body, "accountDoesNotExist") || (r.status == 401 || r.status == 403))) {
            resp_free(&r);
            alog("account not accepted, registering again");
            if (account(&c, 1, err, cap) != 0) return -1;
            continue;
        }
        problem(&r, "new order", err, cap);
        resp_free(&r);
        return -1;
    }
    char ourl[400], fin[400], authz[MAX_DOM][400];
    int na = 0;
    snprintf(ourl, sizeof ourl, "%s", r.location);
    jt t[256];
    int n = j_parse(r.body, r.len, t, 256);
    fin[0] = 0;
    if (n > 0) {
        j_strk(r.body, t, n, 0, "finalize", fin, sizeof fin);
        int a = j_get(r.body, t, n, 0, "authorizations");
        for (int e = a >= 0 && t[a].type == J_ARR ? a + 1 : n; e < n && t[e].start < t[a].end && na < MAX_DOM; e = j_next(t, n, e))
            j_str(r.body, t, e, authz[na++], sizeof authz[0]);
    }
    resp_free(&r);
    if (!ourl[0] || !fin[0] || !na) { snprintf(err, cap, "unexpected order from the ACME server"); return -1; }

    for (int i = 0; i < na; i++)
        if (do_authz(&c, authz[i], i, err, cap) != 0) return -1;

    /* new key for every certificate; the CSR names it */
    char *key = malloc(1024), *csr = malloc(2200), *pl = malloc(2300);
    int ret = -1;
    if (!key || !csr || !pl) { snprintf(err, cap, "out of memory"); goto out; }
    if (tls_key_new_pem(key, 1024) < 0 || make_csr(key, dom, nd, csr, 2200) != 0) { snprintf(err, cap, "cannot create the key / CSR"); goto out; }
    snprintf(pl, 2300, "{\"csr\":\"%s\"}", csr);
    if (jws_post(fin, pl, &r, err, cap) != 0) goto out;
    if (r.status != 200) { problem(&r, "finalize", err, cap); resp_free(&r); goto out; }
    resp_free(&r);
    char st[24] = "", curl[400] = "";
    if (poll_status(ourl, st, sizeof st, &r, err, cap) != 0) goto out;
    n = j_parse(r.body, r.len, t, 256);
    if (n > 0) j_strk(r.body, t, n, 0, "certificate", curl, sizeof curl);
    resp_free(&r);
    if (strcmp(st, "valid") || !curl[0]) { snprintf(err, cap, "the order ended as %s", st); goto out; }
    if (jws_post(curl, NULL, &r, err, cap) != 0) goto out;
    if (r.status != 200 || !strstr(r.body, "-----BEGIN CERTIFICATE-----")) { problem(&r, "download", err, cap); resp_free(&r); goto out; }
    size_t cl = r.len, kl = strlen(key);
    char *all = malloc(cl + kl + 2);
    if (!all) { resp_free(&r); snprintf(err, cap, "out of memory"); goto out; }
    memcpy(all, r.body, cl);
    all[cl] = '\n';
    memcpy(all + cl + 1, key, kl + 1);
    resp_free(&r);
    const char *e = NULL;
    if (tls_custom_install(all, cl + 1 + kl, "acme", &e) != 0) snprintf(err, cap, "the new certificate was refused: %s", e);
    else ret = 0;
    mbedtls_platform_zeroize(all, cl + kl + 2);
    free(all);
out:
    if (key) mbedtls_platform_zeroize(key, 1024);
    free(key); free(csr); free(pl);
    return ret;
}

/* ---- scheduling ------------------------------------------------------------------------ */
/* 1 + reason if the custom certificate must be (re)issued */
static int needs_cert(char *why, size_t cap)
{
    char names[CUS_NAMES_LIST], src[8];
    long left = 0;
    if (!tls_custom_state(names, sizeof names, src, &left)) { snprintf(why, cap, "no certificate yet"); return 1; }
    if (strcmp(src, "acme")) { snprintf(why, cap, "replacing the uploaded certificate"); return 1; }
    char dom[MAX_DOM][254];
    int nd = parse_domains(s_domains, dom), have = 1;
    for (char *p = names; *p; p++) if (*p == ',') have++;
    int all = nd > 0 && nd == have;
    for (int i = 0; all && i < nd; i++) {
        char pat[260];
        snprintf(pat, sizeof pat, ",%s,", dom[i]);
        char list[CUS_NAMES_LIST + 2];
        snprintf(list, sizeof list, ",%s,", names);
        all = strstr(list, pat) != NULL;
    }
    if (!all) { snprintf(why, cap, "the domain names changed"); return 1; }
    if (left < RENEW_SECS) { snprintf(why, cap, "renewal (expires in %ld days)", left / 86400); return 1; }
    return 0;
}

static void *acme_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(2);
        if (g_kick) { g_kick = 0; g_fail = 0; g_next = time(NULL) + 3; }   /* settings changed: try soon */
        if (g_reg) {                       /* acme-dns registration: network here, never in the web thread */
            g_reg = 0;
            char out[300], err[200];
            set_state("running", "registering with the acme-dns server");
            if (acme_dns_register(out, sizeof out, err, sizeof err) < 0) { alog("acme-dns registration failed: %s", err); set_state("error", err); }
            else set_state("waiting", "registered: create the CNAME records, then get the certificate");
            continue;
        }
        if (!s_en) {
            if (g_run) { g_run = 0; set_state("off", "ACME is off"); }
            else if (strcmp(g_state, "off")) set_state("off", "");
            continue;
        }
        if (!tls_ready()) { set_state("waiting", "HTTPS is not running yet"); continue; }
        if (!tls_clock_ok()) { set_state("waiting", "waiting for the clock (NTP)"); continue; }
        char why[120];
        time_t now = time(NULL);
        int need = needs_cert(why, sizeof why);
        if (!g_run && (!need || now < g_next)) {
            if (!need) {
                char names[CUS_NAMES_LIST], src[8], m[160];
                long left = 0;
                tls_custom_state(names, sizeof names, src, &left);
                snprintf(m, sizeof m, "certificate valid for %ld more days, renewed 30 days before it expires", left / 86400);
                set_state("ok", m);
            }
            continue;
        }
        if (g_run) snprintf(why, sizeof why, "requested from the page");
        g_run = 0;
        set_state("running", why);
        alog("starting: %s", why);
        char err[300];
        int ok = issue(err, sizeof err) == 0;
        g_run = 0;                         /* a "run now" pressed during this run is not queued */
        if (ok) {
            g_fail = 0;
            pthread_mutex_lock(&g_st);
            g_last_ok = time(NULL);
            g_next = g_last_ok + MIN_GAP_OK;
            pthread_mutex_unlock(&g_st);
            alog("certificate installed");
            set_state("ok", "certificate installed");
        } else {
            g_fail++;
            long d = 3600L << (g_fail > 5 ? 5 : g_fail - 1);
            if (d > 86400) d = 86400;
            pthread_mutex_lock(&g_st);
            g_next = time(NULL) + d;
            pthread_mutex_unlock(&g_st);
            alog("failed: %s", err);
            alog("next automatic attempt in %ld min", d / 60);
            set_state("error", err);
        }
    }
    return NULL;
}

void acme_init(void)
{
#define REGK(name, fn, fl) do { ctrl_param_t p_ = { name, fn##_get, fn##_setk, fl }; control_register(&p_); } while (0)
    REGK("acme.enabled", p_en, CTRL_PERSIST);
    REGK("acme.server", p_server, CTRL_PERSIST);
    REGK("acme.email", p_email, CTRL_PERSIST);
    REGK("acme.domains", p_domains, CTRL_PERSIST);
    REGK("acme.challenge", p_chal, CTRL_PERSIST);
    REGK("acme.dns_api", p_api, CTRL_PERSIST);
    REGK("acme.dns_token", p_token, CTRL_PERSIST | CTRL_SECRET);
    REGK("acme.dns_user", p_user, CTRL_PERSIST);
    REGK("acme.dns_sub", p_sub, CTRL_PERSIST);
    REGK("acme.dns_target", p_target, CTRL_PERSIST);
    REGK("acme.dns_wait", p_wait, CTRL_PERSIST);
#undef REGK
}

void acme_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 160 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, acme_thread, NULL);
    pthread_attr_destroy(&a);
}
