#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "md5.h"
#include "jpeg_enc.h"

static int fails;
static void chk(const char *name, const char *got, const char *want) {
    int ok = !strcmp(got, want);
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) { printf("  got  %s\n  want %s\n", got, want); fails++; }
}

int main(int argc, char **argv) {
    struct { const char *in, *out; } v[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"},
        {"Mufasa:testrealm@host.com:Circle Of Life", "939e7578ed9e3c518a452acee763bce9"},
    };
    char hex[33];
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        md5_hex(v[i].in, strlen(v[i].in), hex);
        chk(v[i].in[0] ? v[i].in : "(empty)", hex, v[i].out);
    }
    md5_ctx c; uint8_t d[16];
    md5_init(&c);
    md5_update(&c, "The quick brown ", 16);
    md5_update(&c, "fox jumps over the lazy", 23);
    md5_update(&c, " dog", 4);
    md5_final(&c, d); md5_to_hex(d, hex);
    chk("multi-chunk", hex, "9e107d9d372bb6826bd81d3542a419d6");

    if (argc > 2) {
        FILE *f = fopen(argv[1], "rb");
        static uint8_t nv12[640 * 360 * 3 / 2];
        if (!f || fread(nv12, 1, sizeof nv12, f) != sizeof nv12) { puts("FAIL read nv12"); return 1; }
        fclose(f);
        static uint8_t out[256 * 1024];
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int n = jpeg_encode_nv12(nv12, 640, 360, 80, out, sizeof out);
        int n2 = jpeg_encode_nv12(nv12, 640, 360, 80, out, sizeof out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        (void)n;
        if (n2 <= 0) { puts("FAIL encode"); fails++; }
        else {
            printf("PASS encode %d bytes, %.1f ms/frame (host)\n", n2,
                   ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 2);
            f = fopen(argv[2], "wb"); fwrite(out, 1, n2, f); fclose(f);
        }
        /* small buffer must fail without overflow */
        uint8_t *small = malloc(1000 + 16);
        memset(small + 1000, 0xA5, 16);
        int r = jpeg_encode_nv12(nv12, 640, 360, 80, small, 1000);
        int canary_ok = 1;
        for (int i = 0; i < 16; i++) if (small[1000 + i] != 0xA5) canary_ok = 0;
        printf("%s small-buffer (ret=%d canary=%s)\n", (r == -1 && canary_ok) ? "PASS" : "FAIL", r, canary_ok ? "intact" : "SMASHED");
        if (!(r == -1 && canary_ok)) fails++;
        free(small);
        printf("%s bad-dims\n", jpeg_encode_nv12(nv12, 641, 360, 80, out, sizeof out) == -1 ? "PASS" : "FAIL");
    }
    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails != 0;
}
