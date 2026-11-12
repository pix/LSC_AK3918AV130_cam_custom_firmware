#include "tls.h"
#include <strings.h>
#include "monotime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cache.h"
#include "mbedtls/ssl_ticket.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "mbedtls/error.h"
#include "mbedtls/base64.h"
#include "mbedtls/pem.h"

#define F_CA_KEY  TLS_DIR "/ak_rtsp_tls_ca.key"
#define F_CA_CRT  TLS_DIR "/ak_rtsp_tls_ca.crt"
#define F_KEY     TLS_DIR "/ak_rtsp_tls.key"
#define F_CRT     TLS_DIR "/ak_rtsp_tls.crt"
#define F_STAMP   TLS_DIR "/ak_rtsp_tls.san"    /* what the server cert was issued for */
#define F_CONF    TLS_DIR "/ak_rtsp_tls.conf"

#define DAY 86400L
#define SRV_DAYS   800      /* Apple: certs from user-installed CAs <= 825 days */
#define RENEW_DAYS 30
#define CA_YEARS   20

/* ---- state ------------------------------------------------------------------ */
typedef struct pair {          /* server certificate + key in use */
    mbedtls_x509_crt crt;
    mbedtls_pk_context key;
    struct pair *prev;         /* superseded pairs are kept (rare, small): a
                                  handshake in progress may still use them */
} pair_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   /* config + issuing */
static pthread_mutex_t g_cur_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_ssl_config g_conf;
static mbedtls_ssl_cache_context g_cache;
static mbedtls_ssl_ticket_context g_ticket;
static pair_t *g_cur;
static int g_ready, g_redirect;
static char g_names[TLS_NAMES_MAX][254];
static int g_nnames;
static char g_ips[160];          /* comma separated, as in the current cert */
static char g_not_after[16];
static char g_err[96];

int tls_ready(void) { return g_ready; }
static int seed_once(void);

/* MBEDTLS_PLATFORM_MS_TIME_ALT: see tls_config.h */
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)mono_ms(); }
int tls_redirect(void) { return g_ready && g_redirect; }

/* ---- small helpers ------------------------------------------------------------ */
static int write_file(const char *path, const void *data, size_t len, int mode)
{
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        p += w; left -= (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int read_file(const char *path, char *buf, size_t cap)   /* NUL-terminated */
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* A floor for the clock (build time, from the Makefile): used as "now" when
 * certificates must be made before NTP has set the clock. */
#ifndef TLS_BUILD_EPOCH
#define TLS_BUILD_EPOCH 1793491200L          /* 2026-11-01 */
#endif
static time_t build_time(void) { return (time_t)TLS_BUILD_EPOCH; }

static time_t now_or_build(int *trusted)
{
    time_t now = time(NULL), b = build_time();
    if (trusted) *trusted = now >= b;
    return now >= b ? now : b;          /* clock not set yet (no NTP): use the build date */
}

static void x509_time(time_t t, char out[15])
{
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, 15, "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void set_err(const char *what, int ret)
{
    char e[64] = "";
    if (ret) mbedtls_strerror(ret, e, sizeof e);
    snprintf(g_err, sizeof g_err, "%s%s%s", what, ret ? ": " : "", e);
    fprintf(stderr, "[tls] %s\n", g_err);
}

/* Current IPv4 addresses (not loopback), sorted, comma separated. */
static void current_ips(char *out, size_t cap)
{
    struct ifaddrs *ifa = NULL, *i;
    char list[8][16];
    int n = 0;
    out[0] = 0;
    if (getifaddrs(&ifa) != 0) return;
    for (i = ifa; i && n < 8; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        struct in_addr a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        if ((ntohl(a.s_addr) >> 24) == 127) continue;
        inet_ntop(AF_INET, &a, list[n], sizeof list[n]);
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= !strcmp(list[k], list[n]);
        if (!dup) n++;
    }
    freeifaddrs(ifa);
    for (int a = 0; a < n; a++)                 /* sort for a stable stamp */
        for (int b = a + 1; b < n; b++)
            if (strcmp(list[a], list[b]) > 0) { char t[16]; memcpy(t, list[a], 16); memcpy(list[a], list[b], 16); memcpy(list[b], t, 16); }
    size_t o = 0;
    for (int k = 0; k < n && o < cap; k++)
        o += (size_t)snprintf(out + o, cap - o, "%s%s", k ? "," : "", list[k]);
}

/* ---- DNS name validation -------------------------------------------------------- */
static int dns_name_ok(const char *s)
{
    size_t n = strlen(s), lab = 0;
    if (n < 1 || n > 253 || s[0] == '.' || s[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') {
            if (lab == 0 || s[i - 1] == '-') return 0;
            lab = 0;
            continue;
        }
        if (!(isalnum((unsigned char)c) || c == '-')) return 0;
        if (c == '-' && lab == 0) return 0;
        if (++lab > 63) return 0;
    }
    return s[n - 1] != '-';
}

/* Parse "a.lan, b.example" into names[]; 0 ok, -1 invalid. */
static int parse_names(const char *in, char names[TLS_NAMES_MAX][254], int *count)
{
    char buf[1100];
    snprintf(buf, sizeof buf, "%s", in);
    if (strlen(in) >= sizeof buf) return -1;
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, ", \t\r\n", &save); t; t = strtok_r(NULL, ", \t\r\n", &save)) {
        for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!dns_name_ok(t)) return -1;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= !strcmp(names[k], t);
        if (dup) continue;
        if (n == TLS_NAMES_MAX) return -1;
        snprintf(names[n++], 254, "%s", t);
    }
    *count = n;
    return 0;
}

static void load_conf(void)
{
    char buf[1400];
    g_nnames = 0;
    g_redirect = 0;
    if (read_file(F_CONF, buf, sizeof buf) < 0) return;
    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(l, "names=", 6)) {
            char tmp[TLS_NAMES_MAX][254];
            int n = 0;
            if (parse_names(l + 6, tmp, &n) == 0) { memcpy(g_names, tmp, sizeof tmp); g_nnames = n; }
        } else if (!strncmp(l, "redirect=", 9)) {
            g_redirect = l[9] == '1';
        }
    }
}

static void names_csv(char names[TLS_NAMES_MAX][254], int n, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    for (int k = 0; k < n && o < cap; k++)
        o += (size_t)snprintf(out + o, cap - o, "%s%s", k ? "," : "", names[k]);
}

/* ---- keys and certificates ------------------------------------------------------ */
static int gen_key(mbedtls_pk_context *pk)
{
    int ret = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (!ret) ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk),
                                        mbedtls_ctr_drbg_random, &g_drbg);
    return ret;
}

static int save_key(mbedtls_pk_context *pk, const char *path)
{
    unsigned char buf[800];
    int ret = mbedtls_pk_write_key_pem(pk, buf, sizeof buf);
    if (ret) return ret;
    return write_file(path, buf, strlen((char *)buf), 0600) ? -1 : 0;
}

static int load_key(mbedtls_pk_context *pk, const char *path)
{
    char *buf = malloc(4096);                  /* RSA keys (uploaded client keys) are up to ~3.3 KB */
    if (!buf) return -1;
    int n = read_file(path, buf, 4096);
    int ret = n <= 0 ? -1 : mbedtls_pk_parse_key(pk, (unsigned char *)buf, (size_t)n + 1, NULL, 0,
                                                 mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_platform_zeroize(buf, 4096);
    free(buf);
    return ret;
}

static int load_crt(mbedtls_x509_crt *crt, const char *path)
{
    char *buf = malloc(32768);                 /* CA bundles */
    if (!buf) return -1;
    int n = read_file(path, buf, 32768);
    int ret = n <= 0 ? -1 : mbedtls_x509_crt_parse(crt, (unsigned char *)buf, (size_t)n + 1);
    free(buf);
    return ret;
}

static int g_issue_client;      /* make_cert: clientAuth instead of serverAuth */
/* Write a certificate (subject key `sub`, signed by `iss`), PEM into out. */
/* NameConstraints for the camera CA (critical): it can only vouch for private
 * IPv4 addresses, local DNS suffixes and the names configured when it was
 * created. Users install this CA in their browsers; if the camera were ever
 * compromised, the key could not be used to impersonate other sites. */
static size_t der_len(unsigned char *p, size_t n)        /* writes a DER length, returns bytes */
{
    if (n < 128) { p[0] = (unsigned char)n; return 1; }
    if (n < 256) { p[0] = 0x81; p[1] = (unsigned char)n; return 2; }
    p[0] = 0x82; p[1] = (unsigned char)(n >> 8); p[2] = (unsigned char)n; return 3;
}
static size_t der_wrap(unsigned char tag, const unsigned char *in, size_t n, unsigned char *out)
{
    size_t h = 1;
    out[0] = tag;
    h += der_len(out + 1, n);
    memmove(out + h, in, n);
    return h + n;
}
static int ca_name_constraints(unsigned char *out, size_t cap, size_t *outlen)
{
    unsigned char subs[1400], tmp[300], one[300];
    size_t sl = 0;
    static const unsigned char nets[][8] = {
        { 10, 0, 0, 0, 255, 0, 0, 0 }, { 172, 16, 0, 0, 255, 240, 0, 0 },
        { 192, 168, 0, 0, 255, 255, 0, 0 }, { 169, 254, 0, 0, 255, 255, 0, 0 },
    };
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        size_t n = der_wrap(0x87, nets[i], 8, tmp);              /* [7] iPAddress */
        n = der_wrap(0x30, tmp, n, one);                         /* GeneralSubtree */
        if (sl + n > sizeof subs) return -1;
        memcpy(subs + sl, one, n); sl += n;
    }
    const char *dns[TLS_NAMES_MAX + 5] = { "local", "lan", "home.arpa", "internal", "localdomain" };
    int nd = 5;
    for (int k = 0; k < TLS_NAMES_MAX; k++) if (g_names[k][0]) dns[nd++] = g_names[k];
    for (int i = 0; i < nd; i++) {
        size_t l = strlen(dns[i]);
        if (l > 250) continue;
        size_t n = der_wrap(0x82, (const unsigned char *)dns[i], l, tmp);   /* [2] dNSName */
        n = der_wrap(0x30, tmp, n, one);
        if (sl + n > sizeof subs) return -1;
        memcpy(subs + sl, one, n); sl += n;
    }
    unsigned char perm[1500];
    size_t pl = der_wrap(0xA0, subs, sl, perm);                  /* permittedSubtrees [0] */
    if (pl + 4 > cap) return -1;
    *outlen = der_wrap(0x30, perm, pl, out);
    return 0;
}

static int make_cert(mbedtls_pk_context *sub, mbedtls_pk_context *iss, const char *subject,
                     const char *issuer, int ca, time_t nb, time_t na,
                     mbedtls_x509_san_list *san, unsigned char *out, size_t cap)
{
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    unsigned char serial[16];
    char tb[15], ta[15];
    int ret = mbedtls_ctr_drbg_random(&g_drbg, serial, sizeof serial);
    serial[0] = (unsigned char)((serial[0] & 0x7f) | 0x40);        /* positive, 16 bytes */
    x509_time(nb, tb);
    x509_time(na, ta);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, sub);
    mbedtls_x509write_crt_set_issuer_key(&w, iss);
    if (!ret) ret = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial);
    if (!ret) ret = mbedtls_x509write_crt_set_subject_name(&w, subject);
    if (!ret) ret = mbedtls_x509write_crt_set_issuer_name(&w, issuer);
    if (!ret) ret = mbedtls_x509write_crt_set_validity(&w, tb, ta);
    if (!ret) ret = mbedtls_x509write_crt_set_basic_constraints(&w, ca, ca ? 0 : -1);
    if (!ret) ret = mbedtls_x509write_crt_set_subject_key_identifier(&w);
    if (!ret) ret = mbedtls_x509write_crt_set_authority_key_identifier(&w);
    if (!ret) ret = mbedtls_x509write_crt_set_key_usage(&w, ca ?
                        (MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN) :
                        MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (!ret && !ca) {
        mbedtls_asn1_sequence eku;
        memset(&eku, 0, sizeof eku);
        eku.buf.tag = MBEDTLS_ASN1_OID;
        eku.buf.p = (unsigned char *)(g_issue_client ? MBEDTLS_OID_CLIENT_AUTH : MBEDTLS_OID_SERVER_AUTH);
        eku.buf.len = g_issue_client ? MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH) : MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
        ret = mbedtls_x509write_crt_set_ext_key_usage(&w, &eku);
    }
    if (!ret && san) ret = mbedtls_x509write_crt_set_subject_alternative_name(&w, san);
    if (!ret && ca) {
        unsigned char nc[1600];
        size_t ncl = 0;
        if (ca_name_constraints(nc, sizeof nc, &ncl) == 0)
            ret = mbedtls_x509write_crt_set_extension(&w, MBEDTLS_OID_NAME_CONSTRAINTS,
                                                      MBEDTLS_OID_SIZE(MBEDTLS_OID_NAME_CONSTRAINTS), 1, nc, ncl);
    }
    if (!ret) ret = mbedtls_x509write_crt_pem(&w, out, cap, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_x509write_crt_free(&w);
    return ret;
}

static int ca_fingerprint(char out[65])
{
    mbedtls_x509_crt ca;
    mbedtls_x509_crt_init(&ca);
    int ret = load_crt(&ca, F_CA_CRT);
    if (!ret) {
        unsigned char h[32];
        mbedtls_sha256(ca.raw.p, ca.raw.len, h, 0);
        for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
    }
    mbedtls_x509_crt_free(&ca);
    return ret;
}

static int ensure_ca(int force)
{
    if (!force && access(F_CA_KEY, F_OK) == 0 && access(F_CA_CRT, F_OK) == 0) {
        mbedtls_x509_crt c; mbedtls_pk_context k;
        mbedtls_x509_crt_init(&c); mbedtls_pk_init(&k);
        int ok = load_crt(&c, F_CA_CRT) == 0 && load_key(&k, F_CA_KEY) == 0 &&
                 mbedtls_pk_check_pair(&c.pk, &k, mbedtls_ctr_drbg_random, &g_drbg) == 0;
        mbedtls_x509_crt_free(&c); mbedtls_pk_free(&k);
        if (ok) return 0;
        fprintf(stderr, "[tls] CA files unusable, creating a new CA\n");
    }
    mbedtls_pk_context k;
    mbedtls_pk_init(&k);
    unsigned char id[3], pem[4096];
    char subj[96];
    int ret = gen_key(&k);
    if (!ret) ret = mbedtls_ctr_drbg_random(&g_drbg, id, sizeof id);
    snprintf(subj, sizeof subj, "CN=LSC camera CA %02X%02X%02X,O=ak_rtsp", id[0], id[1], id[2]);
    time_t nb = now_or_build(NULL) - DAY;
    if (!ret) ret = make_cert(&k, &k, subj, subj, 1, nb, nb + (time_t)CA_YEARS * 365 * DAY + 5 * DAY, NULL, pem, sizeof pem);
    if (!ret) ret = save_key(&k, F_CA_KEY);
    if (!ret) ret = write_file(F_CA_CRT, pem, strlen((char *)pem), 0644) ? -1 : 0;
    mbedtls_pk_free(&k);
    if (ret) { set_err("creating the CA failed", ret); return -1; }
    unlink(F_STAMP);                            /* server cert must be re-issued */
    printf("[tls] created %s\n", subj);
    return 0;
}

/* The stamp describes what the server certificate covers; any difference
 * (addresses, names, CA) means it must be re-issued. */
static void make_stamp(const char *ips, const char *names, const char *ca_fp, char *out, size_t cap)
{
    snprintf(out, cap, "ip=%s\ndns=%s\nca=%.64s\n", ips, names, ca_fp);
}

static int install_pair(void)
{
    pair_t *p = calloc(1, sizeof *p);
    if (!p) return -1;
    mbedtls_x509_crt_init(&p->crt);
    mbedtls_pk_init(&p->key);
    int ret = load_crt(&p->crt, F_CRT);
    if (!ret) ret = load_key(&p->key, F_KEY);
    if (!ret) ret = mbedtls_pk_check_pair(&p->crt.pk, &p->key, mbedtls_ctr_drbg_random, &g_drbg);
    if (ret) {
        mbedtls_x509_crt_free(&p->crt); mbedtls_pk_free(&p->key); free(p);
        return ret;
    }
    const mbedtls_x509_time *t = &p->crt.valid_to;
    snprintf(g_not_after, sizeof g_not_after, "%04d-%02d-%02d", t->year, t->mon, t->day);
    pthread_mutex_lock(&g_cur_lock);
    p->prev = g_cur;
    g_cur = p;
    pthread_mutex_unlock(&g_cur_lock);
    return 0;
}

static int cert_expiring(const mbedtls_x509_crt *c)
{
    int trusted;
    time_t now = now_or_build(&trusted);
    if (!trusted) return 0;                     /* can't tell without a real clock */
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = c->valid_to.year - 1900; tm.tm_mon = c->valid_to.mon - 1; tm.tm_mday = c->valid_to.day;
    tm.tm_hour = c->valid_to.hour; tm.tm_min = c->valid_to.min; tm.tm_sec = c->valid_to.sec;
    return timegm(&tm) - now < RENEW_DAYS * DAY;
}

/* Issue (or keep) the server certificate. Caller holds g_lock. */
static int ensure_server_cert(int force)
{
    char ips[160], names[1100], fp[65] = "", want[1400], have[1400] = "";
    current_ips(ips, sizeof ips);
    names_csv(g_names, g_nnames, names, sizeof names);
    if (ca_fingerprint(fp) != 0) { set_err("CA certificate missing", 0); return -1; }
    make_stamp(ips, names, fp, want, sizeof want);
    read_file(F_STAMP, have, sizeof have);

    int need = force || strcmp(want, have) != 0 || !g_cur;
    if (!need) {
        need = cert_expiring(&g_cur->crt);
        if (need) printf("[tls] server certificate expires soon, renewing\n");
    }
    if (!need) return 0;
    if (strcmp(want, have) == 0 && !force && g_cur == NULL) {
        /* files match the stamp: just load them (normal boot) */
        if (install_pair() == 0 && !cert_expiring(&g_cur->crt)) {
            snprintf(g_ips, sizeof g_ips, "%s", ips);
            return 0;
        }
    }
    if (!ips[0] && !g_nnames) { set_err("no IP address yet", 0); return -1; }

    /* server key: kept across renewals; created once */
    mbedtls_pk_context key, cakey;
    mbedtls_x509_crt ca;
    mbedtls_pk_init(&key); mbedtls_pk_init(&cakey); mbedtls_x509_crt_init(&ca);
    int ret = 0;
    if (load_key(&key, F_KEY) != 0) {
        mbedtls_pk_free(&key); mbedtls_pk_init(&key);
        ret = gen_key(&key);
        if (!ret) ret = save_key(&key, F_KEY);
    }
    if (!ret) ret = load_key(&cakey, F_CA_KEY);
    if (!ret) ret = load_crt(&ca, F_CA_CRT);

    /* SubjectAltName: every name and address */
    mbedtls_x509_san_list san[TLS_NAMES_MAX + 8];
    unsigned char ipb[8][4];
    int ns = 0;
    memset(san, 0, sizeof san);
    for (int k = 0; k < g_nnames; k++) {
        san[ns].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san[ns].node.san.unstructured_name.p = (unsigned char *)g_names[k];
        san[ns].node.san.unstructured_name.len = strlen(g_names[k]);
        ns++;
    }
    char ipcopy[160], *save = NULL;
    snprintf(ipcopy, sizeof ipcopy, "%s", ips);
    int ni = 0;
    for (char *t = strtok_r(ipcopy, ",", &save); t && ni < 8; t = strtok_r(NULL, ",", &save)) {
        struct in_addr a;
        if (inet_pton(AF_INET, t, &a) != 1) continue;
        memcpy(ipb[ni], &a, 4);
        san[ns].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        san[ns].node.san.unstructured_name.p = ipb[ni];
        san[ns].node.san.unstructured_name.len = 4;
        ns++; ni++;
    }
    for (int k = 0; k + 1 < ns; k++) san[k].next = &san[k + 1];

    char subj[300], issuer[256];
    unsigned char pem[2048];
    char first[16];                              /* CN: first name, else first IP */
    snprintf(first, sizeof first, "%.*s", (int)strcspn(ips, ","), ips);
    snprintf(subj, sizeof subj, "CN=%s,O=ak_rtsp", g_nnames ? g_names[0] : first[0] ? first : "camera");
    if (!ret) ret = mbedtls_x509_dn_gets(issuer, sizeof issuer, &ca.subject) < 0 ? -1 : 0;
    time_t nb = now_or_build(NULL) - DAY;
    if (!ret) ret = make_cert(&key, &cakey, subj, issuer, 0, nb, nb + (time_t)SRV_DAYS * DAY,
                              ns ? san : NULL, pem, sizeof pem);
    if (!ret) ret = write_file(F_CRT, pem, strlen((char *)pem), 0644) ? -1 : 0;
    mbedtls_pk_free(&key); mbedtls_pk_free(&cakey); mbedtls_x509_crt_free(&ca);
    if (ret) { set_err("issuing the server certificate failed", ret); return -1; }
    if ((ret = install_pair()) != 0) { set_err("loading the new certificate failed", ret); return -1; }
    write_file(F_STAMP, want, strlen(want), 0644);
    snprintf(g_ips, sizeof g_ips, "%s", ips);
    g_err[0] = 0;
    printf("[tls] server certificate issued for %s%s%s (until %s)\n",
           names, names[0] && ips[0] ? "," : "", ips, g_not_after);
    return 0;
}

/* ---- TLS plumbing ----------------------------------------------------------------- */
static int cert_cb(mbedtls_ssl_context *ssl)
{
    pthread_mutex_lock(&g_cur_lock);
    pair_t *p = g_cur;
    pthread_mutex_unlock(&g_cur_lock);
    if (!p) return MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE;
    return mbedtls_ssl_set_hs_own_cert(ssl, &p->crt, &p->key);
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    conn_t *c = ctx;
    for (;;) {
        ssize_t n = send(c->fd, buf, len, MSG_NOSIGNAL);
        if (n >= 0) return (int)n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;   /* SO_SNDTIMEO */
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    conn_t *c = ctx;
    for (;;) {
        ssize_t n = recv(c->fd, buf, len, c->nonblock ? MSG_DONTWAIT : 0);
        if (n >= 0) return (int)n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return c->nonblock ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_TIMEOUT;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
}

int tls_init(void)
{
    static int conf_done;
    int ret = seed_once();
    if (ret) { set_err("random generator", 0); return -1; }

    pthread_mutex_lock(&g_lock);
    load_conf();
    if (ensure_ca(0) != 0 || ensure_server_cert(0) != 0) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_lock);

    if (conf_done) { g_ready = 1; return 0; }
    mbedtls_ssl_config_init(&g_conf);
    ret = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret) { set_err("TLS setup", ret); return -1; }
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_min_tls_version(&g_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&g_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_cert_cb(&g_conf, cert_cb);
    mbedtls_ssl_conf_own_cert(&g_conf, &g_cur->crt, &g_cur->key);   /* fallback */

    mbedtls_ssl_cache_init(&g_cache);
    mbedtls_ssl_cache_set_max_entries(&g_cache, 16);
    mbedtls_ssl_cache_set_timeout(&g_cache, 86400);
    mbedtls_ssl_conf_session_cache(&g_conf, &g_cache, mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
    mbedtls_ssl_ticket_init(&g_ticket);
    ret = mbedtls_ssl_ticket_setup(&g_ticket, mbedtls_ctr_drbg_random, &g_drbg,
                                   MBEDTLS_CIPHER_AES_256_GCM, 86400);
    if (!ret)
        mbedtls_ssl_conf_session_tickets_cb(&g_conf, mbedtls_ssl_ticket_write,
                                            mbedtls_ssl_ticket_parse, &g_ticket);
    conf_done = 1;
    g_ready = 1;
    return 0;
}

void tls_maintain(void)
{
    if (!g_ready) return;
    pthread_mutex_lock(&g_lock);
    ensure_server_cert(0);
    pthread_mutex_unlock(&g_lock);
}

conn_t *tls_accept(int fd, int handshake_ms)
{
    static int logged;
    if (!g_ready) { close(fd); return NULL; }
    conn_t *c = calloc(1, sizeof *c);
    mbedtls_ssl_context *ssl = calloc(1, sizeof *ssl);
    if (!c || !ssl) { free(c); free(ssl); close(fd); return NULL; }
    c->fd = fd;
    mbedtls_ssl_init(ssl);
    struct timeval tv = { handshake_ms / 1000, (handshake_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int ret = mbedtls_ssl_setup(ssl, &g_conf);
    if (!ret) {
        mbedtls_ssl_set_bio(ssl, c, bio_send, bio_recv, NULL);
        while ((ret = mbedtls_ssl_handshake(ssl)) == MBEDTLS_ERR_SSL_WANT_READ ||
               ret == MBEDTLS_ERR_SSL_WANT_WRITE) {}
    }
    if (ret) {
        /* Browsers that don't trust the CA yet abort here: normal, log a few. */
        if (logged < 5) {
            char e[80];
            mbedtls_strerror(ret, e, sizeof e);
            fprintf(stderr, "[tls] handshake failed: %s\n", e);
            logged++;
        }
        mbedtls_ssl_free(ssl); free(ssl); free(c); close(fd);
        return NULL;
    }
    c->ssl = ssl;
    return c;
}

int tls_set_config(const char *names, int redirect, const char **err)
{
    char tmp[TLS_NAMES_MAX][254];
    int n = 0;
    if (names && parse_names(names, tmp, &n) != 0) {
        *err = "names must be up to 4 valid DNS names (letters, digits, '-' and '.')";
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    if (names) { memcpy(g_names, tmp, sizeof tmp); g_nnames = n; }
    if (redirect >= 0) g_redirect = redirect;
    char csv[1100], buf[1300];
    names_csv(g_names, g_nnames, csv, sizeof csv);
    snprintf(buf, sizeof buf, "names=%s\nredirect=%d\n", csv, g_redirect);
    int ret = write_file(F_CONF, buf, strlen(buf), 0644);
    if (!ret && g_ready) ret = ensure_server_cert(0);
    pthread_mutex_unlock(&g_lock);
    if (ret) { *err = g_err[0] ? g_err : "cannot save"; return -1; }
    return 0;
}

int tls_regenerate_ca(const char **err)
{
    pthread_mutex_lock(&g_lock);
    int ret = ensure_ca(1);
    if (!ret) ret = ensure_server_cert(1);
    pthread_mutex_unlock(&g_lock);
    if (ret) { *err = g_err[0] ? g_err : "failed"; return -1; }
    return 0;
}

int tls_ca_pem(char *out, size_t cap)
{
    return read_file(F_CA_CRT, out, cap);
}

int tls_server_pem(char *out, size_t cap)
{
    return read_file(F_CRT, out, cap);
}

/* SHA-256 of the certificate (hex) and of its public key (SubjectPublicKeyInfo,
 * base64: the "pin-sha256" used by curl --pinnedpubkey, HPKP-style pins). */
static void cert_pins(const char *path, char fp[65], char spki[45])
{
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    fp[0] = spki[0] = 0;
    if (load_crt(&c, path) == 0) {
        unsigned char h[32], der[200];
        mbedtls_sha256(c.raw.p, c.raw.len, h, 0);
        for (int i = 0; i < 32; i++) sprintf(fp + 2 * i, "%02x", h[i]);
        int n = mbedtls_pk_write_pubkey_der(&c.pk, der, sizeof der);   /* written at the end */
        if (n > 0) {
            size_t ol = 0;
            mbedtls_sha256(der + sizeof der - n, (size_t)n, h, 0);
            if (mbedtls_base64_encode((unsigned char *)spki, 45, &ol, h, 32) != 0) spki[0] = 0;
        }
    }
    mbedtls_x509_crt_free(&c);
}

int tls_info_json(char *out, size_t cap)
{
    char fp[65] = "", names[1100] = "", subj[160] = "", ca_spki[45], srv_fp[65], srv_spki[45];
    pthread_mutex_lock(&g_lock);
    cert_pins(F_CA_CRT, fp, ca_spki);
    cert_pins(F_CRT, srv_fp, srv_spki);
    names_csv(g_names, g_nnames, names, sizeof names);
    {
        mbedtls_x509_crt ca;
        mbedtls_x509_crt_init(&ca);
        if (load_crt(&ca, F_CA_CRT) == 0) mbedtls_x509_dn_gets(subj, sizeof subj, &ca.subject);
        mbedtls_x509_crt_free(&ca);
    }
    /* names/IPs/DNs are validated or generated by us: no JSON escaping needed
     * beyond the absence of quotes and backslashes, which they can't contain */
    int n = snprintf(out, cap,
        "{\"ready\":%s,\"port\":443,\"redirect\":%s,\"names\":\"%s\",\"ips\":\"%s\","
        "\"not_after\":\"%s\",\"ca_name\":\"%s\",\"ca_sha256\":\"%s\",\"ca_spki\":\"%s\","
        "\"srv_sha256\":\"%s\",\"srv_spki\":\"%s\",\"rtsps_port\":322,\"error\":\"%s\"}",
        g_ready ? "true" : "false", g_redirect ? "true" : "false", names, g_ips,
        g_not_after, subj, fp, ca_spki, srv_fp, srv_spki, g_err);
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* ---- connections ------------------------------------------------------------------ */
conn_t *conn_plain(int fd)
{
    conn_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = fd;
    return c;
}

int conn_send_all(conn_t *c, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n) {
        if (c->ssl) {
            int w = mbedtls_ssl_write(c->ssl, b, n);
            if (w == MBEDTLS_ERR_SSL_WANT_WRITE || w == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (w <= 0) return -1;
            b += w; n -= (size_t)w;
        } else {
            ssize_t w = send(c->fd, b, n, MSG_NOSIGNAL);
            if (w < 0) { if (errno == EINTR) continue; return -1; }
            b += w; n -= (size_t)w;
        }
    }
    return 0;
}

static ssize_t tls_read(conn_t *c, void *p, size_t n)
{
    for (;;) {
        int r = mbedtls_ssl_read(c->ssl, p, n);
        if (r > 0) return r;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            return c->nonblock ? -2 : -1;
        return -1;
    }
}

ssize_t conn_recv(conn_t *c, void *p, size_t n)
{
    if (c->ssl) { c->nonblock = 0; return tls_read(c, p, n); }
    for (;;) {
        ssize_t r = recv(c->fd, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r < 0 ? -1 : r;
    }
}

ssize_t conn_recv_nb(conn_t *c, void *p, size_t n)
{
    if (c->ssl) {
        c->nonblock = 1;
        ssize_t r = tls_read(c, p, n);
        c->nonblock = 0;
        return r;
    }
    ssize_t r = recv(c->fd, p, n, MSG_DONTWAIT);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -2 : -1;
    return r;
}

int conn_readable(conn_t *c, int timeout_ms)
{
    if (c->ssl && mbedtls_ssl_get_bytes_avail(c->ssl) > 0) return 1;
    struct pollfd p = { c->fd, POLLIN, 0 };
    return poll(&p, 1, timeout_ms) > 0;
}

void conn_shutdown_wr(conn_t *c)
{
    if (c->ssl) mbedtls_ssl_close_notify(c->ssl);
    shutdown(c->fd, SHUT_WR);
}

void conn_close(conn_t *c)
{
    if (!c) return;
    if (c->ssl) {
        struct timeval tv = { 1, 0 };
        setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        mbedtls_ssl_close_notify(c->ssl);
        mbedtls_ssl_free(c->ssl);
        free(c->ssl);
    }
    if (c->free_extra) c->free_extra(c->extra);
    close(c->fd);
    free(c);
}

/* =========================================================================
 * MQTT over TLS: client side
 * ========================================================================= */
#define F_MQ_CA       TLS_DIR "/ak_rtsp_mqtt_ca.crt"       /* broker CA (uploaded) */
#define F_MQ_CAM_CRT  TLS_DIR "/ak_rtsp_mqtt_cam.crt"      /* issued by the camera CA */
#define F_MQ_CAM_KEY  TLS_DIR "/ak_rtsp_mqtt_cam.key"
#define F_MQ_CAM_FOR  TLS_DIR "/ak_rtsp_mqtt_cam.ca"       /* CA fingerprint it was issued under */
#define F_MQ_CUS_CRT  TLS_DIR "/ak_rtsp_mqtt_client.crt"   /* uploaded / signed from our CSR */
#define F_MQ_CUS_KEY  TLS_DIR "/ak_rtsp_mqtt_client.key"   /* uploaded or generated for the CSR */
#define F_MQ_PIN      TLS_DIR "/ak_rtsp_mqtt_pin"          /* trusted broker key (SPKI SHA-256, base64) */
#define F_MQ_FETCHED  "/tmp/ak_rtsp_mqtt_fetched.pem"      /* chain shown by the broker (RAM) */

/* SHA-256 of a certificate's public key (SPKI), base64 */
static void spki_b64(const mbedtls_x509_crt *c, char out[45])
{
    unsigned char der[600], h[32];
    out[0] = 0;
    int n = mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)&c->pk, der, sizeof der);
    if (n <= 0) return;
    mbedtls_sha256(der + sizeof der - n, (size_t)n, h, 0);
    size_t ol = 0;
    if (mbedtls_base64_encode((unsigned char *)out, 45, &ol, h, 32) != 0) out[0] = 0;
}

static int seed_once(void)
{
    static int seeded;
    if (seeded) return 0;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    const char *pers = "ak_rtsp tls";
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char *)pers, strlen(pers)))
        return -1;
    seeded = 1;
    return 0;
}

/* Client certificate issued by the camera CA (CN = name, clientAuth). */
static int ensure_camera_client_cert(const char *cn)
{
    char fp[65] = "", have[80] = "";
    if (ca_fingerprint(fp) != 0) return -1;
    read_file(F_MQ_CAM_FOR, have, sizeof have);
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    int ok = !strcmp(have, fp) && load_crt(&c, F_MQ_CAM_CRT) == 0 && access(F_MQ_CAM_KEY, F_OK) == 0 && !cert_expiring(&c);
    mbedtls_x509_crt_free(&c);
    if (ok) return 0;
    mbedtls_pk_context key, cakey;
    mbedtls_x509_crt ca;
    mbedtls_pk_init(&key); mbedtls_pk_init(&cakey); mbedtls_x509_crt_init(&ca);
    int ret = 0;
    if (load_key(&key, F_MQ_CAM_KEY) != 0) {
        mbedtls_pk_free(&key); mbedtls_pk_init(&key);
        ret = gen_key(&key);
        if (!ret) ret = save_key(&key, F_MQ_CAM_KEY);
    }
    if (!ret) ret = load_key(&cakey, F_CA_KEY);
    if (!ret) ret = load_crt(&ca, F_CA_CRT);
    char issuer[256], subj[96];
    unsigned char pem[2048];
    if (!ret) ret = mbedtls_x509_dn_gets(issuer, sizeof issuer, &ca.subject) < 0 ? -1 : 0;
    snprintf(subj, sizeof subj, "CN=%s,O=ak_rtsp", cn);
    time_t nb = now_or_build(NULL) - DAY;
    g_issue_client = 1;
    if (!ret) ret = make_cert(&key, &cakey, subj, issuer, 0, nb, nb + (time_t)SRV_DAYS * DAY, NULL, pem, sizeof pem);
    g_issue_client = 0;
    if (!ret) ret = write_file(F_MQ_CAM_CRT, pem, strlen((char *)pem), 0644) ? -1 : 0;
    if (!ret) write_file(F_MQ_CAM_FOR, fp, strlen(fp), 0644);
    mbedtls_pk_free(&key); mbedtls_pk_free(&cakey); mbedtls_x509_crt_free(&ca);
    if (!ret) printf("[tls] MQTT client certificate issued: %s\n", subj);
    return ret ? -1 : 0;
}

typedef struct {
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca, crt;
    mbedtls_pk_context key;
    char leaf_pin[45];          /* broker key seen during the handshake */
    FILE *chain;                /* fetch mode: PEM chain written here */
} cli_t;

/* Called for every certificate of the broker's chain (deepest first). We
 * don't change the verdict; we only remember the leaf key and, when
 * fetching, write the chain out. */
static int cli_verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    cli_t *k = ctx;
    (void)flags;
    if (depth == 0) spki_b64(crt, k->leaf_pin);
    if (k->chain) {
        unsigned char pem[4096];
        size_t ol = 0;
        if (mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n",
                                     crt->raw.p, crt->raw.len, pem, sizeof pem, &ol) == 0)
            fwrite(pem, 1, strlen((char *)pem), k->chain);
    }
    return 0;
}

static void cli_free(void *p)
{
    cli_t *k = p;
    if (!k) return;
    mbedtls_ssl_config_free(&k->conf);
    mbedtls_x509_crt_free(&k->ca);
    mbedtls_x509_crt_free(&k->crt);
    mbedtls_pk_free(&k->key);
    free(k);
}

conn_t *tls_client_connect(int fd, const char *host, int verify, int client_cert, const char *cn,
                           int timeout_ms, char *err, size_t errcap)
{
    char e[100];
    err[0] = 0;
    pthread_mutex_lock(&g_lock);
    int sret = seed_once();
    if (!sret && client_cert == 1 && ensure_camera_client_cert(cn) != 0) {
        pthread_mutex_unlock(&g_lock);
        snprintf(err, errcap, "cannot issue the camera client certificate");
        close(fd);
        return NULL;
    }
    pthread_mutex_unlock(&g_lock);
    cli_t *k = calloc(1, sizeof *k);
    conn_t *c = calloc(1, sizeof *c);
    mbedtls_ssl_context *ssl = calloc(1, sizeof *ssl);
    if (sret || !k || !c || !ssl) { free(k); free(c); free(ssl); close(fd); snprintf(err, errcap, "out of memory"); return NULL; }
    mbedtls_ssl_config_init(&k->conf);
    mbedtls_x509_crt_init(&k->ca); mbedtls_x509_crt_init(&k->crt); mbedtls_pk_init(&k->key);
    mbedtls_ssl_init(ssl);
    c->fd = fd; c->extra = k; c->free_extra = cli_free;
    int ret = mbedtls_ssl_config_defaults(&k->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&k->conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_min_tls_version(&k->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&k->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    char pin[64] = "";
    mbedtls_ssl_conf_verify(&k->conf, cli_verify_cb, k);
    if (!ret && verify == 1) {
        if (load_crt(&k->ca, F_MQ_CA) != 0) { snprintf(err, errcap, "no broker CA certificate (upload or fetch it, or pin the broker key)"); goto fail; }
        mbedtls_ssl_conf_ca_chain(&k->conf, &k->ca, NULL);
        mbedtls_ssl_conf_authmode(&k->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else if (!ret && verify == 2) {
        if (read_file(F_MQ_PIN, pin, sizeof pin) <= 0) { snprintf(err, errcap, "no pinned broker key (fetch it from the broker first)"); goto fail; }
        pin[strcspn(pin, "\r\n")] = 0;
        mbedtls_ssl_conf_authmode(&k->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);   /* chain not checked; key compared below */
    } else {
        mbedtls_ssl_conf_authmode(&k->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    }
    if (!ret && client_cert) {
        const char *cf = client_cert == 1 ? F_MQ_CAM_CRT : F_MQ_CUS_CRT, *kf = client_cert == 1 ? F_MQ_CAM_KEY : F_MQ_CUS_KEY;
        if (load_crt(&k->crt, cf) != 0 || load_key(&k->key, kf) != 0) { snprintf(err, errcap, "client certificate or key missing"); goto fail; }
        if (client_cert == 1) load_crt(&k->crt, F_CA_CRT);      /* send the chain: client cert + camera CA */
        ret = mbedtls_ssl_conf_own_cert(&k->conf, &k->crt, &k->key);
    }
    if (!ret) ret = mbedtls_ssl_setup(ssl, &k->conf);
    if (!ret) ret = mbedtls_ssl_set_hostname(ssl, host);
    if (ret) goto tlsfail;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    mbedtls_ssl_set_bio(ssl, c, bio_send, bio_recv, NULL);
    while ((ret = mbedtls_ssl_handshake(ssl)) == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {}
    if (ret) {
        uint32_t flags = mbedtls_ssl_get_verify_result(ssl);
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags) {
            if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) snprintf(err, errcap, "broker certificate is not valid for %s", host);
            else if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) snprintf(err, errcap, "broker certificate is not signed by the uploaded CA");
            else if (flags & MBEDTLS_X509_BADCERT_EXPIRED) snprintf(err, errcap, "broker certificate has expired");
            else if (flags & MBEDTLS_X509_BADCERT_FUTURE) snprintf(err, errcap, "broker certificate is not valid yet (camera clock?)");
            else {
                char vi[160];
                mbedtls_x509_crt_verify_info(vi, sizeof vi, "", flags);
                vi[strcspn(vi, "\n")] = 0;
                snprintf(err, errcap, "broker certificate rejected: %s", vi);
            }
            goto fail;
        }
        goto tlsfail;
    }
    if (verify == 2 && strcmp(k->leaf_pin, pin) != 0) {
        snprintf(err, errcap, "broker key changed (pinned %.12s..., got %.12s...)", pin, k->leaf_pin);
        goto fail;
    }
    c->ssl = ssl;
    return c;
tlsfail:
    mbedtls_strerror(ret, e, sizeof e);
    snprintf(err, errcap, "TLS: %s", e);
fail:
    mbedtls_ssl_free(ssl); free(ssl);
    cli_free(k);
    close(fd); free(c);
    return NULL;
}

/* ---- certificate management for the web page ---- */
static int pem_kind_ok(const char *pem, size_t n, int want_key)
{
    if (want_key) {
        mbedtls_pk_context k;
        mbedtls_pk_init(&k);
        int r = mbedtls_pk_parse_key(&k, (const unsigned char *)pem, n + 1, NULL, 0, mbedtls_ctr_drbg_random, &g_drbg);
        mbedtls_pk_free(&k);
        return r == 0;
    }
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    int r = mbedtls_x509_crt_parse(&c, (const unsigned char *)pem, n + 1);
    mbedtls_x509_crt_free(&c);
    return r == 0;
}

int tls_mqtt_upload(const char *what, const char *pem, size_t n, const char **err)
{
    char *buf = malloc(n + 1);
    if (!buf) { *err = "out of memory"; return -1; }
    memcpy(buf, pem, n); buf[n] = 0;
    pthread_mutex_lock(&g_lock);
    seed_once();
    int ret = -1;
    if (!strcmp(what, "ca")) {
        if (!pem_kind_ok(buf, n, 0)) *err = "not a PEM certificate";
        else ret = write_file(F_MQ_CA, buf, n, 0644);
    } else if (!strcmp(what, "cert")) {
        mbedtls_x509_crt c; mbedtls_pk_context k;
        mbedtls_x509_crt_init(&c); mbedtls_pk_init(&k);
        if (mbedtls_x509_crt_parse(&c, (const unsigned char *)buf, n + 1) != 0) *err = "not a PEM certificate";
        else if (load_key(&k, F_MQ_CUS_KEY) == 0 && mbedtls_pk_check_pair(&c.pk, &k, mbedtls_ctr_drbg_random, &g_drbg) != 0)
            *err = "certificate does not match the client key (upload the key, or generate a new CSR)";
        else ret = write_file(F_MQ_CUS_CRT, buf, n, 0644);
        mbedtls_x509_crt_free(&c); mbedtls_pk_free(&k);
    } else if (!strcmp(what, "key")) {
        if (!pem_kind_ok(buf, n, 1)) *err = "not an unencrypted PEM private key";
        else ret = write_file(F_MQ_CUS_KEY, buf, n, 0600);
    } else *err = "unknown item";
    pthread_mutex_unlock(&g_lock);
    memset(buf, 0, n); free(buf);
    if (ret && !*err) *err = "cannot save";
    return ret ? -1 : 0;
}

int tls_mqtt_clear(const char *what)
{
    if (!strcmp(what, "ca")) return unlink(F_MQ_CA);
    if (!strcmp(what, "pin")) return unlink(F_MQ_PIN);
    if (!strcmp(what, "client")) { unlink(F_MQ_CUS_CRT); return unlink(F_MQ_CUS_KEY); }
    return -1;
}

/* New key on the camera + CSR (CN = name). The old custom cert is removed. */
int tls_mqtt_csr(const char *cn, char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    seed_once();
    mbedtls_pk_context key;
    mbedtls_x509write_csr w;
    mbedtls_pk_init(&key);
    mbedtls_x509write_csr_init(&w);
    char subj[96];
    snprintf(subj, sizeof subj, "CN=%s,O=ak_rtsp", cn);
    int ret = gen_key(&key);
    if (!ret) ret = save_key(&key, F_MQ_CUS_KEY);
    if (!ret) { unlink(F_MQ_CUS_CRT); mbedtls_x509write_csr_set_md_alg(&w, MBEDTLS_MD_SHA256); mbedtls_x509write_csr_set_key(&w, &key); }
    if (!ret) ret = mbedtls_x509write_csr_set_subject_name(&w, subj);
    if (!ret) ret = mbedtls_x509write_csr_pem(&w, (unsigned char *)out, cap, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_x509write_csr_free(&w);
    mbedtls_pk_free(&key);
    pthread_mutex_unlock(&g_lock);
    return ret ? -1 : (int)strlen(out);
}

int tls_mqtt_camera_cert_pem(const char *cn, char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    int ok = seed_once() == 0 && ensure_camera_client_cert(cn) == 0;
    pthread_mutex_unlock(&g_lock);
    return ok ? read_file(F_MQ_CAM_CRT, out, cap) : -1;
}

static void crt_desc(const char *path, char *out, size_t cap)
{
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    out[0] = 0;
    if (load_crt(&c, path) == 0) {
        char subj[160];
        mbedtls_x509_dn_gets(subj, sizeof subj, &c.subject);
        for (char *p = subj; *p; p++) if (*p == '"' || *p == '\\') *p = '\'';
        snprintf(out, cap, "%s (until %04d-%02d-%02d)", subj, c.valid_to.year, c.valid_to.mon, c.valid_to.day);
    }
    mbedtls_x509_crt_free(&c);
}

int tls_mqtt_info_json(char *out, size_t cap)
{
    char ca[220], cus[220], cam[220];
    crt_desc(F_MQ_CA, ca, sizeof ca);
    crt_desc(F_MQ_CUS_CRT, cus, sizeof cus);
    crt_desc(F_MQ_CAM_CRT, cam, sizeof cam);
    char pin[64] = "";
    if (read_file(F_MQ_PIN, pin, sizeof pin) > 0) pin[strcspn(pin, "\r\n")] = 0;
    return snprintf(out, cap, "{\"ca\":\"%s\",\"pin\":\"%s\",\"custom_cert\":\"%s\",\"custom_key\":%s,\"camera_cert\":\"%s\"}",
                    ca, pin, cus, access(F_MQ_CUS_KEY, F_OK) == 0 ? "true" : "false", cam);
}

/* Connect to host:port without trusting anything, keep the chain the broker
 * presents (in RAM) and describe it: [{subject, issuer, ca, self_signed,
 * sha256, pin}], leaf first. */
int tls_mqtt_fetch(int fd, const char *host, char *out, size_t cap, char *err, size_t errcap)
{
    pthread_mutex_lock(&g_lock);
    seed_once();
    pthread_mutex_unlock(&g_lock);
    cli_t *k = calloc(1, sizeof *k);
    mbedtls_ssl_context ssl;
    if (!k) { close(fd); return -1; }
    mbedtls_ssl_config_init(&k->conf);
    mbedtls_ssl_init(&ssl);
    k->chain = fopen(F_MQ_FETCHED ".tmp", "w");
    conn_t c = { .fd = fd };
    int ret = mbedtls_ssl_config_defaults(&k->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&k->conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_min_tls_version(&k->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&k->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&k->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&k->conf, cli_verify_cb, k);
    if (!ret) ret = mbedtls_ssl_setup(&ssl, &k->conf);
    if (!ret) ret = mbedtls_ssl_set_hostname(&ssl, host);
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    mbedtls_ssl_set_bio(&ssl, &c, bio_send, bio_recv, NULL);
    if (!ret) while ((ret = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {}
    /* a broker that requires a client certificate may end the handshake
     * after sending its own chain; we have what we need either way */
    if (k->chain) fclose(k->chain);
    k->chain = NULL;
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&k->conf);
    free(k);
    close(fd);

    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    if (load_crt(&chain, F_MQ_FETCHED ".tmp") != 0) {
        char e[100];
        mbedtls_strerror(ret, e, sizeof e);
        snprintf(err, errcap, "no certificate received (%s)", ret ? e : "is this a TLS port?");
        unlink(F_MQ_FETCHED ".tmp");
        return -1;
    }
    rename(F_MQ_FETCHED ".tmp", F_MQ_FETCHED);
    /* callback order is deepest first; list leaf first */
    const mbedtls_x509_crt *v[8];
    int n = 0;
    for (const mbedtls_x509_crt *c2 = &chain; c2 && c2->raw.len && n < 8; c2 = c2->next) v[n++] = c2;
    size_t o = (size_t)snprintf(out, cap, "[");
    for (int i = n - 1; i >= 0 && o < cap; i--) {
        char subj[200], iss[200], pin[45], fp[65];
        unsigned char h[32];
        mbedtls_x509_dn_gets(subj, sizeof subj, &v[i]->subject);
        mbedtls_x509_dn_gets(iss, sizeof iss, &v[i]->issuer);
        for (char *p = subj; *p; p++) if (*p == '"' || *p == '\\') *p = '\'';
        for (char *p = iss; *p; p++) if (*p == '"' || *p == '\\') *p = '\'';
        spki_b64(v[i], pin);
        mbedtls_sha256(v[i]->raw.p, v[i]->raw.len, h, 0);
        for (int j = 0; j < 32; j++) sprintf(fp + 2 * j, "%02x", h[j]);
        int self = v[i]->subject_raw.len == v[i]->issuer_raw.len &&
                   !memcmp(v[i]->subject_raw.p, v[i]->issuer_raw.p, v[i]->issuer_raw.len);
        o += (size_t)snprintf(out + o, o < cap ? cap - o : 0,
            "%s{\"subject\":\"%s\",\"issuer\":\"%s\",\"ca\":%s,\"self_signed\":%s,\"sha256\":\"%s\",\"pin\":\"%s\",\"until\":\"%04d-%02d-%02d\"}",
            i == n - 1 ? "" : ",", subj, iss, v[i]->private_ca_istrue ? "true" : "false", self ? "true" : "false", fp, pin,
            v[i]->valid_to.year, v[i]->valid_to.mon, v[i]->valid_to.day);
    }
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]");
    mbedtls_x509_crt_free(&chain);
    return (int)o;
}

/* Trust what the last fetch returned: "ca" = the top certificate of the
 * chain (must be a CA), "pin" = the broker's own key. */
int tls_mqtt_trust(const char *what, const char *expect, const char **err)
{
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    if (load_crt(&chain, F_MQ_FETCHED) != 0) { *err = "fetch the broker certificate first"; return -1; }
    int ret = -1;
    if (!strcmp(what, "ca")) {
        const mbedtls_x509_crt *top = &chain;          /* deepest first: first entry = top of the chain */
        int self = top->subject_raw.len == top->issuer_raw.len && !memcmp(top->subject_raw.p, top->issuer_raw.p, top->issuer_raw.len);
        unsigned char hh[32]; char hx[65];
        mbedtls_sha256(top->raw.p, top->raw.len, hh, 0);
        for (int i = 0; i < 32; i++) snprintf(hx + 2 * i, 3, "%02x", hh[i]);
        if (!top->private_ca_istrue && !self) *err = "the broker did not send its CA certificate; pin its key instead";
        else if (!expect || strcasecmp(expect, hx) != 0) *err = "the certificate is not the one shown; fetch it again";
        else {
            unsigned char pem[4096];
            size_t ol = 0;
            if (mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n", "-----END CERTIFICATE-----\n",
                                         top->raw.p, top->raw.len, pem, sizeof pem, &ol) == 0)
                ret = write_file(F_MQ_CA, pem, strlen((char *)pem), 0644);
            if (ret) *err = "cannot save";
        }
    } else if (!strcmp(what, "pin")) {
        const mbedtls_x509_crt *leaf = &chain;
        while (leaf->next && leaf->next->raw.len) leaf = leaf->next;   /* last = depth 0 */
        char pin[45];
        spki_b64(leaf, pin);
        if (!expect || strcmp(expect, pin) != 0) *err = "the key is not the one shown; fetch it again";
        else {
            ret = pin[0] ? write_file(F_MQ_PIN, pin, strlen(pin), 0644) : -1;
            if (ret) *err = "cannot save";
        }
    } else *err = "unknown";
    mbedtls_x509_crt_free(&chain);
    return ret ? -1 : 0;
}

/* 1 if host is one of the configured certificate DNS names */
int tls_name_ok(const char *host)
{
    for (int k = 0; k < TLS_NAMES_MAX; k++)
        if (g_names[k][0] && !strcasecmp(g_names[k], host)) return 1;
    return 0;
}
