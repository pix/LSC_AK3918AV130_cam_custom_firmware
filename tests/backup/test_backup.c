/* Host test for backup.c (round trip + hostile archives). Build and run:
 *   d=$(mktemp -d); mkdir -p $d/cfg
 *   cc -fsanitize=address,undefined -DCFG_DIR="\"$d/cfg\"" -I src/ak_rtsp \
 *      tests/backup/test_backup.c src/ak_rtsp/backup.c -o $d/t && $d/t $d/cfg
 */
#include "backup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *dir;
static int fails;
#define CHECK(c, what) do { if (c) printf("PASS %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

static void wr(const char *name, const char *s)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); fputs(s, f); fclose(f);
}
static char *rd(const char *name)
{
    static char b[4096]; char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "r"); if (!f) return NULL;
    size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b;
}
/* minimal tar entry writer for hostile archives */
static size_t entry(uint8_t *o, const char *name, char type, const char *data)
{
    size_t n = strlen(data);
    memset(o, 0, 512);
    snprintf((char *)o, 100, "%s", name);
    memcpy(o + 100, "0000600", 8); memcpy(o + 108, "0000000", 8); memcpy(o + 116, "0000000", 8);
    snprintf((char *)o + 124, 12, "%011o", (unsigned)n);
    snprintf((char *)o + 136, 12, "%011o", 0u);
    memset(o + 148, ' ', 8); o[156] = (uint8_t)type;
    memcpy(o + 257, "ustar", 6); memcpy(o + 263, "00", 2);
    unsigned s = 0; for (int i = 0; i < 512; i++) s += o[i];
    snprintf((char *)o + 148, 8, "%06o", s); o[155] = ' ';
    memcpy(o + 512, data, n);
    return 512 + (n + 511) / 512 * 512;
}
static int restore_of(uint8_t *t, size_t len, char *err)
{
    int n = 0; return backup_restore(t, len, &n, err, 120);
}

int main(int argc, char **argv)
{
    dir = argv[1];
    const char *auth = "admin\n0123456789abcdef0123456789abcdef\n";
    wr("ak_rtsp_settings", "night.mode auto\n");
    wr("ak_rtsp_ext", "mqtt.pass\tsecret\n");
    wr("ak_rtsp_auth", auth);
    wr("wpa_supplicant.conf", "network={}\n");
    wr("ak_rtsp_lastcrash", "not to be backed up");

    uint8_t *t; size_t tl; char err[120];
    CHECK(backup_build(&t, &tl, 0, "cam", "test") == 0, "build backup");
    CHECK(memmem(t, tl, "wpa_supplicant", 14) == NULL, "Wi-Fi excluded unless asked");
    CHECK(memmem(t, tl, "ak_rtsp_lastcrash", 17) == NULL, "non-whitelisted file not included");
    uint8_t *tw; size_t twl;
    CHECK(backup_build(&tw, &twl, 1, "cam", "test") == 0 && memmem(tw, twl, "wpa_supplicant", 14), "Wi-Fi included when asked");
    free(tw);

    wr("ak_rtsp_settings", "changed\n");
    wr("ak_rtsp_ext", "changed\n");
    int n = 0;
    CHECK(backup_restore(t, tl, &n, err, sizeof err) == 0 && n == 3, "restore round trip");
    CHECK(!strcmp(rd("ak_rtsp_settings"), "night.mode auto\n") && !strcmp(rd("ak_rtsp_ext"), "mqtt.pass\tsecret\n"), "contents restored");
    struct stat st; char p[512]; snprintf(p, sizeof p, "%s/ak_rtsp_auth", dir); stat(p, &st);
    CHECK((st.st_mode & 0777) == 0600, "secret file mode 0600");
    free(t);

    static uint8_t h[64 * 1024];
    size_t o;
    const char *man = "ak_rtsp backup 1\ndevice=x\n";
    /* no manifest */
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_settings", '0', "x\n");
    CHECK(restore_of(h, o + 1024, err) != 0, "rejected: no manifest");
    /* path traversal */
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_backup.txt", '0', man); o += entry(h + o, "../../tmp/evil", '0', "x");
    CHECK(restore_of(h, o + 1024, err) != 0 && access("/tmp/evil", F_OK) != 0, "rejected: ../ path");
    /* absolute path / unknown name */
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_backup.txt", '0', man); o += entry(h + o, "/etc/passwd", '0', "x");
    CHECK(restore_of(h, o + 1024, err) != 0, "rejected: absolute path");
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_backup.txt", '0', man); o += entry(h + o, "ak_rtsp_settings/x", '0', "x");
    CHECK(restore_of(h, o + 1024, err) != 0, "rejected: subdirectory name");
    /* symlink entry with a whitelisted name */
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_backup.txt", '0', man); o += entry(h + o, "ak_rtsp_settings", '2', "");
    CHECK(restore_of(h, o + 1024, err) != 0, "rejected: symlink entry");
    /* malformed login file (would fall back to admin/admin) */
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_backup.txt", '0', man); o += entry(h + o, "ak_rtsp_auth", '0', "admin\nnothex\n");
    CHECK(restore_of(h, o + 1024, err) != 0 && !strcmp(rd("ak_rtsp_auth"), auth), "rejected: bad login file, old one kept");
    /* all-or-nothing: valid file then invalid one -> nothing written */
    wr("ak_rtsp_settings", "before\n");
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_backup.txt", '0', man); o += entry(h + o, "ak_rtsp_settings", '0', "after\n");
    o += entry(h + o, "bogus", '0', "x");
    CHECK(restore_of(h, o + 1024, err) != 0 && !strcmp(rd("ak_rtsp_settings"), "before\n"), "atomic: nothing written when any entry is bad");
    /* bad checksum */
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_backup.txt", '0', man); h[0] ^= 1;
    CHECK(restore_of(h, o + 1024, err) != 0, "rejected: bad header checksum");
    /* truncated: an entry declares 5000 bytes but the archive ends long before */
    memset(h, 0, sizeof h); o = entry(h, "ak_rtsp_backup.txt", '0', man);
    size_t e2 = o; o += entry(h + o, "ak_rtsp_settings", '0', "abc");
    uint8_t *hh = h + e2;
    snprintf((char *)hh + 124, 12, "%011o", 5000u);
    memset(hh + 148, ' ', 8);
    unsigned s2 = 0; for (int i = 0; i < 512; i++) s2 += hh[i];
    snprintf((char *)hh + 148, 8, "%06o", s2); hh[155] = ' ';
    CHECK(restore_of(h, o + 1024, err) != 0, "rejected: entry larger than the archive");
    /* random garbage */
    for (size_t i = 0; i < sizeof h; i++) h[i] = (uint8_t)(rand() & 0xff);
    CHECK(restore_of(h, sizeof h, err) != 0, "rejected: random data");
    printf(fails ? "FAILURES: %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
