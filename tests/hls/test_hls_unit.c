/* Unit test for hls.c: tokens against a fake clock (expiry rules, LRU,
 * revocation), the /hls/ path parser, the segment ring (count / byte bounds,
 * consecutive numbers, target duration) and the playlist text.
 * Built and run by tests/hls/run.sh against the host library (ASan+UBSan or
 * valgrind). Prints PASS/FAIL lines; exit status = failures. */
#define mono_s  t_mono_s                 /* hls.c's clock -> the fake one below */
#define mono_ms t_mono_ms
#include "hls.c"
#undef mono_s
#undef mono_ms

static uint32_t g_now = 100000;
uint32_t t_mono_s(void) { return g_now; }
int64_t  t_mono_ms(void) { return (int64_t)g_now * 1000; }

static int fails, total;
#define CHECK(cond, ...) do { total++; if (cond) printf("PASS "); else { fails++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static int check(const char *t)
{
    pthread_mutex_lock(&g_lock);
    int r = tok_check(t, g_now);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void test_paths(void)
{
    static const struct { const char *p; int kind; uint32_t seq; } c[] = {
        { "/hls/0123456789abcdef0123456789abcdef/index.m3u8", HLS_PLAYLIST, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/index.m3u8?_HLS_msn=3", HLS_PLAYLIST, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/init.mp4", HLS_INIT, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/seg0.m4s", HLS_SEGMENT, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/seg7.m4s", HLS_SEGMENT, 7 },
        { "/hls/0123456789abcdef0123456789abcdef/seg4294967295.m4s", HLS_SEGMENT, 4294967295u },
        { "/hls/0123456789abcdef0123456789abcdef/seg4294967296.m4s", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/seg99999999999.m4s", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/seg07.m4s", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/seg.m4s", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/seg-1.m4s", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/seg1.m4s/x", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/index.m3u8x", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdef/", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdef", HLS_BAD, 0 },
        { "/hls/0123456789ABCDEF0123456789abcdef/index.m3u8", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcde/index.m3u8", HLS_BAD, 0 },
        { "/hls/0123456789abcdef0123456789abcdefa/index.m3u8", HLS_BAD, 0 },
        { "/hls/../../etc/passwd", HLS_BAD, 0 },
        { "/hls//index.m3u8", HLS_BAD, 0 },
        { "/hlsx/0123456789abcdef0123456789abcdef/index.m3u8", HLS_BAD, 0 },
        { "", HLS_BAD, 0 },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        char tok[33] = "";
        uint32_t seq = 0;
        int k = hls_parse_path(c[i].p, tok, &seq);
        int good = k == c[i].kind && (k != HLS_SEGMENT || seq == c[i].seq) &&
                   (k == HLS_BAD || !strcmp(tok, "0123456789abcdef0123456789abcdef"));
        CHECK(good, "path %s -> kind %d seq %u", c[i].p, k, seq);
    }
    CHECK(hls_parse_path(NULL, (char[33]){ 0 }, &(uint32_t){ 0 }) == HLS_BAD, "path NULL");
}

static void test_tokens(void)
{
    const char *e = NULL;
    char t[10][33];
    CHECK(hls_token_new(t[0]) == -1, "no token while off");
    CHECK(hls_en_set("maybe", &e) == -1 && e, "hls.enabled rejects 'maybe'");
    CHECK(hls_en_set("on", &e) == 0 && hls_enabled(), "hls.enabled on");
    CHECK(hls_token_new(t[0]) == 0 && strlen(t[0]) == 32, "token %s", t[0]);
    CHECK(check(t[0]), "fresh token valid");
    char near[33]; memcpy(near, t[0], 33); near[31] = near[31] == '0' ? '1' : '0';
    CHECK(!check(near), "token differing in the last digit refused");
    near[31] = t[0][31]; near[0] = near[0] == '0' ? '1' : '0';
    CHECK(!check(near), "token differing in the first digit refused");
    /* unused: 10 min */
    CHECK(hls_token_new(t[1]) == 0, "second token");
    g_now += HLS_TOK_UNUSED_S - 1;
    CHECK(hls_token_new(t[2]) == 0, "third token");
    g_now += 2;
    CHECK(!check(t[1]), "unused token expired after %d s", HLS_TOK_UNUSED_S + 1);
    CHECK(check(t[2]), "unused token valid after 2 s");
    /* used: 2 min idle */
    g_now += HLS_TOK_IDLE_S;
    CHECK(check(t[2]), "used token valid after exactly %d s idle", HLS_TOK_IDLE_S);
    g_now += HLS_TOK_IDLE_S + 1;
    CHECK(!check(t[2]), "used token expired after %d s idle", HLS_TOK_IDLE_S + 1);
    /* max lifetime 24 h even when used */
    CHECK(hls_token_new(t[3]) == 0, "long-running token");
    uint32_t made = g_now;
    int ok_all = 1;
    while (g_now - made + 100 <= HLS_TOK_MAX_S) { g_now += 100; ok_all &= check(t[3]); }
    CHECK(ok_all, "used every 100 s: valid for 24 h");
    g_now += 101;
    CHECK(!check(t[3]), "refused after 24 h");
    /* 8 slots: a 9th token replaces the least recently used one */
    pthread_mutex_lock(&g_lock); memset(g_tok, 0, sizeof g_tok); pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < HLS_TOKENS; i++) { CHECK(hls_token_new(t[i]) == 0, "slot token %d", i); }
    for (int i = 0; i < HLS_TOKENS; i++) { g_now += 1; check(t[(i + 3) % HLS_TOKENS]); }   /* t[3] used first */
    g_now += 1;
    CHECK(hls_token_new(t[8]) == 0, "9th token");
    int valid = 0;
    for (int i = 0; i < HLS_TOKENS; i++) valid += check(t[i]);
    CHECK(!check(t[3]) && valid == HLS_TOKENS - 1 && check(t[8]), "LRU token (t3) replaced, 7 others kept (%d)", valid);
    /* expired slots are reused before live ones */
    g_now += HLS_TOK_IDLE_S + 1;                   /* all used ones expire */
    CHECK(hls_token_new(t[9]) == 0 && check(t[9]), "token after all expired");
    /* switching off revokes everything */
    CHECK(hls_token_new(t[0]) == 0 && check(t[0]), "token before off");
    CHECK(hls_en_set("off", &e) == 0 && !hls_enabled(), "hls.enabled off");
    CHECK(!check(t[0]) && !check(t[9]), "tokens refused after off");
    hls_en_set("on", &e);
    CHECK(!check(t[0]), "tokens still refused after on again");
    char buf[16];
    hls_en_get(buf, sizeof buf);
    CHECK(!strcmp(buf, "on"), "hls.enabled reads '%s'", buf);
}

static void pub(uint32_t sess, size_t n, uint32_t ms, int key)
{
    wbuf_t b = { 0 };
    b.d = malloc(n); b.n = b.cap = n;
    memset(b.d, 0xab, n);
    publish_seg(sess, &b, (uint64_t)ms * 90, key);
}

static int consecutive(void)
{
    for (int i = 1; i < g_nseg; i++) if (g_seg[i].seq != g_seg[i - 1].seq + 1) return 0;
    return 1;
}

static void test_ring(void)
{
    pthread_mutex_lock(&g_lock);
    g_session = 7; g_producing = 1; g_target = 0;
    pthread_mutex_unlock(&g_lock);
    uint32_t first = g_next_seq;
    for (int i = 0; i < 7; i++) pub(7, 100000, 2000, 1);
    CHECK(g_nseg == HLS_RING && g_seg[0].seq == first + 2 && consecutive(), "count bound: %d kept, first %u", g_nseg, g_seg[0].seq);
    CHECK(g_seg_bytes == (size_t)HLS_RING * 100000, "byte count %zu", g_seg_bytes);
    CHECK(g_target == 2, "target 2 for 2.000 s segments (%u)", g_target);
    pub(7, 100000, 2400, 1);
    CHECK(g_target == 2, "2.4 s rounds to 2 (%u)", g_target);
    pub(7, 100000, 2600, 1);
    CHECK(g_target == 3, "2.6 s raises the target to 3 (%u)", g_target);
    pub(7, 100000, 900, 1);
    CHECK(g_target == 3, "target never lowered (%u)", g_target);
    /* byte budget */
    int okb = 1;
    for (int i = 0; i < 6; i++) {
        pub(7, HLS_SEG_MAX_BYTES, 2000, 1);
        okb &= (g_seg_bytes <= HLS_MEM_MAX || g_nseg <= HLS_MIN_KEEP) && g_nseg >= 1 && consecutive();
    }
    CHECK(okb && g_nseg == HLS_MIN_KEEP, "byte budget: %d x %u B kept, %zu B", g_nseg, HLS_SEG_MAX_BYTES, g_seg_bytes);
    for (int i = 0; i < 3; i++) pub(7, 400000, 2000, 1);
    CHECK(g_seg_bytes <= HLS_MEM_MAX && g_nseg > HLS_MIN_KEEP && consecutive(), "smaller segments again: %d kept, %zu B", g_nseg, g_seg_bytes);
    /* playlist */
    char pl[1024];
    pthread_mutex_lock(&g_lock);
    int n = playlist_locked(pl, sizeof pl);
    uint32_t want_first = g_seg[g_nseg > HLS_LIST ? g_nseg - HLS_LIST : 0].seq;
    pthread_mutex_unlock(&g_lock);
    char ms[64]; snprintf(ms, sizeof ms, "#EXT-X-MEDIA-SEQUENCE:%u\n", want_first);
    int nseg = 0;
    for (const char *p = pl; (p = strstr(p, "\nseg")); p++) nseg++;
    static const char head[] = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:3\n";
    CHECK(n > 0 && !strncmp(pl, head, sizeof head - 1) && strstr(pl, ms) &&
          strstr(pl, "#EXT-X-INDEPENDENT-SEGMENTS\n") && strstr(pl, "#EXT-X-MAP:URI=\"init.mp4\"\n") &&
          strstr(pl, "#EXTINF:2.000,\n") && nseg == (g_nseg < HLS_LIST ? g_nseg : HLS_LIST), "playlist:\n%s", pl);
    pub(7, 1000, 1500, 0);                           /* a segment cut without a keyframe */
    pthread_mutex_lock(&g_lock); playlist_locked(pl, sizeof pl); pthread_mutex_unlock(&g_lock);
    CHECK(!strstr(pl, "INDEPENDENT") && strstr(pl, "#EXTINF:1.500,\n"), "no INDEPENDENT-SEGMENTS with a non-key segment listed");
    /* lost segments keep the numbering, stale sessions publish nothing */
    uint32_t next = g_next_seq; int cnt = g_nseg;
    wbuf_t bad = { 0 }; bad.err = 1;
    publish_seg(7, &bad, 180000, 1);
    CHECK(g_next_seq == next && g_nseg == cnt && g_dropped == 1, "failed segment dropped without a number");
    pub(6, 5000, 2000, 1);
    CHECK(g_next_seq == next && g_nseg == cnt, "old session cannot publish");
    /* idle: the producer's check ends the session and frees the ring */
    pthread_mutex_lock(&g_lock); g_last_req = g_now; pthread_mutex_unlock(&g_lock);
    CHECK(alive(7), "alive while requested");
    g_now += HLS_IDLE_S + 1;
    CHECK(!alive(7) && !g_producing && g_nseg == 0 && g_seg_bytes == 0, "idle %d s: session ended, ring freed", HLS_IDLE_S + 1);
    char js[512];
    hls_status_json(js, sizeof js);
    CHECK(strstr(js, "\"running\":false") && strstr(js, "\"segments\":0"), "status %s", js);
}

static void test_cookie(void)
{
    wbuf_t b = { 0 }, *target = NULL;
    cookie_io_functions_t io;
    memset(&io, 0, sizeof io);
    io.write = ck_write;
    FILE *f = fopencookie(&target, "w", io);
    setvbuf(f, NULL, _IONBF, 0);
    CHECK(fwrite("dropped", 1, 7, f) == 7, "write with no target is dropped");
    target = &b;
    static uint8_t big[300000];
    memset(big, 7, sizeof big);
    CHECK(fwrite(big, 1, sizeof big, f) == sizeof big && b.n == sizeof big && b.cap % HLS_GROW == 0 && !b.err,
          "300000 B appended, capacity %zu", b.cap);
    CHECK(ftell(f) < 0, "ftell fails (fmp4_close then skips its header patch)");
    target = NULL;
    fclose(f);
    free(b.d);
}

int main(void)
{
    test_paths();
    test_tokens();
    test_ring();
    test_cookie();
    pthread_mutex_lock(&g_lock); free_ring_locked(); pthread_mutex_unlock(&g_lock);
    printf("test_hls_unit: %d/%d passed\n", total - fails, total);
    return fails;
}
