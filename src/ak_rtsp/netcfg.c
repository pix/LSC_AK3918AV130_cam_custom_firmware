#include "netcfg.h"
#include "system.h"
#include "monotime.h"
#include "qrwifi.h"

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

#define IFACE         "wlan0"
/* AP mode is parked: the camera's atbm Wi-Fi driver lacks mgmt_tx/AP_SME,
 * so phones can't associate (see research notes). Code kept for later. */
#ifndef NET_ENABLE_AP
#define NET_ENABLE_AP 0
#endif
#ifndef NET_TMP
#define NET_TMP "/tmp"
#endif
#define TMP_NEW_CONF  NET_TMP "/akr_wpa_new.conf"
#define AP_CONF       NET_TMP "/akr_ap.conf"
#define UDHCPD_CONF   NET_TMP "/akr_udhcpd.conf"
#ifndef CONNECT_TIMEOUT_S
#define CONNECT_TIMEOUT_S 45
#endif
#ifndef BOOT_GRACE_S
#define BOOT_GRACE_S 30
#endif
#ifndef AP_MINUTE_S
#define AP_MINUTE_S 60   /* tests shrink this */
#endif
#ifndef AP_EXTEND_S
#define AP_EXTEND_S 120
#endif
#ifndef AP_UP_TIMEOUT_S
#define AP_UP_TIMEOUT_S 15
#endif

/* NETCFG_SIM (host tests only): shell commands are appended to
 * NET_TMP/sim_cmds.log instead of being executed, and wpa_cli / interface
 * state is read from NET_TMP/sim_wpa_status, sim_all_sta, sim_ip. */
#ifdef NETCFG_SIM
int qrwifi_seconds_left(void) { return 0; }
#include <stdarg.h>
static int sim_system(const char *cmd)
{
    FILE *f = fopen(NET_TMP "/sim_cmds.log", "a");
    if (f) { fprintf(f, "%s\n", cmd); fclose(f); }
    return 0;
}
static FILE *sim_popen(const char *cmd, const char *mode)
{
    (void)mode;
    if (strstr(cmd, " status"))  return fopen(NET_TMP "/sim_wpa_status", "r");
    if (strstr(cmd, " all_sta")) return fopen(NET_TMP "/sim_all_sta", "r");
    return fopen("/dev/null", "r");
}
#define system(c)    sim_system(c)
#define popen(c, m)  sim_popen(c, m)
#define pclose(f)    fclose(f)
#endif

#ifndef FALLBACK_AFTER_S
#define FALLBACK_AFTER_S 300
#endif   /* no Wi-Fi for 5 min -> AP */
#ifndef FALLBACK_AP_MIN
#define FALLBACK_AP_MIN 5
#endif     /* AP window before retrying station mode */

enum { MODE_STA = 0, MODE_AP = 1 };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int      g_mode = MODE_STA;
static int      g_busy;                 /* connect attempt or mode switch running */
static uint32_t g_ap_until;             /* mono_s() deadline in AP mode */
static int      g_fallback = NET_ENABLE_AP;
static char     g_last[160] = "";       /* last connect/AP result for the UI */

/* ---- small helpers ----------------------------------------------------------- */
static void run(const char *cmd)
{
    sys_cloexec_all();                       /* udhcpc & co. must not keep our devices */
    if (system(cmd) == -1) printf("[net] system() failed: %s\n", cmd);
}

/* popen without leaking our descriptors into the child */
static FILE *popen_clean(const char *cmd, const char *mode)
{
    sys_cloexec_all();
    return popen(cmd, mode);
}

static void set_last(const char *fmt, const char *a, const char *b)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_last, sizeof(g_last), fmt, a ? a : "", b ? b : "");
    pthread_mutex_unlock(&g_lock);
    printf("[net] %s\n", g_last);
}

static int iface_ipv4(char *out, size_t cap)
{
#ifdef NETCFG_SIM
    FILE *sf = fopen(NET_TMP "/sim_ip", "r");
    char b[20] = "";
    if (sf) { if (!fgets(b, sizeof(b), sf)) b[0] = 0; fclose(sf); }
    b[strcspn(b, "\n")] = 0;
    if (!b[0]) return 0;
    if (out) snprintf(out, cap, "%s", b);
    return 1;
#endif
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), IFACE);
    int ok = ioctl(s, SIOCGIFADDR, &ifr) == 0;
    close(s);
    if (!ok) return 0;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    if (sin->sin_addr.s_addr == 0) return 0;
    if (out) inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)cap);
    return 1;
}

static void iface_mac(char *out, size_t cap)
{
    FILE *f = fopen("/sys/class/net/" IFACE "/address", "r");
    snprintf(out, cap, "00:00:00:00:00:00");
    if (f) { if (!fgets(out, (int)cap, f)) {} fclose(f); }
    out[strcspn(out, "\n")] = 0;
}

/* wpa_cli status -> selected keys. Returns 1 if wpa_cli answered. */
typedef struct { char state[24], ssid[64], mode[16], ip[20]; } wpa_status_t;

static int wpa_status(wpa_status_t *st)
{
    memset(st, 0, sizeof(*st));
    FILE *p = popen_clean("wpa_cli -i " IFACE " status 2>/dev/null", "r");
    if (!p) return 0;
    char line[160];
    int any = 0;
    while (fgets(line, sizeof(line), p)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        any = 1;
        if      (!strcmp(line, "wpa_state"))  snprintf(st->state, sizeof(st->state), "%s", v);
        else if (!strcmp(line, "ssid"))       snprintf(st->ssid, sizeof(st->ssid), "%s", v);
        else if (!strcmp(line, "mode"))       snprintf(st->mode, sizeof(st->mode), "%s", v);
        else if (!strcmp(line, "ip_address")) snprintf(st->ip, sizeof(st->ip), "%s", v);
    }
    pclose(p);
    return any;
}

/* Signal level in dBm from /proc/net/wireless (0 if unavailable). */
static int wifi_rssi(void)
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return 0;
    char line[256];
    int rssi = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, IFACE ":");
        if (!p) continue;
        int status; float link, level;
        if (sscanf(p + strlen(IFACE) + 1, "%x %f %f", &status, &link, &level) == 3)
            rssi = (int)level;
    }
    fclose(f);
    return rssi;
}

int net_rssi(void) { return wifi_rssi(); }

static int sta_connected(void)
{
    wpa_status_t st;
    if (wpa_status(&st) && !strcmp(st.state, "COMPLETED") && strcmp(st.mode, "AP") != 0)
        return 1;
    return g_mode == MODE_STA && iface_ipv4(NULL, 0);
}

static void dhcp_hostname(char *out, size_t cap)
{
    /* Same hostname the decloud wrapper uses (udhcpc -x hostname:NAME). */
    snprintf(out, cap, "lsc-camera");
    FILE *f = fopen("/usr/sbin/ak_rtsp_wrapper.sh", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *h = strstr(line, "hostname:");
        if (!h) continue;
        h += 9;
        size_t n = 0;
        while (n + 1 < cap && (isalnum((unsigned char)h[n]) || h[n] == '-' || h[n] == '.')) n++;
        if (n) { memcpy(out, h, n); out[n] = 0; }
        break;
    }
    fclose(f);
}

/* (Re)start station mode with the given config file. */
static void sta_start(const char *conf)
{
    char host[64], cmd[256];
    dhcp_hostname(host, sizeof(host));
    run("killall udhcpd udhcpc wpa_supplicant >/dev/null 2>&1");
    usleep(500000);
    run("ifconfig " IFACE " 0.0.0.0 up");
    snprintf(cmd, sizeof(cmd), "wpa_supplicant -B -Dnl80211 -i " IFACE " -c %s >/dev/null 2>&1", conf);
    run(cmd);
    snprintf(cmd, sizeof(cmd), "udhcpc -i " IFACE " -x hostname:%s >/dev/null 2>&1 &", host);
    run(cmd);
}

/* Write a station config. SSID as hex (no escaping problems); passphrase
 * quoted (wpa_supplicant takes everything up to the LAST quote). */
static int write_sta_conf(const char *path, const char *ssid, const char *psk)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -1; }
    fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\nnetwork={\n\tssid=");
    for (const unsigned char *c = (const unsigned char *)ssid; *c; c++) fprintf(f, "%02x", *c);
    fprintf(f, "\n\tscan_ssid=1\n");
    if (psk[0]) fprintf(f, "\tpsk=\"%s\"\n\tkey_mgmt=WPA-PSK\n", psk);
    else        fprintf(f, "\tkey_mgmt=NONE\n");
    fprintf(f, "}\n");
    fflush(f);
    int ok = fsync(fileno(f)) == 0;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static int copy_file_atomic(const char *src, const char *dst)
{
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    FILE *in = fopen(src, "r");
    if (!in) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { fclose(in); return -1; }
    char buf[512];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (write(fd, buf, n) != (ssize_t)n) ok = 0;
    fclose(in);
    if (fsync(fd) != 0) ok = 0;
    close(fd);
    if (!ok || rename(tmp, dst) != 0) { unlink(tmp); return -1; }
    sync();
    return 0;
}

/* ---- station connect with rollback ----------------------------------------- */
static int ap_bring_up(void);

typedef struct { char ssid[33], psk[64]; } connect_req_t;

static void *connect_thread(void *arg)
{
    connect_req_t req = *(connect_req_t *)arg;
    free(arg);

    set_last("Trying network \"%s\"...%s", req.ssid, "");
    pthread_mutex_lock(&g_lock);
    int was_ap = g_mode == MODE_AP;
    pthread_mutex_unlock(&g_lock);
    int ok = 0, stage = 0;   /* 0 not found, 1 associating, 2 key handshake, 3 connected */
    char ip[20] = "";
    if (write_sta_conf(TMP_NEW_CONF, req.ssid, req.psk) == 0) {
        pthread_mutex_lock(&g_lock);
        g_mode = MODE_STA;
        pthread_mutex_unlock(&g_lock);
        sta_start(TMP_NEW_CONF);
        for (int i = 0; i < CONNECT_TIMEOUT_S && !ok; i++) {   /* associate + DHCP */
            sleep(1);
            wpa_status_t st;
            if (!wpa_status(&st)) continue;
            /* furthest stage reached, to explain a failure */
            if (!strcmp(st.state, "COMPLETED")) stage = 3;
            else if ((!strcmp(st.state, "4WAY_HANDSHAKE") || !strcmp(st.state, "GROUP_HANDSHAKE")) && stage < 2) stage = 2;
            else if ((!strcmp(st.state, "ASSOCIATING") || !strcmp(st.state, "ASSOCIATED") ||
                      !strcmp(st.state, "AUTHENTICATING")) && stage < 1) stage = 1;
            ok = !strcmp(st.state, "COMPLETED") && iface_ipv4(ip, sizeof(ip));
        }
    }
    if (ok && copy_file_atomic(TMP_NEW_CONF, NET_STA_CONF) == 0) {
        set_last("Connected to \"%s\", address %s (saved)", req.ssid, ip);
    } else if (was_ap && ap_bring_up() == 0) {
        /* configured from the camera's own AP: go back there, not offline */
        pthread_mutex_lock(&g_lock);
        g_mode = MODE_AP;
        g_ap_until = mono_s() + 10 * AP_MINUTE_S;
        pthread_mutex_unlock(&g_lock);
        set_last("Could not join \"%s\". %s", req.ssid,
                 "Back on the camera's access point, nothing saved.");
    } else {
        sta_start(NET_STA_CONF);   /* rollback */
        static const char *why[] = {
            "network not found (name wrong or out of range)",
            "network found but it did not accept the camera",
            "wrong password (the key handshake failed)",
            "joined but got no address from the router (DHCP)",
        };
        char msg[200];
        snprintf(msg, sizeof(msg), "%s. Previous network restored, nothing saved.", why[stage]);
        set_last("Could not join \"%s\": %s", req.ssid, msg);
    }
    unlink(TMP_NEW_CONF);
    memset(&req, 0, sizeof(req));
    pthread_mutex_lock(&g_lock);
    g_busy = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int net_connect(const char *ssid, const char *psk, const char **err)
{
    size_t sl = strlen(ssid), pl = strlen(psk);
    if (sl < 1 || sl > 32) { *err = "SSID must be 1-32 bytes"; return -1; }
    if (pl && (pl < 8 || pl > 63)) { *err = "password must be 8-63 characters (or empty for an open network)"; return -1; }
    for (size_t i = 0; i < pl; i++)
        if ((unsigned char)psk[i] < 0x20 || psk[i] == 0x7f) { *err = "password has invalid characters"; return -1; }

    pthread_mutex_lock(&g_lock);
    if (g_busy) { pthread_mutex_unlock(&g_lock); *err = "another network change is in progress"; return -1; }
    g_busy = 1;
    pthread_mutex_unlock(&g_lock);

    connect_req_t *req = calloc(1, sizeof(*req));
    if (!req) { pthread_mutex_lock(&g_lock); g_busy = 0; pthread_mutex_unlock(&g_lock); *err = "out of memory"; return -1; }
    snprintf(req->ssid, sizeof(req->ssid), "%s", ssid);
    snprintf(req->psk, sizeof(req->psk), "%s", psk);
    pthread_t t;
    if (pthread_create(&t, NULL, connect_thread, req) != 0) {
        free(req); pthread_mutex_lock(&g_lock); g_busy = 0; pthread_mutex_unlock(&g_lock); *err = "cannot start thread"; return -1;
    }
    pthread_detach(t);
    return 0;
}

/* ---- access point -------------------------------------------------------------- */
static void ap_password(char *out, size_t cap)
{
    snprintf(out, cap, NET_AP_DEFAULT_PASS);
    FILE *f = fopen(NET_AP_FILE, "r");
    if (!f) return;
    char line[80];
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) >= 8 && strlen(line) <= 63) snprintf(out, cap, "%s", line);
    }
    fclose(f);
}

int net_ap_set_password(const char *pass, const char **err)
{
    size_t n = strlen(pass);
    if (n < 8 || n > 63) { *err = "AP password must be 8-63 characters"; return -1; }
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)pass[i] < 0x20 || pass[i] == 0x7f) { *err = "invalid characters"; return -1; }
    char tmp[] = NET_AP_FILE ".tmp";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { *err = "cannot write"; return -1; }
    int ok = write(fd, pass, n) == (ssize_t)n && write(fd, "\n", 1) == 1 && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, NET_AP_FILE) != 0) { unlink(tmp); *err = "cannot save"; return -1; }
    return 0;
}

static void ap_ssid(char *out, size_t cap)
{
    char mac[24];
    iface_mac(mac, sizeof(mac));
    size_t l = strlen(mac);
    char a = l >= 5 ? mac[l - 5] : '0', b = l >= 4 ? mac[l - 4] : '0';
    char c = l >= 2 ? mac[l - 2] : '0', d = l >= 1 ? mac[l - 1] : '0';
    snprintf(out, cap, "LSC-Camera-%c%c%c%c", toupper((unsigned char)a), toupper((unsigned char)b),
             toupper((unsigned char)c), toupper((unsigned char)d));
}

static int ap_bring_up(void)
{
    char ssid[40], pass[80];
    ap_ssid(ssid, sizeof(ssid));
    ap_password(pass, sizeof(pass));

    FILE *f = fopen(AP_CONF, "w");
    if (!f) return -1;
    fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\nap_scan=2\nnetwork={\n"
               "\tssid=\"%s\"\n\tmode=2\n\tfrequency=2437\n\tkey_mgmt=WPA-PSK\n"
               "\tproto=RSN\n\tpairwise=CCMP\n\tgroup=CCMP\n\tpsk=\"%s\"\n}\n", ssid, pass);
    fclose(f);
    chmod(AP_CONF, 0600);
    f = fopen(UDHCPD_CONF, "w");
    if (!f) return -1;
    fprintf(f, "start 192.168.4.10\nend 192.168.4.60\ninterface " IFACE "\n"
               "lease_file /tmp/akr_udhcpd.leases\noption subnet 255.255.255.0\n"
               "option router " NET_AP_IP "\noption lease 3600\n");
    fclose(f);

    run("killall udhcpc udhcpd wpa_supplicant >/dev/null 2>&1");
    usleep(500000);
    run("ifconfig " IFACE " " NET_AP_IP " netmask 255.255.255.0 up");
    run("wpa_supplicant -B -Dnl80211 -i " IFACE " -c " AP_CONF " >/dev/null 2>&1");
    run("touch /tmp/akr_udhcpd.leases; udhcpd -S " UDHCPD_CONF " >/dev/null 2>&1");

    for (int i = 0; i < AP_UP_TIMEOUT_S; i++) {   /* verify the AP really came up */
        sleep(1);
        wpa_status_t st;
        if (wpa_status(&st) && !strcmp(st.mode, "AP") && !strcmp(st.state, "COMPLETED")) {
            printf("[net] AP \"%s\" up at " NET_AP_IP "\n", ssid);
            return 0;
        }
    }
    return -1;
}

typedef struct { int minutes; char why[48]; } ap_req_t;

static void *ap_thread(void *arg)
{
    ap_req_t req = *(ap_req_t *)arg;
    free(arg);
    if (ap_bring_up() == 0) {
        pthread_mutex_lock(&g_lock);
        g_mode = MODE_AP;
        g_ap_until = mono_s() + (uint32_t)req.minutes * AP_MINUTE_S;
        pthread_mutex_unlock(&g_lock);
        char m[16];
        snprintf(m, sizeof(m), "%d", req.minutes);
        set_last("Access point active for %s min (%s)", m, req.why);
    } else {
        set_last("Access point could not start (%s); %s", req.why, "back to normal Wi-Fi");
        sta_start(NET_STA_CONF);
    }
    pthread_mutex_lock(&g_lock);
    g_busy = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int net_ap_start(int minutes, const char *why)
{
    if (!NET_ENABLE_AP) {
        set_last("Access point not supported by this camera's Wi-Fi driver (%s)%s", why, "");
        return -1;
    }
    if (minutes < 1) minutes = 1;
    if (minutes > 60) minutes = 60;
    pthread_mutex_lock(&g_lock);
    if (g_busy) { pthread_mutex_unlock(&g_lock); return -1; }
    if (g_mode == MODE_AP) {               /* already AP: just extend */
        g_ap_until = mono_s() + (uint32_t)minutes * AP_MINUTE_S;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_busy = 1;
    pthread_mutex_unlock(&g_lock);
    ap_req_t *req = calloc(1, sizeof(*req));
    if (!req) { g_busy = 0; return -1; }
    req->minutes = minutes;
    snprintf(req->why, sizeof(req->why), "%s", why);
    pthread_t t;
    if (pthread_create(&t, NULL, ap_thread, req) != 0) { free(req); g_busy = 0; return -1; }
    pthread_detach(t);
    return 0;
}

static int ap_has_clients(void)
{
    FILE *p = popen_clean("wpa_cli -i " IFACE " all_sta 2>/dev/null", "r");
    if (!p) return 0;
    char line[128];
    int n = 0;
    while (fgets(line, sizeof(line), p))
        if (strlen(line) >= 17 && line[2] == ':' && line[5] == ':') n++;
    pclose(p);
    return n;
}

void net_set_fallback(int on) { g_fallback = (on && NET_ENABLE_AP) ? 1 : 0; }
int  net_get_fallback(void)   { return g_fallback; }

/* ---- monitor ------------------------------------------------------------------- */
static void *monitor_thread(void *arg)
{
    (void)arg;
    uint32_t last_ok = mono_s();
    sleep(BOOT_GRACE_S);   /* boot: the stock wrapper brings Wi-Fi up first */
    for (;;) {
        sleep(5);
        pthread_mutex_lock(&g_lock);
        int busy = g_busy, mode = g_mode;
        uint32_t until = g_ap_until;
        pthread_mutex_unlock(&g_lock);
        if (busy) { last_ok = mono_s(); continue; }

        if (mode == MODE_AP) {
            if (mono_s() < until) continue;
            if (ap_has_clients()) {          /* someone is configuring: extend */
                pthread_mutex_lock(&g_lock);
                g_ap_until = mono_s() + AP_EXTEND_S;
                pthread_mutex_unlock(&g_lock);
                continue;
            }
            pthread_mutex_lock(&g_lock);
            g_mode = MODE_STA;
            pthread_mutex_unlock(&g_lock);
            set_last("Access point timed out, %s%s", "back to normal Wi-Fi", "");
            sta_start(NET_STA_CONF);
            last_ok = mono_s();
            continue;
        }

        if (sta_connected()) { last_ok = mono_s(); continue; }
        if (g_fallback && mono_s() - last_ok >= FALLBACK_AFTER_S) {
            printf("[net] no Wi-Fi for %d s -> fallback AP\n", FALLBACK_AFTER_S);
            net_ap_start(FALLBACK_AP_MIN, "Wi-Fi unavailable");
            last_ok = mono_s();
        }
    }
    return NULL;
}

void net_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, monitor_thread, NULL) == 0) pthread_detach(t);
}

/* ---- JSON for the web page --------------------------------------------------- */
static void json_str(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    if (cap) out[n++] = '"';
    for (const unsigned char *c = (const unsigned char *)s; *c && n + 7 < cap; c++) {
        if (*c == '"' || *c == '\\') { out[n++] = '\\'; out[n++] = (char)*c; }
        else if (*c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *c);
        else out[n++] = (char)*c;
    }
    if (n + 2 <= cap) { out[n++] = '"'; out[n] = 0; } else if (cap) out[cap - 1] = 0;
}

int net_status_json(char *out, size_t cap)
{
    wpa_status_t st;
    int have = wpa_status(&st);
    char ip[20] = "", mac[24], apssid[40], j_ssid[140], j_last[340], j_ap[90];
    iface_ipv4(ip, sizeof(ip));
    iface_mac(mac, sizeof(mac));
    ap_ssid(apssid, sizeof(apssid));
    pthread_mutex_lock(&g_lock);
    int mode = g_mode, busy = g_busy;
    long left = mode == MODE_AP ? (long)g_ap_until - (long)mono_s() : 0;
    json_str(j_last, sizeof(j_last), g_last);
    pthread_mutex_unlock(&g_lock);
    json_str(j_ssid, sizeof(j_ssid), have ? st.ssid : "");
    json_str(j_ap, sizeof(j_ap), apssid);
    return snprintf(out, cap,
        "{\"qr_seconds_left\":%d,\"ap_supported\":%s,\"mode\":\"%s\",\"busy\":%s,\"state\":\"%s\",\"ssid\":%s,\"ip\":\"%s\",\"mac\":\"%s\","
        "\"rssi_dbm\":%d,\"ap_ssid\":%s,\"ap_seconds_left\":%ld,\"fallback\":%s,\"last\":%s}",
        qrwifi_seconds_left(), NET_ENABLE_AP ? "true" : "false", mode == MODE_AP ? "ap" : "sta", busy ? "true" : "false",
        have ? st.state : "unknown", j_ssid, ip, mac, wifi_rssi(), j_ap,
        left > 0 ? left : 0, g_fallback ? "true" : "false", j_last);
}

int net_scan_json(char *out, size_t cap)
{
    if (g_mode == MODE_AP) return snprintf(out, cap, "[]");
    run("wpa_cli -i " IFACE " scan >/dev/null 2>&1");
    sleep(4);
    FILE *p = popen_clean("wpa_cli -i " IFACE " scan_results 2>/dev/null", "r");
    size_t n = (size_t)snprintf(out, cap, "[");
    int first = 1;
    if (p) {
        char line[256];
        while (fgets(line, sizeof(line), p)) {
            /* bssid \t freq \t signal \t flags \t ssid */
            char *f[5] = {0};
            char *save = NULL, *tok = strtok_r(line, "\t\r\n", &save);
            int k = 0;
            while (tok && k < 5) { f[k++] = tok; tok = strtok_r(NULL, "\t\r\n", &save); }
            if (k < 5 || strlen(f[0]) != 17) continue;
            char js[140];
            json_str(js, sizeof(js), f[4]);
            int secured = strstr(f[3], "WPA") || strstr(f[3], "WEP");
            if (n + 200 >= cap) break;
            n += (size_t)snprintf(out + n, cap - n, "%s{\"ssid\":%s,\"signal\":%d,\"secured\":%s}",
                                  first ? "" : ",", js, atoi(f[2]), secured ? "true" : "false");
            first = 0;
        }
        pclose(p);
    }
    n += (size_t)snprintf(out + n, cap - n, "]");
    return (int)n;
}

void net_hostname(char *out, size_t cap) { dhcp_hostname(out, cap); }
