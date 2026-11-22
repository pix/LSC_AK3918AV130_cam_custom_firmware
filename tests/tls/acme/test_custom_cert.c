/* Custom HTTPS certificate (tls.c): upload validation, SNI selection over a
 * real handshake, persistence, expiry, host names, status JSON.
 * Built by run.sh with -DTLS_DIR=<temp dir>; argv[1] = fixtures, argv[2] = JSON output file. */
#include "tls.c"
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (c) printf("PASS "); else { fails++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *FX;
static char *slurp(const char *name, size_t *n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", FX, name);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("FAIL missing fixture %s\n", name); exit(1); }
    char *b = malloc(65536);
    size_t l = fread(b, 1, 65535, f);
    fclose(f);
    b[l] = 0;
    if (n) *n = l;
    return b;
}
/* concatenation of fixture files */
static char *cat(size_t *n, const char *a, const char *b, const char *c)
{
    char *out = calloc(1, 200000);
    size_t o = 0;
    const char *v[3] = { a, b, c };
    for (int i = 0; i < 3; i++) if (v[i]) { size_t l; char *s = slurp(v[i], &l); memcpy(out + o, s, l); o += l; free(s); }
    *n = o;
    return out;
}

static int try_install(const char *a, const char *b, const char *c, const char **err)
{
    size_t n;
    char *buf = cat(&n, a, b, c);
    int r = tls_custom_install(buf, n, "upload", err);
    free(buf);
    return r;
}

static void expect_ok(const char *what, const char *a, const char *b, const char *c)
{
    const char *err = NULL;
    int r = try_install(a, b, c, &err);
    CHECK(r == 0, "accept %s%s%s", what, r ? ": " : "", r ? err : "");
}
static void expect_err(const char *what, const char *needle, const char *a, const char *b, const char *c)
{
    const char *err = NULL;
    int r = try_install(a, b, c, &err);
    CHECK(r != 0 && err && strstr(err, needle), "reject %s (\"%s\")", what, err ? err : "(no error)");
}

/* ---- handshake through a socketpair: which certificate does the server send for this SNI? ---- */
static void *srv(void *arg)
{
    conn_t *c = tls_accept((int)(intptr_t)arg, 5000);
    if (c) conn_close(c);
    return NULL;
}
static int served_pin(const char *sni, char pin[45])
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pthread_t t;
    pthread_create(&t, NULL, srv, (void *)(intptr_t)sv[0]);
    char err[200];
    conn_t *c = tls_client_connect(sv[1], sni, 0, 0, NULL, 5000, err, sizeof err);
    pin[0] = 0;
    if (c) { snprintf(pin, 45, "%s", ((cli_t *)c->extra)->leaf_pin); conn_close(c); }
    else printf("     handshake failed: %s\n", err);
    pthread_join(t, NULL);
    return c ? 0 : -1;
}
static void file_pin(const char *fixture, char pin[45])
{
    size_t n;
    char *b = slurp(fixture, &n);
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    pin[0] = 0;
    if (mbedtls_x509_crt_parse(&c, (unsigned char *)b, n + 1) == 0) spki_b64(&c, pin);
    mbedtls_x509_crt_free(&c);
    free(b);
}
static void camera_pin(char pin[45])
{
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    pin[0] = 0;
    if (load_crt(&c, F_CRT) == 0) spki_b64(&c, pin);
    mbedtls_x509_crt_free(&c);
}

static volatile int stop;
static int hs_ok, hs_n;
static pthread_mutex_t hs_lock = PTHREAD_MUTEX_INITIALIZER;
static void *hammer(void *a)
{
    (void)a;
    const char *names[] = { "cam.example.org", "x.wild.example.org", "other.lan" };
    char pin[45];
    for (int i = 0; !stop; i++) {
        int r = served_pin(names[i % 3], pin) == 0;
        pthread_mutex_lock(&hs_lock); hs_ok += r; hs_n++; pthread_mutex_unlock(&hs_lock);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    FX = argv[1];
    FILE *jo = fopen(argv[2], "w");
    CHECK(tls_init() == 0, "tls_init (camera CA + server certificate)");
    char cam[45], got[45], good[45], rsa[45];
    camera_pin(cam);
    file_pin("good.crt", good);
    file_pin("rsa.crt", rsa);
    CHECK(cam[0] && good[0] && rsa[0], "fixture key pins");

    /* nothing installed: camera certificate for every name */
    CHECK(served_pin("cam.example.org", got) == 0 && !strcmp(got, cam), "no custom certificate: camera certificate served");
    static char js[8192];
    tls_info_json(js, sizeof js);
    CHECK(strstr(js, "\"custom\":null") != NULL, "status: custom null before any upload");
    fprintf(jo, "%s\n", js);

    /* ---- rejections ---- */
    const char *err;
    CHECK(tls_custom_install("", 0, "upload", &err) != 0 && strstr(err, "empty"), "reject empty body (%s)", err);
    char *big = calloc(1, 40000); memset(big, 'A', 39999);
    CHECK(tls_custom_install(big, 39999, "upload", &err) != 0 && strstr(err, "too large"), "reject 40 KB body (%s)", err);
    CHECK(tls_custom_install(big, 30000, "upload", &err) != 0 && strstr(err, "no PEM certificate"), "reject garbage (%s)", err);
    free(big);
    expect_err("certificate without key", "no private key", "good.crt", "inter.crt", NULL);
    expect_err("key without certificate", "no PEM certificate", "ec.key", NULL, NULL);
    expect_err("two keys", "more than one private key", "good.crt", "ec.key", "other.key");
    expect_err("wrong key", "does not belong", "good.crt", "inter.crt", "other.key");
    expect_err("CA first, server certificate second", "must come first", "inter.crt", "good.crt", "ec.key");
    expect_err("encrypted PKCS#8 key", "encrypted", "good.crt", "ec_enc.key", NULL);
    expect_err("encrypted traditional RSA key", "encrypted", "rsa.crt", "rsa_trad_enc.key", NULL);
    expect_err("expired certificate", "expired", "expired.crt", "inter.crt", "e1.key");
    expect_err("not yet valid certificate", "not valid yet", "future.crt", "inter.crt", "e2.key");
    expect_err("no subjectAltName", "no DNS name", "nosan.crt", "e4.key", NULL);
    expect_err("clientAuth-only certificate", "not for servers", "client.crt", "e6.key", NULL);
    expect_err("key usage without digitalSignature", "digital signatures", "kuenc.crt", "e8.key", NULL);
    expect_err("CA certificate as server certificate", "CA certificate", "isca.crt", "e9.key", NULL);
    expect_err("RSA-1024", "2048 to 4096", "rsa1024.crt", "rsa1024.key", NULL);
    expect_err("P-521", "P-256 / P-384", "p521.crt", "p521.key", NULL);
    {   /* truncated PEM (no END line) and corrupted base64 */
        size_t n; char *b = cat(&n, "good.crt", "ec.key", NULL);
        char *e = strstr(b, "-----END CERTIFICATE-----"); memcpy(e, "-----XND", 8);
        CHECK(tls_custom_install(b, n, "upload", &err) != 0 && strstr(err, "no PEM certificate"), "reject truncated PEM (%s)", err);
        free(b);
        b = cat(&n, "good.crt", "ec.key", NULL);
        char *m = strchr(b, '\n') + 30; memcpy(m, "!!!!", 4);
        CHECK(tls_custom_install(b, n, "upload", &err) != 0, "reject corrupted certificate (%s)", err);
        free(b);
    }
    {   /* regression (fuzzing): PEM blocks back to back overflowed the split buffer */
        char adj[600] = "";
        for (int i = 0; i < 10; i++) strcat(adj, "-----BEGIN CERTIFICATE-----END CERTIFICATE-----");
        CHECK(tls_custom_install(adj, strlen(adj), "upload", &err) != 0, "10 adjacent empty certificate blocks refused, no overflow (%s)", err);
        size_t n; char *b = cat(&n, "good.crt", NULL, NULL), *nine = calloc(1, 9 * n + 4096);
        size_t o = 0;
        for (int i = 0; i < 9; i++) { memcpy(nine + o, b, n); o += n; }
        char *k = slurp("ec.key", &n); memcpy(nine + o, k, n); o += n;
        CHECK(tls_custom_install(nine, o, "upload", &err) != 0 && strstr(err, "8 at most"), "9 certificates refused (%s)", err);
        free(b); free(k); free(nine);
    }
    CHECK(access(F_CUS_CRT, F_OK) != 0 && !g_cus, "nothing saved after rejections");
    CHECK(served_pin("cam.example.org", got) == 0 && !strcmp(got, cam), "still the camera certificate after rejections");

    /* ---- accepted forms ---- */
    expect_ok("RSA-2048 (keyEncipherment + digitalSignature)", "rsa.crt", "inter.crt", "rsa.key");
    CHECK(served_pin("rsa.example.org", got) == 0 && !strcmp(got, rsa), "RSA certificate served for its name (ECDHE-RSA)");
    expect_ok("traditional RSA key, key first", "rsa_trad.key", "rsa.crt", "inter.crt");
    expect_ok("P-384", "p384.crt", "inter.crt", "p384.key");
    expect_ok("SEC1 key with EC PARAMETERS block", "sec1.crt", "sec1.key", NULL);
    expect_ok("no extended key usage", "noeku.crt", "e7.key", NULL);
    expect_ok("20 names (first 16 kept)", "many.crt", "e11.key", NULL);
    CHECK(g_cus_n == 16 && tls_name_ok("n16.example.org") && !tls_name_ok("n17.example.org"), "names capped at 16");
    expect_ok("mixed-case and invalid names", "upper.crt", "e10.key", NULL);
    CHECK(g_cus_n == 1 && !strcmp(g_cus_names[0], "cam.upper.example.org"), "names lower-cased, invalid ones dropped (%d: %s)", g_cus_n, g_cus_names[0]);
    /* address-only certificate (company CA for 192.168.1.5): used when a
     * client without SNI connects to that address, never for names */
    expect_ok("address-only subjectAltName", "ipsan.crt", "e5.key", NULL);
    {
        uint32_t a5 = inet_addr("192.168.1.5"), a6 = inet_addr("192.168.1.6");
        pthread_mutex_lock(&g_cur_lock);
        int i1 = custom_for(NULL, 0, a5) == g_cus, i2 = custom_for(NULL, 0, a6) == NULL, i3 = custom_for(NULL, 0, 0) == NULL,
            i4 = custom_for("other.lan", 9, a5) == NULL, i5 = g_cus_n == 0 && g_cus_nip == 1;
        pthread_mutex_unlock(&g_cur_lock);
        CHECK(i1 && i2 && i3 && i4 && i5, "IP certificate: its address without SNI only (%d%d%d%d%d)", i1, i2, i3, i4, i5);
        tls_info_json(js, sizeof js);
        fprintf(jo, "%s\n", js);
        CHECK(strstr(js, "\"ips\":\"192.168.1.5\"") != NULL, "status JSON lists the address");
        CHECK(!tls_name_ok("192.168.1.5.example") && served_pin("other.lan", got) == 0 && !strcmp(got, cam), "IP certificate not served for names");
    }
    expect_ok("leaf only (no chain)", "good.crt", "ec.key", NULL);
    expect_ok("EC P-256 with chain", "good.crt", "inter.crt", "ec.key");

    /* ---- selection ---- */
    CHECK(served_pin("cam.example.org", got) == 0 && !strcmp(got, good), "SNI cam.example.org -> custom");
    CHECK(served_pin("CAM.Example.Org", got) == 0 && !strcmp(got, good), "SNI case-insensitive -> custom");
    CHECK(served_pin("a.wild.example.org", got) == 0 && !strcmp(got, good), "SNI a.wild.example.org -> custom (wildcard)");
    CHECK(served_pin("wild.example.org", got) == 0 && !strcmp(got, cam), "SNI wild.example.org -> camera (wildcard needs a label)");
    CHECK(served_pin("a.b.wild.example.org", got) == 0 && !strcmp(got, cam), "SNI a.b.wild.example.org -> camera (one label only)");
    CHECK(served_pin("example.org", got) == 0 && !strcmp(got, cam), "SNI example.org -> camera");
    CHECK(served_pin("xcam.example.org", got) == 0 && !strcmp(got, cam), "SNI xcam.example.org -> camera");
    pthread_mutex_lock(&g_cur_lock);
    int t1 = custom_for("cam.example.org.", 16, 0) != NULL, t2 = custom_for(NULL, 0, 0) == NULL, t3 = custom_for("cam.example.org", 0, 0) == NULL,
        t4 = custom_for(".wild.example.org", 17, 0) == NULL, t5 = custom_for(NULL, 0, inet_addr("192.168.1.5")) == NULL;
    pthread_mutex_unlock(&g_cur_lock);
    CHECK(t1 && t2 && t3 && t4 && t5, "custom_for: trailing dot, no SNI, empty, empty label, address not in the certificate");

    /* ---- "use it for every connection" ---- */
    const char *e2;
    CHECK(tls_set_config(NULL, -1, 1, &e2) == 0 && g_cus_all, "custom_all on");
    CHECK(served_pin("other.lan", got) == 0 && !strcmp(got, good), "custom_all: other.lan -> custom");
    CHECK(served_pin("wild.example.org", got) == 0 && !strcmp(got, good), "custom_all: uncovered name -> custom");
    pthread_mutex_lock(&g_cur_lock);
    int a1 = custom_for(NULL, 0, 0) == g_cus, a2 = custom_for(NULL, 0, inet_addr("10.1.2.3")) == g_cus;
    pthread_mutex_unlock(&g_cur_lock);
    CHECK(a1 && a2, "custom_all: no SNI / any address -> custom");
    {
        char cf[400] = "";
        read_file(F_CONF, cf, sizeof cf);
        CHECK(strstr(cf, "custom_all=1") != NULL, "custom_all saved (%s)", cf);
        g_cus_all = 0;
        load_conf();
        CHECK(g_cus_all == 1, "custom_all read back from the file");
    }
    time_t keep0 = g_cus_na;
    g_cus_na = time(NULL) - 1;
    CHECK(served_pin("other.lan", got) == 0 && !strcmp(got, cam), "custom_all + expired: camera certificate comes back");
    g_cus_na = keep0;
    tls_info_json(js, sizeof js);
    fprintf(jo, "%s\n", js);
    CHECK(strstr(js, "\"custom_all\":true") != NULL, "status JSON: custom_all true");
    /* the reset path (button / reset_auth flag) turns it and the redirect off,
     * keeping the names, even when it runs before HTTPS has started */
    CHECK(tls_set_config("camera.lan, cam2.lan", 1, -1, &e2) == 0 && g_redirect && g_cus_all, "names + redirect set");
    g_ready = 0;
    g_nnames = 0;                              /* as at boot, before tls_init */
    g_cus_all = 0; g_redirect = 0;
    CHECK(tls_set_config(NULL, 0, 0, &e2) == 0, "reset before tls_init");
    g_ready = 1;
    {
        char cf[400] = "";
        read_file(F_CONF, cf, sizeof cf);
        CHECK(strstr(cf, "names=camera.lan,cam2.lan\n") && strstr(cf, "redirect=0") && strstr(cf, "custom_all=0"),
              "reset kept the names, turned redirect and custom_all off (%s)", cf);
    }
    CHECK(served_pin("other.lan", got) == 0 && !strcmp(got, cam) && served_pin("cam.example.org", got) == 0 && !strcmp(got, good),
          "after reset: camera certificate for other names, custom still for its names");
    /* regression (valgrind): a removed name must stop being accepted */
    CHECK(tls_set_config("one.lan, two.lan", -1, -1, &e2) == 0 && tls_name_ok("two.lan"), "two names accepted");
    CHECK(tls_set_config("one.lan", -1, -1, &e2) == 0 && tls_name_ok("one.lan") && !tls_name_ok("two.lan"), "removed name refused");
    CHECK(tls_set_config("", -1, -1, &e2) == 0, "names cleared again");
    CHECK(!tls_name_ok("one.lan") && !tls_name_ok(""), "no names left");
    camera_pin(cam);                           /* re-issued for the new names */
    CHECK(tls_name_ok("cam.example.org") && tls_name_ok("x.wild.example.org") && !tls_name_ok("wild.example.org") &&
          !tls_name_ok("evil.org"), "host allow-list includes the certificate names");

    /* ---- files, state, JSON ---- */
    struct stat st;
    CHECK(stat(F_CUS_KEY, &st) == 0 && (st.st_mode & 0777) == 0600, "key file is 0600");
    char names[4200], src[8];
    long left;
    CHECK(tls_custom_state(names, sizeof names, src, &left) == 1 && !strcmp(names, "cam.example.org,*.wild.example.org") &&
          !strcmp(src, "upload") && left > 50 * 86400, "tls_custom_state (%s, %s, %ld d)", names, src, left / 86400);
    tls_info_json(js, sizeof js);
    fprintf(jo, "%s\n", js);
    CHECK(strstr(js, "\"present\":true,\"active\":true") && strstr(js, "Test Intermediate"), "status JSON: present, active, issuer");

    /* reload from disk (reboot) */
    pthread_mutex_lock(&g_cur_lock); cus_retire(g_cus); g_cus = NULL; g_cus_n = 0; cus_reap(); pthread_mutex_unlock(&g_cur_lock);
    CHECK(custom_load() == 0 && g_cus && g_cus_n == 2, "loaded again from the saved files");
    CHECK(served_pin("cam.example.org", got) == 0 && !strcmp(got, good), "served after reload");

    /* expired while installed: not served, shown */
    time_t keep = g_cus_na;
    g_cus_na = time(NULL) - 1;
    CHECK(served_pin("cam.example.org", got) == 0 && !strcmp(got, cam), "expired: camera certificate served instead");
    tls_info_json(js, sizeof js);
    fprintf(jo, "%s\n", js);
    CHECK(strstr(js, "\"active\":false") != NULL, "status JSON: active false when expired");
    g_cus_na = keep;

    /* an expired certificate on disk (e.g. after months off): kept, shown, not served */
    {
        size_t n; char *b;
        b = slurp("expired.crt", &n); write_file(F_CUS_CRT, b, n, 0644); free(b);
        b = slurp("e1.key", &n); write_file(F_CUS_KEY, b, n, 0600); free(b);
        CHECK(custom_load() == 0 && g_cus && strstr(g_cus_err, "expired"), "expired file loaded with an error (%s)", g_cus_err);
        CHECK(served_pin("old.example.org", got) == 0 && !strcmp(got, cam), "expired file not served");
        /* corrupt file: error, previous state kept */
        write_file(F_CUS_KEY, "junk", 4, 0600);
        CHECK(custom_load() != 0 && strstr(g_cus_err, "unreadable"), "corrupt key file reported (%s)", g_cus_err);
        tls_info_json(js, sizeof js);
        fprintf(jo, "%s\n", js);
    }

    /* ACME source + renewal window */
    {
        size_t n; char *b = cat(&n, "soon.crt", "inter.crt", "e3.key");
        CHECK(tls_custom_install(b, n, "acme", &err) == 0, "install as acme");
        free(b);
        CHECK(tls_custom_state(names, sizeof names, src, &left) && !strcmp(src, "acme") && left < 11 * 86400, "source acme, %ld days left", left / 86400);
    }

    tls_custom_clear();
    CHECK(!g_cus && access(F_CUS_CRT, F_OK) != 0 && access(F_CUS_KEY, F_OK) != 0, "clear removes the files");
    CHECK(served_pin("cam.example.org", got) == 0 && !strcmp(got, cam), "after clear: camera certificate");
    CHECK(!tls_name_ok("cam.example.org"), "after clear: names no longer allowed");
    tls_info_json(js, sizeof js);
    fprintf(jo, "%s\n", js);

    /* extra ACME CA */
    {
        size_t n; char *b = slurp("extra_ca.crt", &n);
        CHECK(tls_acme_ca_set(b, n, &err) == 0 && access(F_ACME_CA, F_OK) == 0, "extra CA saved");
        free(b);
        b = slurp("good.crt", &n);
        CHECK(tls_acme_ca_set(b, n, &err) != 0 && strstr(err, "not a CA"), "server certificate refused as extra CA");
        free(b);
        CHECK(tls_acme_ca_set("junk", 4, &err) != 0, "junk refused as extra CA");
        CHECK(tls_acme_ca_set(NULL, 0, &err) == 0 && access(F_ACME_CA, F_OK) != 0, "extra CA removed");
    }
    /* a pair held by a handshake survives its replacement and is freed on release */
    try_install("good.crt", "inter.crt", "ec.key", &err);
    pthread_mutex_lock(&g_cur_lock);
    pair_t *held = g_cus;
    held->refs++;
    pthread_mutex_unlock(&g_cur_lock);
    try_install("good.crt", "inter.crt", "ec.key", &err);
    tls_custom_clear();
    pthread_mutex_lock(&g_cur_lock);
    int still = 0;
    for (pair_t *q = g_cus_old; q; q = q->prev) still += q == held;
    int n_old = 0;
    for (pair_t *q = g_cus_old; q; q = q->prev) n_old++;
    held->refs--;
    int nleft = cus_reap();
    pthread_mutex_unlock(&g_cur_lock);
    CHECK(still && n_old == 1 && nleft == 0 && !g_cus_old, "held pair kept while in use (%d retired), freed on release", n_old);

    /* concurrency: handshakes in a loop while the certificate is replaced and removed (ASan: no use after free) */
    {
        pthread_t th[3];
        for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, hammer, NULL);
        for (int i = 0; i < 60; i++) {
            try_install(i % 2 ? "good.crt" : "rsa.crt", "inter.crt", i % 2 ? "ec.key" : "rsa.key", &err);
            if (i % 4 == 0) tls_custom_clear();
            usleep(20000);
        }
        stop = 1;
        for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
        pthread_mutex_lock(&g_cur_lock);
        int remaining = cus_reap();
        pthread_mutex_unlock(&g_cur_lock);
        CHECK(hs_ok == hs_n && hs_n > 30, "%d handshakes during 60 replacements, all completed", hs_n);
        CHECK(remaining == 0 && !g_cus_old, "every replaced certificate freed (%d left)", remaining);
    }
    CHECK(served_pin("x.wild.example.org", got) == 0, "handshake after the replacement storm");
    fclose(jo);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
