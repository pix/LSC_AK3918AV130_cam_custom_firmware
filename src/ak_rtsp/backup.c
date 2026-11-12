/* Settings backup / restore.
 *
 * Backup: an uncompressed tar of an explicit whitelist of files on the CONFIG
 * partition (/etc/config) plus a manifest. It holds secrets: the login
 * hash, the ONVIF password copy, the MQTT password, TLS private keys and
 * optionally the Wi-Fi password, so it is only produced for the logged-in
 * admin (POST + X-Requested-With, see web.c).
 *
 * Restore: only a tar with our manifest is accepted; each entry must be one
 * of the whitelisted names exactly (no paths, so nothing outside the list
 * can be written), a regular file, at most 64 KB. The login file is checked
 * strictly (a malformed one would otherwise fall back to admin/admin).
 * Files are written atomically (tmp + fsync + rename); the caller reboots so
 * every module reloads its settings. */
#include "backup.h"
#include "system.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef CFG_DIR
#define CFG_DIR   "/etc/config"          /* overridden by the host test */
#endif
#define MANIFEST  "ak_rtsp_backup.txt"
#define MAX_FILE  (64 * 1024)
#define MAX_TOTAL (768 * 1024)

/* name, secret (0600) or public (0644), Wi-Fi only */
static const struct { const char *name; int secret; int wifi; } k_files[] = {
    { "ak_rtsp_settings", 1, 0 },        /* image / day-night / time / LED ...      */
    { "ak_rtsp_ext", 1, 0 },             /* module settings (incl. MQTT password)   */
    { "ak_rtsp_auth", 1, 0 },            /* login: user + Digest HA1                 */
    { "ak_rtsp_onvif_pw", 1, 0 },        /* ONVIF WS-Security password copy         */
    { "ak_rtsp_hook", 1, 0 },            /* webhook token                           */
    { "ak_rtsp_osd", 1, 0 },             /* overlay                                 */
    { "ak_rtsp_ap", 1, 0 },              /* fallback access point                   */
    { "ak_rtsp_tls.conf", 1, 0 },        /* HTTPS names / redirect                  */
    { "ak_rtsp_tls_ca.key", 1, 0 },      /* camera CA (keep: devices trust it)      */
    { "ak_rtsp_tls_ca.crt", 0, 0 },
    { "ak_rtsp_tls.key", 1, 0 },
    { "ak_rtsp_tls.crt", 0, 0 },
    { "ak_rtsp_tls.san", 0, 0 },
    { "ak_rtsp_mqtt_ca.crt", 0, 0 },     /* MQTT broker trust and client certificates */
    { "ak_rtsp_mqtt_pin", 0, 0 },
    { "ak_rtsp_mqtt_cam.crt", 0, 0 },
    { "ak_rtsp_mqtt_cam.key", 1, 0 },
    { "ak_rtsp_mqtt_cam.ca", 0, 0 },
    { "ak_rtsp_mqtt_client.crt", 0, 0 },
    { "ak_rtsp_mqtt_client.key", 1, 0 },
    { "wpa_supplicant.conf", 1, 1 },     /* Wi-Fi network and password (optional)   */
};
#define NFILES (sizeof k_files / sizeof k_files[0])

static int find_file(const char *name)
{
    for (size_t i = 0; i < NFILES; i++) if (!strcmp(k_files[i].name, name)) return (int)i;
    return -1;
}

/* ---- tar writing ----------------------------------------------------------- */
typedef struct { uint8_t *b; size_t len, cap; } buf_t;

static int put(buf_t *o, const void *p, size_t n)
{
    if (o->len + n > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 65536;
        while (c < o->len + n) c *= 2;
        uint8_t *nb = realloc(o->b, c);
        if (!nb) return -1;
        o->b = nb; o->cap = c;
    }
    memcpy(o->b + o->len, p, n);
    o->len += n;
    return 0;
}

static int put_entry(buf_t *o, const char *name, const void *data, size_t size, time_t mt)
{
    char h[512];
    memset(h, 0, sizeof h);
    snprintf(h, 100, "%s", name);
    memcpy(h + 100, "0000600", 8);
    memcpy(h + 108, "0000000", 8);
    memcpy(h + 116, "0000000", 8);
    snprintf(h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf(h + 136, 12, "%011llo", (unsigned long long)mt);
    memset(h + 148, ' ', 8);
    h[156] = '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)h[i];
    snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
    static const uint8_t zero[512];
    if (put(o, h, 512) || put(o, data, size)) return -1;
    size_t pad = (512 - size % 512) % 512;
    return pad ? put(o, zero, pad) : 0;
}

int backup_build(uint8_t **out, size_t *len, int with_wifi, const char *device, const char *version)
{
    buf_t o = { 0 };
    time_t now = time(NULL);
    char man[256];
    int n = snprintf(man, sizeof man, "ak_rtsp backup 1\ndevice=%s\nfirmware=%s\ntime=%lld\nwifi=%d\n",
                     device, version, (long long)now, with_wifi ? 1 : 0);
    if (put_entry(&o, MANIFEST, man, (size_t)n, now) != 0) goto fail;
    static uint8_t data[MAX_FILE];
    for (size_t i = 0; i < NFILES; i++) {
        if (k_files[i].wifi && !with_wifi) continue;
        char path[160];
        snprintf(path, sizeof path, CFG_DIR "/%s", k_files[i].name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;                       /* not every file exists on every camera */
        struct stat st;
        ssize_t r = -1;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= MAX_FILE)
            r = read(fd, data, sizeof data);
        close(fd);
        if (r < 0) continue;
        if (put_entry(&o, k_files[i].name, data, (size_t)r, st.st_mtime) != 0) goto fail;
    }
    static const uint8_t end[1024];
    if (put(&o, end, sizeof end) != 0) goto fail;
    *out = o.b; *len = o.len;
    return 0;
fail:
    free(o.b);
    return -1;
}

/* ---- restore ------------------------------------------------------------------- */
static long long octal(const char *p, size_t n)
{
    long long v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

static int header_ok(const uint8_t *h)
{
    unsigned stored = (unsigned)octal((const char *)h + 148, 8), sum = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return stored == sum;
}

/* login file: "<user>\n<32 hex>\n" with the same user rules as auth.c */
static int auth_file_ok(const uint8_t *d, size_t n)
{
    char t[128];
    if (n == 0 || n >= sizeof t) return 0;
    memcpy(t, d, n); t[n] = 0;
    char *nl = strchr(t, '\n');
    if (!nl) return 0;
    *nl = 0;
    size_t ul = strlen(t);
    if (ul < 1 || ul > 32) return 0;
    for (size_t i = 0; i < ul; i++)
        if (!(isalnum((unsigned char)t[i]) || t[i] == '.' || t[i] == '_' || t[i] == '-')) return 0;
    char *h = nl + 1;
    size_t hl = strcspn(h, "\r\n");
    if (hl != 32) return 0;
    for (size_t i = 0; i < 32; i++) if (!isxdigit((unsigned char)h[i])) return 0;
    return 1;
}

static int write_atomic(const char *name, const uint8_t *d, size_t n, int secret)
{
    char path[160], tmp[176];
    snprintf(path, sizeof path, CFG_DIR "/%s", name);
    snprintf(tmp, sizeof tmp, "%s.restore", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, secret ? 0600 : 0644);
    if (fd < 0) return -1;
    int ok = (n == 0 || write(fd, d, n) == (ssize_t)n) && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int backup_restore(const uint8_t *tar, size_t len, int *restored, char *err, size_t errcap)
{
    *restored = 0;
    if (len < 1024 || len > MAX_TOTAL) { snprintf(err, errcap, "not a camera backup (size)"); return -1; }
    /* pass 1: validate everything before writing anything */
    int seen_manifest = 0, count = 0;
    for (size_t off = 0; off + 512 <= len;) {
        const uint8_t *h = tar + off;
        int zero = 1;
        for (int i = 0; i < 512 && zero; i++) zero = h[i] == 0;
        if (zero) break;                                      /* end of archive */
        if (!header_ok(h)) { snprintf(err, errcap, "damaged archive"); return -1; }
        char name[101];
        memcpy(name, h, 100); name[100] = 0;
        long long size = octal((const char *)h + 124, 12);
        char type = (char)h[156];
        if ((type != '0' && type != 0) || size < 0 || size > MAX_FILE || off + 512 + (size_t)size > len) {
            snprintf(err, errcap, "unexpected entry %.40s", name); return -1;
        }
        const uint8_t *d = h + 512;
        if (!strcmp(name, MANIFEST)) {
            if (off != 0 || size < 17 || memcmp(d, "ak_rtsp backup 1\n", 17)) { snprintf(err, errcap, "not a camera backup"); return -1; }
            seen_manifest = 1;
        } else {
            if (!seen_manifest) { snprintf(err, errcap, "not a camera backup"); return -1; }
            if (find_file(name) < 0) { snprintf(err, errcap, "unexpected file %.40s", name); return -1; }
            if (!strcmp(name, "ak_rtsp_auth") && !auth_file_ok(d, (size_t)size)) { snprintf(err, errcap, "invalid login file"); return -1; }
            count++;
        }
        off += 512 + ((size_t)size + 511) / 512 * 512;
    }
    if (!seen_manifest || !count) { snprintf(err, errcap, "not a camera backup (empty)"); return -1; }
    /* pass 2: write */
    for (size_t off = 0; off + 512 <= len;) {
        const uint8_t *h = tar + off;
        int zero = 1;
        for (int i = 0; i < 512 && zero; i++) zero = h[i] == 0;
        if (zero) break;
        char name[101];
        memcpy(name, h, 100); name[100] = 0;
        size_t size = (size_t)octal((const char *)h + 124, 12);
        int k = find_file(name);
        if (k >= 0) {
            if (write_atomic(name, h + 512, size, k_files[k].secret) != 0) {
                snprintf(err, errcap, "cannot write %s (%s)", name, strerror(errno));
                sync();
                return -1;
            }
            (*restored)++;
        }
        off += 512 + (size + 511) / 512 * 512;
    }
    sync();
    printf("[backup] restored %d file(s)\n", *restored);
    return 0;
}
