/* Fuzz harness: tls.c PEM uploads from the web API
 *   tls_custom_install()  POST /api/tls/custom (and the ACME client's chain)
 *   tls_acme_ca_set()     POST /api/acme/ca
 *   tls_mqtt_upload()     POST /api/mqtt/upload?what=ca|cert|key
 * Byte 0 selects the function (% 6), the rest is the body. Mode 4 ("cert")
 * splits the body at the first NUL: key uploaded first, then the certificate,
 * so mbedtls_pk_check_pair() runs. Every saved file is removed again after
 * each input so runs stay independent.
 * tls_init() needs a non-loopback IPv4 address for the server certificate;
 * if it fails, g_ready is forced so the parsers are still reached. */
#include "tls.c"

static int g_init_rc;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    g_init_rc = tls_init();
    fprintf(stderr, "[fuzz_pem] tls_init() = %d%s\n", g_init_rc, g_init_rc ? " (forcing g_ready)" : "");
    if (g_init_rc) { seed_once(); g_ready = 1; }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 1) return 0;
    int mode = d[0] % 6;
    const char *pem = (const char *)d + 1;
    size_t len = n - 1;
    /* exact-size heap copy so ASan sees reads past the body */
    char *b = malloc(len ? len : 1);
    memcpy(b, pem, len);
    const char *err = NULL;
    switch (mode) {
    case 0: if (tls_custom_install(b, len, "upload", &err) == 0) tls_custom_clear(); break;
    case 1: if (tls_custom_install(b, len, "acme", &err) == 0) tls_custom_clear(); break;
    case 2: tls_acme_ca_set(b, len, &err); tls_acme_ca_set(NULL, 0, &err); break;
    case 3: err = NULL; tls_mqtt_upload("ca", b, len, &err); break;
    case 4: {
        const char *z = memchr(b, 0, len);
        size_t kl = z ? (size_t)(z - b) : 0;
        err = NULL;
        if (kl) tls_mqtt_upload("key", b, kl, &err);
        err = NULL;
        tls_mqtt_upload("cert", b + kl + (z ? 1 : 0), len - kl - (z ? 1 : 0), &err);
        break;
    }
    case 5: err = NULL; tls_mqtt_upload("key", b, len, &err); break;
    }
    tls_mqtt_clear("ca");
    tls_mqtt_clear("client");
    free(b);
    return 0;
}
