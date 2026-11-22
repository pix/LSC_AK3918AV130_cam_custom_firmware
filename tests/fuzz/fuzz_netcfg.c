/* Fuzz harness: netcfg.c text parsing of wpa_cli output and the station
 * config writer.
 *   wpa_cli status        -> wpa_status() via net_status_json() / sta_connected()
 *   wpa_cli scan_results  -> net_scan_json() (SSIDs come from any AP in range)
 *   wpa_cli all_sta       -> ap_has_clients()
 *   net_connect() checks + write_sta_conf() (wpa_supplicant.conf) with fuzzed SSID/PSK
 * NETCFG_SIM only simulates status/all_sta (scan_results reads /dev/null and
 * net_scan_json sleeps 4 s), so this harness builds netcfg.c without it and
 * replaces popen/system/sleep with in-memory versions instead.
 * Input: status \0 scan_results \0 all_sta \0 ssid \0 psk [\0 cap u16]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "system.h"
#include "monotime.h"
#include "qrwifi.h"
#include "netcfg.h"

static const char *g_txt[3];
static size_t g_len[3];
static FILE *fz_popen(const char *cmd)
{
    int k = strstr(cmd, " status") ? 0 : strstr(cmd, " scan_results") ? 1 : strstr(cmd, " all_sta") ? 2 : -1;
    if (k < 0 || !g_len[k]) return fopen("/dev/null", "r");
    return fmemopen((void *)g_txt[k], g_len[k], "r");
}
#undef NETCFG_SIM
#define popen(c, m)        fz_popen(c)
#define pclose(f)          fclose(f)
#define system(c)          ((void)(c), 0)
#define sleep(s)           ((void)(s), 0u)
#define sys_cloexec_all()  ((void)0)
int qrwifi_seconds_left(void) { return 0; }   /* qrwifi.c is built without the camera part */
#include "netcfg.c"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    char *b = malloc(n + 1);
    memcpy(b, d, n);
    b[n] = 0;
    const char *part[6] = { 0 };
    size_t plen[6] = { 0 };
    const char *p = b, *end = b + n;
    for (int i = 0; i < 6; i++) {
        if (p > end) { part[i] = end; plen[i] = 0; continue; }
        const char *z = memchr(p, 0, (size_t)(end - p));
        if (!z) z = end;
        part[i] = p; plen[i] = (size_t)(z - p);
        p = z + 1;
    }
    /* each wpa_cli output as its own exact-size heap block */
    char *txt[3];
    for (int i = 0; i < 3; i++) { txt[i] = malloc(plen[i] ? plen[i] : 1); memcpy(txt[i], part[i], plen[i]); g_txt[i] = txt[i]; g_len[i] = plen[i]; }
    size_t cap = 6144;
    if (plen[5] >= 2) cap = ((unsigned char)part[5][0] | (unsigned char)part[5][1] << 8) % 6144 + 1;   /* web.c h_json: 6144; 0 is never passed */
    char *out = malloc(cap ? cap : 1);
    g_mode = MODE_STA;
    net_scan_json(out, cap);
    net_status_json(out, cap);
    (void)sta_connected();
    (void)ap_has_clients();
    g_mode = MODE_AP;
    net_status_json(out, cap);
    g_mode = MODE_STA;
    /* station config as net_connect() would let it through */
    char ssid[33], psk[64];
    snprintf(ssid, sizeof ssid, "%s", part[3]);
    snprintf(psk, sizeof psk, "%s", part[4]);
    size_t sl = strlen(ssid), pl = strlen(psk);
    int ok = sl >= 1 && sl <= 32 && (!pl || (pl >= 8 && pl <= 63));
    for (size_t i = 0; i < pl; i++) if ((unsigned char)psk[i] < 0x20 || psk[i] == 0x7f) ok = 0;
    if (ok) write_sta_conf(TMP_NEW_CONF, ssid, psk);
    set_last("Could not join \"%s\": %s", part[3], part[4]);
    net_status_json(out, cap);
    free(out);
    for (int i = 0; i < 3; i++) free(txt[i]);
    free(b);
    return 0;
}
