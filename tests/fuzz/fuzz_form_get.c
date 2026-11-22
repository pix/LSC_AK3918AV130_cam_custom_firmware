/* Example harness: web.c form_get() (URL-encoded form fields from request bodies
 * and query strings) and host_allowed() (Host header check). */
#include "web.c"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    char *s = malloc(n + 1);
    memcpy(s, d, n);
    s[n] = 0;
    char out[64];
    form_get(s, "name", out, sizeof out);
    form_get(s, "value", out, sizeof out);
    form_get(s, "", out, 1);
    char host[128];
    snprintf(host, sizeof host, "%s", s);
    host_allowed(host);
    free(s);
    return 0;
}
