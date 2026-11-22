/* libFuzzer harness: push notifications (notify.c) - everything that reads
 * data from the network or from settings.
 *
 * Byte 0 picks the target, the rest is the input:
 *   0  HTTP response parser (status, headers, Content-Length, chunked
 *      de-chunking in place, Retry-After) + each service's answer
 *      interpretation (Telegram "ok"/"description", Pushover "status"/
 *      "errors", Discord "retry_after"/"message", Gotify "errorDescription",
 *      ntfy "error")
 *   1  "is the response complete" check used while reading
 *   2  JSON member lookup (first input byte = key length, then key, then JSON;
 *      also with a 1- and 8-byte output buffer)
 *   3  URL parser and the settings validators (notify.url, notify.events,
 *      notify.image_url, tokens, user ids)
 *   4  header encoding (RFC 2047) and JSON escaping of titles / messages
 * notify.c is included to reach its static functions; the library's copy of
 * it is not linked (build_fuzzer.sh: --allow-multiple-definition).
 *   tests/host/build_host.sh /tmp/fz fuzz
 *   tests/fuzz/build_fuzzer.sh tests/fuzz/fuzz_notify.c /tmp/fz
 *   /tmp/fz/fuzz_notify -max_total_time=300 corpus_dir */
#include "notify.c"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 1 || n > 65536) return 0;
    int mode = d[0] % 5;
    d++; n--;
    char *b = malloc(n + 1);
    if (!b) return 0;
    memcpy(b, d, n);
    b[n] = 0;
    char err[200], out[256];
    switch (mode) {
    case 0: {
        notify_resp_t r;
        if (notify_parse_response(b, n, &r) == 0) {
            if (r.body_len > n || (r.body && strlen(r.body) > r.body_len)) __builtin_trap();
            for (int s = 0; s < NOTIFY_NSERVICES; s++) {
                int rm = 0;
                int rc = notify_result(s, &r, &rm, err, sizeof err);
                if (rc != 0 && strlen(err) >= sizeof err) __builtin_trap();
                if (rm < 0) __builtin_trap();
            }
        }
        break;
    }
    case 1:
        (void)resp_complete(b, n);
        break;
    case 2: {
        size_t kl = n ? (uint8_t)b[0] % 24 : 0;
        if (kl + 1 > n) break;
        char key[32];
        memcpy(key, b + 1, kl);
        key[kl] = 0;
        const char *js = (const char *)d + 1 + kl;    /* not NUL-terminated */
        size_t jl = n - 1 - kl;
        char tiny[8];
        notify_json_get(js, jl, key, out, sizeof out);
        notify_json_get(js, jl, key, tiny, sizeof tiny);
        notify_json_get(js, jl, key, tiny, 1);
        notify_json_get(js, jl, "errors", out, sizeof out);
        break;
    }
    case 3: {
        url_t u;
        const char *e = "";
        if (url_parse(b, &u) == 0 && (strlen(u.host) >= sizeof u.host || u.port < 1 || u.port > 65535 || u.path[0] != '/')) __builtin_trap();
        (void)url_ok(b); (void)img_ok(b); (void)tok_ok(b); (void)user_ok(b); (void)bot_token_ok(b); (void)digits_ok(b);
        if (p_ev_set(b, &e) == 0) { p_ev_get(out, sizeof out); if (p_ev_set(out, &e) != 0) __builtin_trap(); }
        break;
    }
    case 4: {
        char h[400], js[1024];
        sb_t s = { js, 0, sizeof js, 0 };
        if (n < 300) { hdr_enc(b, h, sizeof h); hdr_enc(b, h, 20); }
        json_esc(&s, b);
        if (s.n >= sizeof js) __builtin_trap();
        break;
    }
    }
    free(b);
    return 0;
}
