/* libFuzzer harness: auth.c Digest verification (web, ONVIF: strict with
 * req_uri; RTSP: req_uri NULL), nonce checking, nc replay window, the
 * Authorization parameter parser and the login backoff table.
 *
 * Input: byte 0 = mode, then fields separated by NUL bytes.
 *   mode & 3 == 0  raw:   authz, method, uri (uri unused when mode & 4: RTSP style)
 *   mode & 3 != 0  built: user, method, uri, qop, nc, cnonce, extra-params
 *                  The harness builds an Authorization header around these
 *                  fields with a REAL nonce (age picked by mode bits 3-5) and
 *                  the CORRECT response for whatever the parser will extract,
 *                  so the fuzzer gets past the MAC/hash checks and reaches the
 *                  replay window, stale handling and user comparison.
 *                  mode & 4: RTSP style (req_uri NULL); mode & 0x40: values
 *                  unquoted; mode & 0x80: request sent twice (replay).
 * The per-boot secret is fixed and all tables are reset per input
 * (deterministic). */
#include "auth.c"

static const uint32_t k_ages[8] = { 0, 1, 299, 301, 3600, 43199, 43201, 0x80000000u };

static size_t field(const uint8_t **p, const uint8_t *end, char *out, size_t cap)
{
    size_t n = 0;
    while (*p < end && **p) { if (n + 1 < cap) out[n++] = (char)**p; (*p)++; }
    if (*p < end) (*p)++;               /* the NUL */
    out[n] = 0;
    return n;
}

static void put_param(char *h, size_t cap, const char *key, const char *v, int unquoted)
{
    size_t o = strlen(h);
    if (o + 4 >= cap) return;
    o += (size_t)snprintf(h + o, cap - o, "%s%s=", o > 7 ? ", " : "", key);
    if (o >= cap) return;
    if (!unquoted) h[o++] = '"';
    for (const char *s = v; *s && o + 3 < cap; s++) {
        if (!unquoted && (*s == '"' || *s == '\\')) h[o++] = '\\';
        h[o++] = *s;
    }
    if (!unquoted && o + 1 < cap) h[o++] = '"';
    h[o < cap ? o : cap - 1] = 0;
}

static int g_init;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!g_init) {
        g_init = 1;
        auth_init();
        memset(g_secret, 0x5a, sizeof g_secret);
        pthread_mutex_lock(&g_lock);
        load_defaults_locked();             /* admin/admin whatever is on disk */
        pthread_mutex_unlock(&g_lock);
    }
    memset(g_nc, 0, sizeof g_nc);
    memset(g_fail, 0, sizeof g_fail);
    if (size < 1) return 0;
    uint8_t mode = data[0];
    const uint8_t *p = data + 1, *end = data + size;

    if ((mode & 3) == 0) {
        static char authz[4096];
        char method[64], uri[1024];
        field(&p, end, authz, sizeof authz);
        field(&p, end, method, sizeof method);
        field(&p, end, uri, sizeof uri);
        auth_digest_verify_ex(authz, method, (mode & 4) ? NULL : uri);
        auth_digest_verify(authz, method);
        /* the same header text through the backoff table */
        struct sockaddr_in sa = { .sin_family = AF_INET };
        memcpy(&sa.sin_addr, authz, 4);
        for (int i = 0; i < (mode >> 3 & 7) + 1; i++) auth_record((struct sockaddr *)&sa, 0);
        auth_throttled((struct sockaddr *)&sa);
        auth_record((struct sockaddr *)&sa, mode & 0x80 ? 1 : 0);
        char user[48];
        snprintf(user, sizeof user, "%s", method);
        auth_check_plain(user, uri);
        valid_user(user);
        valid_pass(uri);
        return 0;
    }

    char user[128], method[64], uri[700], qop[32], nc[32], cnonce[200], extra[512];
    field(&p, end, user, sizeof user);
    field(&p, end, method, sizeof method);
    field(&p, end, uri, sizeof uri);
    field(&p, end, qop, sizeof qop);
    field(&p, end, nc, sizeof nc);
    field(&p, end, cnonce, sizeof cnonce);
    field(&p, end, extra, sizeof extra);
    int unq = (mode & 0x40) != 0, rtsp = (mode & 4) != 0;

    char nonce[25];
    make_nonce(now_s() - k_ages[mode >> 3 & 7], nonce);

    static char h[2048];
    snprintf(h, sizeof h, "Digest ");
    put_param(h, sizeof h, "username", user, unq);
    put_param(h, sizeof h, "realm", AUTH_REALM, 0);
    put_param(h, sizeof h, "nonce", nonce, unq);
    put_param(h, sizeof h, "uri", uri, unq);
    if (qop[0]) put_param(h, sizeof h, "qop", qop, 1);
    if (nc[0]) put_param(h, sizeof h, "nc", nc, 1);
    if (cnonce[0]) put_param(h, sizeof h, "cnonce", cnonce, unq);
    if (extra[0]) { size_t o = strlen(h); snprintf(h + o, sizeof h - o, ", %s", extra); }

    /* the response for what the parser extracts from this header */
    char g_uri[512] = "", g_nonce[64] = "", g_qop[16] = "", g_nc[16] = "", g_cn[128] = "";
    get_param(h + 7, "uri", g_uri, sizeof g_uri);
    get_param(h + 7, "nonce", g_nonce, sizeof g_nonce);
    get_param(h + 7, "qop", g_qop, sizeof g_qop);
    get_param(h + 7, "nc", g_nc, sizeof g_nc);
    get_param(h + 7, "cnonce", g_cn, sizeof g_cn);
    char ha2[33], resp[33];
    md5_ctx c; uint8_t d[16];
    md5_init(&c); md5_update(&c, method, strlen(method)); md5_update(&c, ":", 1);
    md5_update(&c, g_uri, strlen(g_uri)); md5_final(&c, d); md5_to_hex(d, ha2);
    md5_init(&c); md5_update(&c, g_ha1, 32); md5_update(&c, ":", 1); md5_update(&c, g_nonce, strlen(g_nonce)); md5_update(&c, ":", 1);
    if (g_qop[0]) {
        md5_update(&c, g_nc, strlen(g_nc)); md5_update(&c, ":", 1);
        md5_update(&c, g_cn, strlen(g_cn)); md5_update(&c, ":", 1);
        md5_update(&c, g_qop, strlen(g_qop)); md5_update(&c, ":", 1);
    }
    md5_update(&c, ha2, 32); md5_final(&c, d); md5_to_hex(d, resp);
    size_t o = strlen(h);
    snprintf(h + o, sizeof h - o, ", response=\"%s\"", resp);

    const char *req_uri = rtsp ? NULL : uri;
    int r1 = auth_digest_verify_ex(h, method, req_uri);
    if (getenv("FUZZ_DIGEST_SHOW")) fprintf(stderr, "[digest] %d for %s\n", r1, h);
    if (mode & 0x80) {
        int r2 = auth_digest_verify_ex(h, method, req_uri);
        /* a strict request accepted twice with the same nc would be a replay hole.
         * Known, host-only: nc_fresh() compares strtoul()'s unsigned long with a
         * uint32_t, so on LP64 a signed nc ("-0000001") is accepted again; on the
         * 32-bit camera it is not. Set FUZZ_DIGEST_LP64_NC=1 to trap on it too. */
        int signed_nc = strchr(g_nc, '-') != NULL;
        if (!rtsp && r1 == 1 && r2 == 1 && (!signed_nc || getenv("FUZZ_DIGEST_LP64_NC"))) __builtin_trap();
    }
    return 0;
}
