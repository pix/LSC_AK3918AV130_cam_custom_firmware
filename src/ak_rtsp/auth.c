#include "auth.h"
#include "md5.h"
#include "monotime.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NONCE_LIFETIME_S (12 * 3600)  /* RTSP clients keep one nonce per session */
#define NONCE_STRICT_S   300          /* web / ONVIF: short-lived, renewed with stale=true */
#define MAX_USER 32
#define MAX_PASS 64

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char     g_user[MAX_USER + 1];
static char     g_ha1[33];
static int      g_default = 1;
static uint8_t  g_secret[16];   /* per-boot nonce signing key */

/* ---- helpers ------------------------------------------------------------ */
static int ct_equal(const char *a, const char *b, size_t n)
{
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

static void compute_ha1(const char *user, const char *pass, char out[33])
{
    md5_ctx c; uint8_t d[16];
    md5_init(&c);
    md5_update(&c, user, strlen(user));
    md5_update(&c, ":" AUTH_REALM ":", strlen(":" AUTH_REALM ":"));
    md5_update(&c, pass, strlen(pass));
    md5_final(&c, d);
    md5_to_hex(d, out);
}

static uint32_t now_s(void)
{
    return mono_s();  /* see monotime.h: CLOCK_MONOTONIC is broken here */
}

static void load_defaults_locked(void)
{
    snprintf(g_user, sizeof(g_user), "%s", AUTH_DEF_USER);
    compute_ha1(AUTH_DEF_USER, AUTH_DEF_PASS, g_ha1);
    g_default = 1;
}

static int valid_user(const char *u)
{
    size_t n = strlen(u);
    if (n < 1 || n > MAX_USER) return 0;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)u[i]) || u[i] == '.' || u[i] == '_' || u[i] == '-'))
            return 0;
    return 1;
}

static int valid_pass(const char *p)
{
    size_t n = strlen(p);
    if (n < 4 || n > MAX_PASS) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] == 0x7f) return 0;
    return 1;
}

/* ---- store ---------------------------------------------------------------- */
void auth_init(void)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, g_secret, sizeof(g_secret)) != (ssize_t)sizeof(g_secret)) {
        /* Fallback: still unpredictable enough across boots for nonces. */
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        for (size_t i = 0; i < sizeof(g_secret); i++)
            g_secret[i] = (uint8_t)((ts.tv_nsec >> (i % 4) * 8) ^ (getpid() * 131 + i));
    }
    if (fd >= 0) close(fd);

    pthread_mutex_lock(&g_lock);
    load_defaults_locked();
    FILE *f = fopen(AUTH_FILE, "r");
    if (f) {
        char user[64] = "", ha1[64] = "";
        if (fgets(user, sizeof(user), f) && fgets(ha1, sizeof(ha1), f)) {
            user[strcspn(user, "\r\n")] = 0;
            ha1[strcspn(ha1, "\r\n")] = 0;
            if (valid_user(user) && strlen(ha1) == 32) {
                snprintf(g_user, sizeof(g_user), "%s", user);
                memcpy(g_ha1, ha1, 33);
                g_default = 0;
            }
        }
        fclose(f);
    }
    printf("[auth] login user '%s'%s\n", g_user, g_default ? " (DEFAULT admin/admin)" : "");
    pthread_mutex_unlock(&g_lock);
}

int auth_is_default(void)
{
    pthread_mutex_lock(&g_lock);
    int d = g_default;
    pthread_mutex_unlock(&g_lock);
    return d;
}

void auth_get_user(char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    snprintf(out, cap, "%s", g_user);
    pthread_mutex_unlock(&g_lock);
}

int auth_set(const char *user, const char *pass, const char **err)
{
    if (!valid_user(user)) { *err = "username: 1-32 chars, letters/digits . _ -"; return -1; }
    if (!valid_pass(pass)) { *err = "password: 4-64 printable characters"; return -1; }

    char ha1[33];
    compute_ha1(user, pass, ha1);

    /* Atomic replace on JFFS2: write temp, fsync, rename. */
    const char *tmp = AUTH_FILE ".tmp";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { *err = "cannot write credentials file"; return -1; }
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\n%s\n", user, ha1);
    if (write(fd, buf, (size_t)n) != n || fsync(fd) != 0) {
        close(fd); unlink(tmp); *err = "write failed"; return -1;
    }
    close(fd);
    if (rename(tmp, AUTH_FILE) != 0) { unlink(tmp); *err = "rename failed"; return -1; }
    sync();

    pthread_mutex_lock(&g_lock);
    snprintf(g_user, sizeof(g_user), "%s", user);
    memcpy(g_ha1, ha1, 33);
    g_default = 0;
    pthread_mutex_unlock(&g_lock);
    printf("[auth] credentials changed (user '%s')\n", user);
    /* ONVIF WS-Security "PasswordDigest" needs the password itself (the
     * login above stores only a hash): keep a copy readable by root only. */
    int pf = open(AUTH_ONVIF_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (pf >= 0) {
        int ok = write(pf, pass, strlen(pass)) == (ssize_t)strlen(pass) && fsync(pf) == 0;
        close(pf);
        if (!ok || rename(AUTH_ONVIF_FILE ".tmp", AUTH_ONVIF_FILE) != 0) { unlink(AUTH_ONVIF_FILE ".tmp"); unlink(AUTH_ONVIF_FILE); }
    } else unlink(AUTH_ONVIF_FILE);            /* never leave the previous password usable */
    return 0;
}

/* Password for ONVIF digest checks: the default one, or the copy saved by
 * auth_set. -1 if unknown (password set by an older firmware). */
int auth_onvif_password(char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    int def = g_default;
    pthread_mutex_unlock(&g_lock);
    if (def) { snprintf(out, cap, "admin"); return 0; }
    int fd = open(AUTH_ONVIF_FILE, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    out[n] = 0;
    return 0;
}

void auth_reset(void)
{
    unlink(AUTH_FILE);
    unlink(AUTH_ONVIF_FILE);                   /* the old password must not survive a reset */
    sync();
    pthread_mutex_lock(&g_lock);
    load_defaults_locked();
    pthread_mutex_unlock(&g_lock);
    printf("[auth] credentials RESET to default admin/admin\n");
}

int auth_check_plain(const char *user, const char *pass)
{
    char ha1[33];
    compute_ha1(user, pass, ha1);
    pthread_mutex_lock(&g_lock);
    int ok = strlen(user) == strlen(g_user) &&
             ct_equal(user, g_user, strlen(user)) & ct_equal(ha1, g_ha1, 32);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ---- nonces: "<8 hex time><16 hex MAC>" , MAC = MD5(secret:time)[0..8] -- */
static void make_nonce(uint32_t t, char out[25])
{
    char th[9];
    snprintf(th, sizeof(th), "%08x", t);
    md5_ctx c; uint8_t d[16]; char hex[33];
    md5_init(&c);
    md5_update(&c, g_secret, sizeof(g_secret));
    md5_update(&c, th, 8);
    md5_final(&c, d);
    md5_to_hex(d, hex);
    memcpy(out, th, 8);
    memcpy(out + 8, hex, 16);
    out[24] = 0;
}

/* 1 valid, -1 well-formed but expired, 0 forged/malformed */
static int check_nonce(const char *nonce, uint32_t max_age)
{
    if (strlen(nonce) != 24) return 0;
    char th[9]; memcpy(th, nonce, 8); th[8] = 0;
    char *end;
    uint32_t t = (uint32_t)strtoul(th, &end, 16);
    if (*end) return 0;
    char expect[25];
    make_nonce(t, expect);
    if (!ct_equal(expect, nonce, 24)) return 0;
    uint32_t now = now_s();
    if (now - t > max_age) return -1;
    return 1;
}

void auth_digest_challenge(char *out, size_t cap, int with_qop, int stale)
{
    char nonce[25];
    make_nonce(now_s(), nonce);
    snprintf(out, cap, "Digest realm=\"%s\", nonce=\"%s\", algorithm=MD5%s%s",
             AUTH_REALM, nonce,
             with_qop ? ", qop=\"auth\"" : "",
             stale ? ", stale=true" : "");
}

/* Extract key="value" or key=value from a Digest parameter list. */
static int get_param(const char *s, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!strncasecmp(p, key, klen) && p[klen] == '=') {
            p += klen + 1;
            size_t n = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) p++;
                    if (n + 1 < cap) out[n++] = *p;
                    p++;
                }
            } else {
                while (*p && *p != ',' && *p != ' ') {
                    if (n + 1 < cap) out[n++] = *p;
                    p++;
                }
            }
            out[n] = 0;
            return 1;
        }
        /* skip this param */
        while (*p && *p != '=' && *p != ',') p++;
        if (*p == '=') {
            p++;
            if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; } if (*p) p++; }
            else while (*p && *p != ',') p++;
        }
    }
    return 0;
}

/* ---- replay protection for qop=auth: per (nonce, cnonce), the highest nc
 * seen plus a 32-entry window below it (requests may arrive slightly out of
 * order). Nonces are stateless and change once a second, so every client
 * that asks within the same second gets the same nonce; each client counts
 * nc from 1 with its own cnonce. Keyed by nonce alone (before 2026-11-12),
 * a second client's nc=1 looked like a replay and was refused, which made
 * pages loading many settings at once fail at random. */
#define NC_SLOTS 64
static struct { char key[33]; uint32_t max_nc, seen; uint32_t used; } g_nc[NC_SLOTS];

/* 1 = first use of this nc for this nonce + cnonce, 0 = replay / too old */
static int nc_fresh(const char *nonce, const char *cnonce, const char *nc_hex)
{
    char *end;
    unsigned long nc = strtoul(nc_hex, &end, 16);
    if (*end || nc == 0 || strlen(nc_hex) != 8) return 0;
    char key[33];
    {
        md5_ctx c; uint8_t d[16];
        md5_init(&c);
        md5_update(&c, nonce, strlen(nonce));
        md5_update(&c, ":", 1);
        md5_update(&c, cnonce, strlen(cnonce));
        md5_final(&c, d);
        md5_to_hex(d, key);
    }
    uint32_t now = now_s();
    pthread_mutex_lock(&g_lock);
    int slot = -1, victim = 0;
    for (int i = 0; i < NC_SLOTS; i++) {
        if (!strcmp(g_nc[i].key, key)) { slot = i; break; }
        if (g_nc[i].used < g_nc[victim].used) victim = i;
    }
    int ok = 1;
    if (slot < 0) {
        slot = victim;
        memcpy(g_nc[slot].key, key, sizeof key);
        g_nc[slot].max_nc = (uint32_t)nc; g_nc[slot].seen = 1;
    } else {
        uint32_t mx = g_nc[slot].max_nc;
        if (nc > mx) {
            uint32_t sh = (uint32_t)nc - mx;
            g_nc[slot].seen = sh >= 32 ? 1 : (g_nc[slot].seen << sh) | 1;
            g_nc[slot].max_nc = (uint32_t)nc;
        } else if (mx - nc < 32 && !(g_nc[slot].seen & (1u << (mx - nc)))) {
            g_nc[slot].seen |= 1u << (mx - nc);
        } else ok = 0;
    }
    g_nc[slot].used = now ? now : 1;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int auth_digest_verify(const char *authz, const char *method)
{
    return auth_digest_verify_ex(authz, method, NULL);
}

/* strict (req_uri != NULL; web and ONVIF): the digest uri must be this
 * request target, qop=auth with a fresh nc is required and nonces live
 * NONCE_STRICT_S. RTSP (req_uri NULL) keeps the long-lived RFC 2069 style. */
int auth_digest_verify_ex(const char *authz, const char *method, const char *req_uri)
{
    if (!authz) return 0;
    while (*authz == ' ') authz++;
    if (strncasecmp(authz, "Digest ", 7) != 0) return 0;
    const char *params = authz + 7;

    char user[64], realm[64], nonce[64], uri[512], resp[64];
    char qop[16] = "", nc[16] = "", cnonce[128] = "", alg[16] = "";
    if (!get_param(params, "username", user, sizeof(user)) ||
        !get_param(params, "realm", realm, sizeof(realm)) ||
        !get_param(params, "nonce", nonce, sizeof(nonce)) ||
        !get_param(params, "uri", uri, sizeof(uri)) ||
        !get_param(params, "response", resp, sizeof(resp)))
        return 0;
    get_param(params, "qop", qop, sizeof(qop));
    get_param(params, "nc", nc, sizeof(nc));
    get_param(params, "cnonce", cnonce, sizeof(cnonce));
    get_param(params, "algorithm", alg, sizeof(alg));

    if (strcmp(realm, AUTH_REALM) != 0) return 0;
    if (alg[0] && strcasecmp(alg, "MD5") != 0) return 0;
    if (strlen(resp) != 32) return 0;
    for (char *c = resp; *c; c++) *c = (char)tolower((unsigned char)*c);

    int strict = req_uri != NULL;
    int nst = check_nonce(nonce, strict ? NONCE_STRICT_S : NONCE_LIFETIME_S);
    if (nst == 0) return 0;
    if (strict) {
        if (strcmp(uri, req_uri) != 0) return 0;       /* credentials of another request */
        if (!qop[0]) return 0;                         /* we always offer qop=auth to browsers */
    }

    char ha2[33];
    {
        md5_ctx c; uint8_t d[16];
        md5_init(&c);
        md5_update(&c, method, strlen(method));
        md5_update(&c, ":", 1);
        md5_update(&c, uri, strlen(uri));
        md5_final(&c, d);
        md5_to_hex(d, ha2);
    }

    pthread_mutex_lock(&g_lock);
    char ha1[33], cur_user[MAX_USER + 1];
    memcpy(ha1, g_ha1, 33);
    memcpy(cur_user, g_user, sizeof(cur_user));
    pthread_mutex_unlock(&g_lock);

    char expect[33];
    {
        md5_ctx c; uint8_t d[16];
        md5_init(&c);
        md5_update(&c, ha1, 32);
        md5_update(&c, ":", 1);
        md5_update(&c, nonce, strlen(nonce));
        md5_update(&c, ":", 1);
        if (qop[0]) {
            if (strcasecmp(qop, "auth") != 0 || !nc[0] || !cnonce[0]) return 0;
            md5_update(&c, nc, strlen(nc));
            md5_update(&c, ":", 1);
            md5_update(&c, cnonce, strlen(cnonce));
            md5_update(&c, ":", 1);
            md5_update(&c, qop, strlen(qop));
            md5_update(&c, ":", 1);
        }
        md5_update(&c, ha2, 32);
        md5_final(&c, d);
        md5_to_hex(d, expect);
    }

    int ok = (strlen(user) == strlen(cur_user)) &
             ct_equal(user, cur_user, strlen(user) < strlen(cur_user) ? strlen(user) : strlen(cur_user)) &
             ct_equal(expect, resp, 32);
    if (!ok) return 0;
    if (nst == -1) return -1;
    if (strict && !nc_fresh(nonce, cnonce, nc)) return -1;     /* replayed: make the client re-authenticate */
    return 1;
}

/* ---- per-address login failure backoff, shared by every front end ----------
 * 5 failures within 10 min lock the address out for 30 s, doubling with each
 * further failure up to 15 min. A success clears it. Loopback is exempt. */
#define FAIL_SLOTS 32
static struct { uint32_t ip, fails, first, until, used; } g_fail[FAIL_SLOTS];

static uint32_t ip_of(const struct sockaddr *sa)
{
    if (!sa || sa->sa_family != AF_INET) return 0;
    uint32_t ip = ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);
    return (ip >> 24) == 127 ? 0 : ip;
}

int auth_throttled(const struct sockaddr *sa)
{
    uint32_t ip = ip_of(sa), now = now_s();
    if (!ip) return 0;
    int left = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < FAIL_SLOTS; i++)
        if (g_fail[i].ip == ip && g_fail[i].until > now) { left = (int)(g_fail[i].until - now); break; }
    pthread_mutex_unlock(&g_lock);
    return left;
}

void auth_record(const struct sockaddr *sa, int ok)
{
    uint32_t ip = ip_of(sa), now = now_s();
    if (!ip) return;
    pthread_mutex_lock(&g_lock);
    int slot = -1, victim = 0;
    for (int i = 0; i < FAIL_SLOTS; i++) {
        if (g_fail[i].ip == ip) { slot = i; break; }
        if (g_fail[i].used < g_fail[victim].used) victim = i;
    }
    if (ok) {
        if (slot >= 0) memset(&g_fail[slot], 0, sizeof g_fail[slot]);
    } else {
        if (slot < 0) { slot = victim; memset(&g_fail[slot], 0, sizeof g_fail[slot]); g_fail[slot].ip = ip; }
        if (now - g_fail[slot].first > 600) { g_fail[slot].fails = 0; g_fail[slot].first = now; }
        g_fail[slot].fails++;
        g_fail[slot].used = now ? now : 1;
        if (g_fail[slot].fails >= 5) {
            uint32_t sh = g_fail[slot].fails - 5, lock = sh >= 5 ? 900 : 30u << sh;
            if (lock > 900) lock = 900;
            g_fail[slot].until = now + lock;
            printf("[auth] %u.%u.%u.%u: %u failed logins, locked out for %u s\n",
                   ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, g_fail[slot].fails, lock);
        }
    }
    pthread_mutex_unlock(&g_lock);
}
