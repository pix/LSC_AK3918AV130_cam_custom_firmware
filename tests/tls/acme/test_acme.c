/* ACME client internals (acme.c): JSON reader (+ fuzzing), base64url, URL /
 * name / setting validation, the HTTP client against a local fake server,
 * JWS signing on the wire (verified again by check_json.py with Python's
 * cryptography), badNonce retry, http-01 table, renewal decision, status
 * JSON. argv[1] = fixtures, argv[2] = output directory. */
#include "acme.c"
#include <poll.h>
#include <arpa/inet.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (c) printf("PASS "); else { fails++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- control registry stand-in ---- */
static ctrl_param_t g_params[32];
static int g_np;
int control_register(const ctrl_param_t *p) { g_params[g_np++] = *p; return 0; }
static const ctrl_param_t *pfind(const char *n) { for (int i = 0; i < g_np; i++) if (!strcmp(g_params[i].name, n)) return &g_params[i]; return NULL; }
int control_get_value(const char *name, char *out, size_t cap, int reveal)
{
    (void)reveal;
    const ctrl_param_t *p = pfind(name);
    if (!p) return -1;
    out[0] = 0;
    return p->get(out, cap);
}
int control_set_value(const char *name, const char *value, const char **err)
{
    const ctrl_param_t *p = pfind(name);
    if (!p) { *err = "unknown setting"; return -1; }
    return p->set(value, err);
}
static int set(const char *n, const char *v) { const char *e; return control_set_value(n, v, &e); }

/* ---- fake HTTP server: one canned response per connection ---- */
static const char *g_q[16];
static size_t g_qlen[16];
static int g_qn, g_qi, g_hold;
static char g_req[16][8192];
static pthread_mutex_t g_fl = PTHREAD_MUTEX_INITIALIZER;
static int g_port;

static void *fake(void *arg)
{
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        pthread_mutex_lock(&g_fl);
        int i = g_qi < g_qn ? g_qi++ : -1;
        pthread_mutex_unlock(&g_fl);
        char *rq = i >= 0 ? g_req[i] : NULL;
        size_t got = 0;
        char tmp[8192];
        for (;;) {                                     /* headers + Content-Length body */
            struct pollfd p = { c, POLLIN, 0 };
            if (poll(&p, 1, 2000) <= 0) break;
            ssize_t n = recv(c, tmp + got, sizeof tmp - 1 - got, 0);
            if (n <= 0) break;
            got += (size_t)n; tmp[got] = 0;
            char *he = strstr(tmp, "\r\n\r\n");
            if (he) {
                char *cl = strcasestr(tmp, "Content-Length:");
                size_t want = cl ? (size_t)atol(cl + 15) : 0;
                if (got >= (size_t)(he + 4 - tmp) + want) break;
            }
        }
        if (rq) memcpy(rq, tmp, got + 1);
        if (i >= 0 && g_q[i]) send(c, g_q[i], g_qlen[i], MSG_NOSIGNAL);
        if (g_hold) { struct pollfd p = { c, POLLIN, 0 }; poll(&p, 1, 3000); }
        close(c);
    }
    return NULL;
}
static void queue(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    pthread_mutex_lock(&g_fl);
    for (int i = 0; i < n; i++) { g_q[i] = va_arg(ap, const char *); g_qlen[i] = g_q[i] ? strlen(g_q[i]) : 0; g_req[i][0] = 0; }
    g_qn = n; g_qi = 0;
    pthread_mutex_unlock(&g_fl);
    va_end(ap);
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---- JSON ---- */
static void test_json(void)
{
    const char *d = "{\"status\":\"pending\",\"identifier\":{\"type\":\"dns\",\"value\":\"cam.example.org\"},"
                    "\"challenges\":[{\"type\":\"http-01\",\"url\":\"https://a/1\",\"token\":\"T1\"},"
                    "{\"type\":\"dns-01\",\"url\":\"https://a/2\",\"token\":\"T2\",\"error\":{\"detail\":\"no \\\"TXT\\\" \\u00e9\"}}],"
                    "\"wildcard\":true,\"n\":-12.5e3,\"z\":null,\"e\":{},\"a\":[]}";
    jt t[128];
    int n = j_parse(d, strlen(d), t, 128);
    char v[200];
    CHECK(n > 0, "parse a typical authorization (%d tokens)", n);
    j_strk(d, t, n, 0, "status", v, sizeof v);                         CHECK(!strcmp(v, "pending"), "status");
    j_strk(d, t, n, j_get(d, t, n, 0, "identifier"), "value", v, sizeof v); CHECK(!strcmp(v, "cam.example.org"), "nested value");
    j_strk(d, t, n, 0, "wildcard", v, sizeof v);                       CHECK(!strcmp(v, "true"), "primitive true");
    j_strk(d, t, n, 0, "n", v, sizeof v);                              CHECK(!strcmp(v, "-12.5e3"), "number");
    CHECK(j_get(d, t, n, 0, "type") < 0, "keys of nested objects are not found at the top");
    CHECK(j_get(d, t, n, 0, "missing") < 0, "missing key");
    int ch = j_get(d, t, n, 0, "challenges"), k = 0;
    char toks[3][20] = { "", "", "" };
    for (int e = ch + 1; e < n && t[e].start < t[ch].end; e = j_next(t, n, e)) j_strk(d, t, n, e, "token", toks[k++ % 3], 20);
    CHECK(k == 2 && !strcmp(toks[0], "T1") && !strcmp(toks[1], "T2"), "array iteration (%d)", k);
    int e2 = j_next(t, n, ch + 1);
    j_strk(d, t, n, j_get(d, t, n, e2, "error"), "detail", v, sizeof v);
    CHECK(!strcmp(v, "no \"TXT\" ?"), "escapes decoded (%s)", v);
    j_strk(d, t, n, 0, "z", v, sizeof v); CHECK(!strcmp(v, "null"), "null");
    CHECK(j_get(d, t, n, 0, "a") > 0 && j_get(d, t, n, 0, "e") > 0, "empty containers");
    char small[4];
    j_strk(d, t, n, 0, "status", small, sizeof small); CHECK(!strcmp(small, "pen"), "truncation to the buffer");

    const char *bad[] = { "{", "}", "{\"a\":\"x}", "[1,2", "{\"a\":[}", "{\"a\":{]}", "\"unterminated", "{\"a\":\"\\", "" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = j_parse(bad[i], strlen(bad[i]), t, 128);
        CHECK(r <= 0 || bad[i][0] == 0, "malformed JSON refused: %s -> %d", bad[i], r);
    }
    char deep[200] = "";
    for (int i = 0; i < 40; i++) strcat(deep, "[");
    CHECK(j_parse(deep, strlen(deep), t, 128) < 0, "nesting over 32 refused");
    CHECK(j_parse(d, strlen(d), t, 10) < 0, "token overflow refused");
    CHECK(j_get("x", t, 0, 0, "a") < 0 && j_str("x", t, -1, v, sizeof v) < 0, "invalid indexes");
    const char *lead = "  \r\n{\"a\":\"b\"} trailing garbage";
    n = j_parse(lead, strlen(lead), t, 16);
    CHECK(n == 3 && j_strk(lead, t, n, 0, "a", v, sizeof v) == 0 && !strcmp(v, "b"), "whitespace before, garbage after the value");

    /* fuzz: mutations of real-looking documents + random bytes (ASan watches) */
    const char *seeds[] = { d,
        "{\"newNonce\":\"https://x/n\",\"newAccount\":\"https://x/a\",\"meta\":{\"profiles\":{\"classic\":\"x\",\"tlsserver\":\"y\"},\"caaIdentities\":[\"letsencrypt.org\"]}}",
        "{\"result\":[{\"id\":\"023e105f4ecef8ad9ca31a8372d0c353\",\"name\":\"example.com\",\"owner\":{\"id\":null},\"permissions\":[\"#dns_records:edit\"]}],\"success\":true,\"errors\":[],\"messages\":[]}",
        "{\"success\":false,\"errors\":[{\"code\":10000,\"message\":\"Authentication error\"}]}" };
    unsigned seed = 12345;
    char buf[1200];
    long total = 0;
    for (int it = 0; it < 60000; it++) {
        const char *s = seeds[it % 4];
        size_t l = strlen(s);
        if (l > sizeof buf) l = sizeof buf;
        memcpy(buf, s, l);
        int muts = 1 + (int)((seed = seed * 1103515245 + 12345) >> 16) % 6;
        for (int m = 0; m < muts; m++) {
            seed = seed * 1103515245 + 12345;
            size_t pos = (seed >> 8) % l;
            const char pool[] = "{}[]\":,\\ ae0\x01\xff";
            buf[pos] = (it % 7 == 0) ? (char)(seed >> 3) : pool[(seed >> 20) % (sizeof pool - 1)];
        }
        size_t ll = (it % 5 == 0) ? (seed >> 4) % (l + 1) : l;       /* truncations too */
        char *heap = malloc(ll ? ll : 1);                              /* exact-size buffer: ASan catches over-reads */
        memcpy(heap, buf, ll);
        int nn = j_parse(heap, ll, t, 128);
        for (int i = 0; i < nn; i++) {
            char o[64];
            j_str(heap, t, i, o, sizeof o);
            if (t[i].type == J_OBJ) { j_get(heap, t, nn, i, "status"); j_get(heap, t, nn, i, "id"); }
            j_next(t, nn, i);
        }
        total += nn > 0;
        free(heap);
    }
    CHECK(1, "60000 fuzzed documents parsed without memory errors (%ld valid)", total);
}

static void test_b64(void)
{
    char o[64];
    struct { const char *in; size_t n; const char *out; } v[] = {
        { "", 0, "" }, { "f", 1, "Zg" }, { "fo", 2, "Zm8" }, { "foo", 3, "Zm9v" }, { "foob", 4, "Zm9vYg" },
        { "\xfb\xff", 2, "-_8" }, { "\x03\xe0", 2, "A-A" } };
    int ok = 1;
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) { b64url(v[i].in, v[i].n, o, sizeof o); ok &= !strcmp(o, v[i].out); if (strcmp(o, v[i].out)) printf("     %s != %s\n", o, v[i].out); }
    CHECK(ok, "base64url vectors (RFC 4648 section 10, URL alphabet, no padding)");
    CHECK(b64url("abcdef", 6, o, 4) == 0 && o[0] == 0, "base64url refuses a short buffer");
}

static void test_validation(void)
{
    url_t u;
    CHECK(url_parse("https://acme-v02.api.letsencrypt.org/directory", &u) == 0 && u.tls && u.port == 443 &&
          !strcmp(u.host, "acme-v02.api.letsencrypt.org") && !strcmp(u.path, "/directory"), "URL https");
    CHECK(url_parse("http://192.168.1.2:8053/update?x=1", &u) == 0 && !u.tls && u.port == 8053 && !strcmp(u.path, "/update?x=1"), "URL http + port + query");
    CHECK(url_parse("https://h", &u) == 0 && !strcmp(u.path, "/"), "URL without path");
    CHECK(url_parse("https://h?q=1", &u) == 0 && !strcmp(u.path, "/?q=1"), "URL query without path");
    const char *bad[] = { "ftp://h/", "https://", "https://h:0/", "https://h:65536/", "https://h:x/", "https://user@h/",
                          "https://h/a b", "https://h/a\r\nX: y", "https://h_x/", "https://h/\x7f", "//h/", "https://[::1]/" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(url_parse(bad[i], &u) != 0, "URL refused: %s", bad[i]);

    char dom[MAX_DOM][254];
    CHECK(parse_domains("Cam.Example.org, *.example.org cam.example.org", dom) == 2 && !strcmp(dom[0], "cam.example.org") &&
          !strcmp(dom[1], "*.example.org"), "domains: lower case, wildcard, duplicates");
    CHECK(parse_domains("", dom) == 0, "domains: empty");
    const char *bd[] = { "a.b,c.d,e.f,g.h,i.j", "localhost", "*.*.example.org", "a..b", "-a.example.org", "a-.example.org",
                         "a_b.example.org", "example.org.", "x*.example.org", "cam.example.org\"" };
    for (size_t i = 0; i < sizeof bd / sizeof bd[0]; i++) CHECK(parse_domains(bd[i], dom) < 0, "domains refused: %s", bd[i]);
    CHECK(set("acme.domains", "cam.example.org") == 0 && set("acme.domains", "bad name!") != 0, "acme.domains setting validates");
    CHECK(set("acme.server", "staging") == 0 && set("acme.server", "https://pebble:14000/dir") == 0 &&
          set("acme.server", "http://insecure/dir") != 0 && set("acme.server", "https://x/\"") != 0, "acme.server");
    CHECK(set("acme.email", "me+cam@example.org") == 0 && set("acme.email", "") == 0 && set("acme.email", "a\"b@x.y") != 0 &&
          set("acme.email", "nope") != 0 && set("acme.email", "a@b@c.d") != 0, "acme.email");
    CHECK(set("acme.dns_token", "AbC-123_x.y~z") == 0 && set("acme.dns_token", "a b") != 0 && set("acme.dns_token", "a\r\nX:y") != 0 &&
          set("acme.dns_token", "a/b") != 0 && set("acme.dns_token", "a&b=c") != 0, "acme.dns_token alphabet");
    CHECK(set("acme.dns_api", "") == 0 && set("acme.dns_api", "http://10.0.0.2:8053") == 0 && set("acme.dns_api", "ftp://x") != 0, "acme.dns_api");
    CHECK(set("acme.challenge", "duckdns") == 0 && s_chal == CH_DUCKDNS && set("acme.challenge", "acme-dns") == 0 && s_chal == CH_ACMEDNS &&
          set("acme.challenge", "tls-alpn") != 0, "acme.challenge");
    CHECK(set("acme.dns_wait", "600") == 0 && set("acme.dns_wait", "601") != 0, "acme.dns_wait range");
    char v[32];
    control_get_value("acme.challenge", v, sizeof v, 0);
    CHECK(!strcmp(v, "acmedns"), "challenge read back as %s", v);
}

static void test_http(const char *outdir)
{
    char err[300], url[128];
    resp_t r;
    snprintf(url, sizeof url, "http://127.0.0.1:%d/p/a?th=1", g_port);

    g_hold = 1;
    queue(1, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nReplay-Nonce: n0nce\r\nLocation: https://x/acct/1\r\nRetry-After: 7\r\n\r\nhelloEXTRA");
    double t0 = now_s();
    int rc = http_do("POST", url, "X-Api-User: u\r\n", "application/json", "{\"a\":1}", 7, &r, err, sizeof err);
    double dt = now_s() - t0;
    CHECK(rc == 0 && r.status == 200 && r.len == 5 && !strcmp(r.body, "hello") && !strcmp(r.nonce, "n0nce") &&
          !strcmp(r.location, "https://x/acct/1") && r.retry_after == 7, "Content-Length response, headers parsed");
    CHECK(dt < 1.0, "stops at Content-Length without waiting for close (%.2f s)", dt);
    CHECK(!strncmp(g_req[0], "POST /p/a?th=1 HTTP/1.1\r\n", 25) && strstr(g_req[0], "\r\nHost: 127.0.0.1:") &&
          strstr(g_req[0], "\r\nX-Api-User: u\r\n") && strstr(g_req[0], "\r\nContent-Type: application/json\r\n") &&
          strstr(g_req[0], "\r\nContent-Length: 7\r\n") && strstr(g_req[0], "\r\n\r\n{\"a\":1}"), "request line, headers and body sent");
    resp_free(&r);

    queue(1, "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    t0 = now_s();
    rc = http_do("GET", url, NULL, NULL, NULL, 0, &r, err, sizeof err);
    dt = now_s() - t0;
    CHECK(rc == 0 && r.status == 201 && !strcmp(r.body, "hello world") && r.len == 11, "chunked body decoded (%s)", rc ? err : r.body);
    CHECK(dt < 1.0, "stops at the last chunk (%.2f s)", dt);
    CHECK(!strstr(g_req[0], "Content-Length"), "GET without body has no Content-Length");
    resp_free(&r);

    g_hold = 0;
    queue(1, "HTTP/1.0 204 No Content\r\nReplay-Nonce: abc\r\n\r\n");
    rc = http_do("GET", url, NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc == 0 && r.status == 204 && r.len == 0 && !strcmp(r.nonce, "abc"), "no body, closed by the server");
    resp_free(&r);
    queue(1, "HTTP/1.1 200 OK\r\n\r\nuntil close");
    rc = http_do("DELETE", url, NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc == 0 && !strcmp(r.body, "until close") && strstr(g_req[0], "DELETE /p/a?th=1") && strstr(g_req[0], "Content-Length: 0"),
          "body until close; DELETE sends Content-Length: 0");
    resp_free(&r);

    static char huge[300 * 1024 + 100];
    int hl = snprintf(huge, sizeof huge, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", 300 * 1024);
    memset(huge + hl, 'x', 300 * 1024); huge[hl + 300 * 1024] = 0;
    queue(1, huge);
    rc = http_do("GET", url, NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc != 0 && strstr(err, "too large"), "response over 256 KB refused (%s)", err);
    queue(1, "garbage\r\n\r\n");
    rc = http_do("GET", url, NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc != 0 && strstr(err, "invalid HTTP"), "garbage response (%s)", err);
    queue(1, NULL);
    rc = http_do("GET", url, NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc != 0 && strstr(err, "no response"), "connection closed without answer (%s)", err);
    queue(1, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nfffffff\r\nshort");
    rc = http_do("GET", url, NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc == 0 && r.len == 5, "lying chunk size clamped (%zu bytes)", r.len);
    resp_free(&r);
    rc = http_do("GET", "http://127.0.0.1:1/", NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc != 0 && strstr(err, "cannot connect"), "connection refused (%s)", err);
    rc = http_do("GET", "http://no-such-host.invalid/", NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc != 0 && strstr(err, "resolve"), "unknown host (%s)", err);
    rc = http_do("GET", "https://x/\r\nInjected: 1", NULL, NULL, NULL, 0, &r, err, sizeof err);
    CHECK(rc != 0 && !strstr(err, "Injected"), "invalid URL refused without echoing it (%s)", err);

    /* ---- JWS on the wire: nonce fetch, badNonce retry, jwk then kid ---- */
    char base[64], fn[600];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", g_port);
    snprintf(g_u_nonce, sizeof g_u_nonce, "%s/nonce", base);
    g_nonce[0] = 0; g_kid[0] = 0;
    CHECK(account_key(err, sizeof err) == 0 && strlen(g_thumb) == 43, "account key created, thumbprint %s", g_thumb);
    struct stat st;
    CHECK(stat(F_ACC_KEY, &st) == 0 && (st.st_mode & 0777) == 0600, "account key file is 0600");
    queue(3, "HTTP/1.1 204 No Content\r\nReplay-Nonce: nonce-1\r\nContent-Length: 0\r\n\r\n",
             "HTTP/1.1 400 Bad Request\r\nReplay-Nonce: nonce-2\r\nContent-Type: application/problem+json\r\nContent-Length: 66\r\n\r\n"
             "{\"type\":\"urn:ietf:params:acme:error:badNonce\",\"detail\":\"stale\"}  ",
             "HTTP/1.1 201 Created\r\nReplay-Nonce: nonce-3\r\nLocation: http://127.0.0.1/acct/7\r\nContent-Length: 2\r\n\r\n{}");
    char aurl[100];
    snprintf(aurl, sizeof aurl, "%s/new-acct", base);
    rc = jws_post(aurl, "{\"termsOfServiceAgreed\":true}", &r, err, sizeof err);
    CHECK(rc == 0 && r.status == 201 && !strcmp(g_nonce, "nonce-3"), "badNonce retried with the new nonce (%s)", rc ? err : "ok");
    CHECK(strstr(g_req[0], "GET /nonce ") != NULL, "nonce fetched first");
    resp_free(&r);
    /* save both signed requests for check_json.py: first used nonce-1, retry nonce-2 */
    for (int i = 1; i <= 2; i++) {
        snprintf(fn, sizeof fn, "%s/jws%d.txt", outdir, i);
        FILE *f = fopen(fn, "w");
        const char *b = strstr(g_req[i], "\r\n\r\n");
        fprintf(f, "%s\n%s\n%s\n%s\n", b ? b + 4 : "", g_jwk, g_thumb, i == 1 ? "nonce-1" : "nonce-2");
        fclose(f);
    }
    snprintf(g_kid, sizeof g_kid, "http://127.0.0.1/acct/7");
    queue(1, "HTTP/1.1 200 OK\r\nReplay-Nonce: nonce-4\r\nContent-Length: 2\r\n\r\n{}");
    rc = jws_post(aurl, NULL, &r, err, sizeof err);
    CHECK(rc == 0, "POST-as-GET with kid");
    resp_free(&r);
    snprintf(fn, sizeof fn, "%s/jws3.txt", outdir);
    FILE *f = fopen(fn, "w");
    fprintf(f, "%s\n%s\n%s\n%s\n", strstr(g_req[0], "\r\n\r\n") + 4, g_jwk, g_thumb, "nonce-3");
    fclose(f);
    queue(3, "HTTP/1.1 400 Bad Request\r\nReplay-Nonce: a\r\nContent-Length: 10\r\n\r\nbadNonce  ",
             "HTTP/1.1 400 Bad Request\r\nReplay-Nonce: b\r\nContent-Length: 10\r\n\r\nbadNonce  ",
             "HTTP/1.1 400 Bad Request\r\nReplay-Nonce: c\r\nContent-Length: 10\r\n\r\nbadNonce  ");
    rc = jws_post(aurl, NULL, &r, err, sizeof err);
    CHECK(rc != 0 && strstr(err, "nonce"), "gives up after 3 bad nonces (%s)", err);
    CHECK(jws_post("https://x/\"inject", NULL, &r, err, sizeof err) != 0, "URL with a quote refused for the JWS header");
    /* problem documents */
    resp_t pr = { .status = 403, .body = "{\"type\":\"urn:ietf:params:acme:error:unauthorized\",\"detail\":\"Invalid response from http://cam/.well-known/x: 404\"}" };
    pr.len = strlen(pr.body);
    problem(&pr, "challenge", err, sizeof err);
    CHECK(!strcmp(err, "challenge: unauthorized: Invalid response from http://cam/.well-known/x: 404 (HTTP 403)"), "problem detail: %s", err);
    pr.body = "<html>proxy error</html>"; pr.len = strlen(pr.body); pr.status = 502;
    problem(&pr, "new order", err, sizeof err);
    CHECK(!strcmp(err, "new order: error (HTTP 502)"), "non-JSON error: %s", err);
}

static void test_http01(void)
{
    char o[300];
    CHECK(acme_http01("tok", o, sizeof o) < 0, "unknown token -> 404");
    http01_set(0, "tok-A_1", "tok-A_1.thumb");
    http01_set(3, "tok-B", "tok-B.thumb");
    CHECK(acme_http01("tok-A_1", o, sizeof o) > 0 && !strcmp(o, "tok-A_1.thumb"), "token served");
    CHECK(acme_http01("tok-B", o, sizeof o) > 0 && !strcmp(o, "tok-B.thumb"), "second slot");
    CHECK(acme_http01("tok-A", o, sizeof o) < 0 && acme_http01("", o, sizeof o) < 0, "prefix / empty token not served");
    http01_set(0, NULL, NULL); http01_set(3, NULL, NULL);
    CHECK(acme_http01("tok-A_1", o, sizeof o) < 0 && acme_http01("", o, sizeof o) < 0, "cleared");
}

static void install(const char *fx, const char *crt, const char *key, const char *src)
{
    char p[600], *buf = calloc(1, 65536);
    size_t n = 0;
    const char *v[3] = { crt, "inter.crt", key };
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", fx, v[i]); FILE *f = fopen(p, "r"); n += fread(buf + n, 1, 65536 - n, f); fclose(f); }
    const char *e;
    if (tls_custom_install(buf, n, src, &e) != 0) printf("FAIL install %s: %s\n", crt, e);
    free(buf);
}

static void test_renewal(const char *fx)
{
    char why[120];
    set("acme.domains", "cam.example.org, *.wild.example.org");
    tls_custom_clear();
    CHECK(needs_cert(why, sizeof why) && strstr(why, "no certificate"), "no certificate -> issue (%s)", why);
    install(fx, "good.crt", "ec.key", "upload");
    CHECK(needs_cert(why, sizeof why) && strstr(why, "uploaded"), "uploaded certificate -> replaced (%s)", why);
    install(fx, "good.crt", "ec.key", "acme");
    CHECK(!needs_cert(why, sizeof why), "same names, 60 days left -> nothing to do");
    set("acme.domains", "*.WILD.example.org cam.example.org");
    CHECK(!needs_cert(why, sizeof why), "same names in another order / case -> nothing to do");
    set("acme.domains", "cam.example.org");
    CHECK(needs_cert(why, sizeof why) && strstr(why, "changed"), "a name removed -> issue (%s)", why);
    set("acme.domains", "cam.example.org, *.wild.example.org, new.example.org");
    CHECK(needs_cert(why, sizeof why) && strstr(why, "changed"), "a name added -> issue");
    set("acme.domains", "cam.example.org,*.wild.example.org");
    install(fx, "soon.crt", "e3.key", "acme");
    CHECK(needs_cert(why, sizeof why) && strstr(why, "renewal"), "10 days left -> renewal (%s)", why);
}

static void test_status(const char *outdir)
{
    alog("a \"quoted\" message with \\ backslash and \x01 control and \xc3\xa9");
    for (int i = 0; i < 30; i++) alog("line %d", i);
    set_state("error", "x\"y\\z\nw");
    static char js[8192];
    int n = acme_status_json(js, sizeof js);
    char fn[600];
    snprintf(fn, sizeof fn, "%s/status.json", outdir);
    FILE *f = fopen(fn, "w"); fwrite(js, 1, (size_t)n, f); fclose(f);
    CHECK(n > 0 && n < (int)sizeof js && strstr(js, "line 29") && !strstr(js, "line 5\""), "status JSON keeps the last 24 lines");
    char tiny[100];
    n = acme_status_json(tiny, sizeof tiny);
    CHECK(n < (int)sizeof tiny, "status JSON truncates safely (%d)", n);
}

int main(int argc, char **argv)
{
    const char *fx = argv[1], *outdir = argv[2];
    mkdir(outdir, 0755);
    setvbuf(stdout, NULL, _IOLBF, 0);
    acme_init();
    CHECK(tls_init() == 0, "tls_init");
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (struct sockaddr *)&a, sizeof a);
    listen(ls, 8);
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    pthread_t t;
    pthread_create(&t, NULL, fake, (void *)(intptr_t)ls);

    test_json();
    test_b64();
    test_validation();
    test_http(outdir);
    test_http01();
    test_renewal(fx);
    test_status(outdir);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
