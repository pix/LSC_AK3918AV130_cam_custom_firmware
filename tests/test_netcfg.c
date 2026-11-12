/* Host simulation test for netcfg.c (built with -DNETCFG_SIM: no command is
 * executed, Wi-Fi state comes from files). Verifies the safety rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "netcfg.h"

/* system.c is not linked: in simulation no command runs, so there are no
 * descriptors to mark close-on-exec. */
void sys_cloexec_all(void) {}

static int fails;
#define CHECK(c, name) do { if (c) printf("PASS %s\n", name); else { printf("FAIL %s\n", name); fails++; } } while (0)

static void put(const char *f, const char *v) { char p[256]; snprintf(p, sizeof p, NET_TMP "/%s", f); FILE *x = fopen(p, "w"); if (x) { fputs(v, x); fclose(x); } }
static int file_has(const char *path, const char *needle) { char b[8192] = ""; FILE *f = fopen(path, "r"); if (!f) return 0; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return strstr(b, needle) != NULL; }
static int log_has(const char *n) { return file_has(NET_TMP "/sim_cmds.log", n); }
static void clear_log(void) { put("sim_cmds.log", ""); }
static void status(char *o, size_t c) { net_status_json(o, c); }
static int wait_idle(int secs) { char b[2048]; for (int i = 0; i < secs * 10; i++) { status(b, sizeof b); if (strstr(b, "\"busy\":false")) return 1; usleep(100000); } return 0; }
static int wait_mode(const char *m, int secs) { char b[2048], want[32]; snprintf(want, sizeof want, "\"mode\":\"%s\"", m); for (int i = 0; i < secs * 10; i++) { status(b, sizeof b); if (strstr(b, want) && strstr(b, "\"busy\":false")) return 1; usleep(100000); } return 0; }

/* Plays the role of wpa_supplicant: when an AP config is started, report AP up
 * (unless ap_broken); when a station config is started, report per sta_ok. */
static volatile int sta_ok, ap_broken, stop_sim;
static const char *volatile fail_state = "wpa_state=SCANNING\n";
static void *fake_wpa(void *a)
{
    (void)a; long seen = 0;
    while (!stop_sim) {
        FILE *f = fopen(NET_TMP "/sim_cmds.log", "r");
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f);
            if (sz < seen) seen = 0;
            fseek(f, seen, SEEK_SET);
            char line[512];
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "wpa_supplicant -B") && strstr(line, "akr_ap.conf")) {
                    if (!ap_broken) { put("sim_wpa_status", "wpa_state=COMPLETED\nmode=AP\nssid=LSC-Camera\n"); put("sim_ip", "192.168.4.1"); }
                    else { put("sim_wpa_status", "wpa_state=DISCONNECTED\n"); put("sim_ip", ""); }
                } else if (strstr(line, "wpa_supplicant -B")) {
                    if (sta_ok) { put("sim_wpa_status", "wpa_state=COMPLETED\nmode=station\nssid=Net\n"); put("sim_ip", "10.1.2.3"); }
                    else { put("sim_wpa_status", fail_state); put("sim_ip", ""); }
                }
            }
            seen = ftell(f); fclose(f);
        }
        usleep(50000);
    }
    return NULL;
}

int main(void)
{
    const char *err;
    put(NET_STA_CONF_NAME, "ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n\tssid=\"OldNet\"\n\tpsk=\"oldpassword\"\n}\n");
    put("sim_all_sta", ""); clear_log();
    sta_ok = 1; put("sim_wpa_status", "wpa_state=COMPLETED\nmode=station\nssid=OldNet\n"); put("sim_ip", "10.1.2.3");
    pthread_t t; pthread_create(&t, NULL, fake_wpa, NULL);

    /* validation */
    CHECK(net_connect("", "password1", &err) == -1, "empty SSID rejected");
    CHECK(net_connect("123456789012345678901234567890123", "password1", &err) == -1, "33-byte SSID rejected");
    CHECK(net_connect("Net", "short", &err) == -1, "7-char password rejected");
    CHECK(net_connect("Net", "pass\nword1", &err) == -1, "newline in password rejected");
    CHECK(net_ap_set_password("short", &err) == -1, "short AP password rejected");

    /* 1. successful connect -> saved */
    clear_log(); sta_ok = 1;
    CHECK(net_connect("New \"Net\"", "pa\"ss word1", &err) == 0, "connect started");
    CHECK(net_connect("Other", "password2", &err) == -1, "second concurrent connect refused");
    CHECK(wait_idle(10), "connect finished");
    CHECK(file_has(NET_STA_CONF, "ssid=4e657720224e657422"), "saved config has hex SSID");
    CHECK(file_has(NET_STA_CONF, "psk=\"pa\"ss word1\""), "saved config has passphrase");
    { char b[2048]; status(b, sizeof b); CHECK(strstr(b, "Connected to") != NULL, "status reports connected"); }

    /* 2. failing connect -> rollback, saved config untouched */
    char before[4096] = ""; { FILE *f = fopen(NET_STA_CONF, "r"); size_t n = fread(before, 1, sizeof before - 1, f); before[n] = 0; fclose(f); }
    clear_log(); sta_ok = 0;
    CHECK(net_connect("BadNet", "wrongpassword", &err) == 0, "bad connect started");
    sleep(1); sta_ok = 1;   /* the rollback (old config) works */
    CHECK(wait_idle(15), "bad connect finished");
    char after[4096] = ""; { FILE *f = fopen(NET_STA_CONF, "r"); size_t n = fread(after, 1, sizeof after - 1, f); after[n] = 0; fclose(f); }
    CHECK(!strcmp(before, after), "saved config unchanged after failure");
    CHECK(log_has("-c " NET_STA_CONF), "rollback restarted the saved config");
    { char b[2048]; status(b, sizeof b); CHECK(strstr(b, "Previous network restored") != NULL, "status reports rollback"); }
    { char b[2048]; status(b, sizeof b); CHECK(strstr(b, "network not found") != NULL, "failure explained: not found"); }

    /* 2b. wrong password: handshake reached but never completed */
    clear_log(); sta_ok = 0; fail_state = "wpa_state=4WAY_HANDSHAKE\nssid=Home\n";
    CHECK(net_connect("Home", "wrongpassword", &err) == 0, "wrong-password connect started");
    sleep(1); sta_ok = 1;
    CHECK(wait_idle(15), "wrong-password connect finished");
    { char b[2048]; status(b, sizeof b); CHECK(strstr(b, "wrong password") != NULL, "failure explained: wrong password"); }
    fail_state = "wpa_state=SCANNING\n";

    /* 3. manual AP start, then timeout -> back to station */
    clear_log(); sta_ok = 1; ap_broken = 0;
    net_start();   /* monitor (BOOT_GRACE_S=0) */
    CHECK(net_ap_start(1, "test") == 0, "AP start accepted");
    CHECK(wait_mode("ap", 10), "AP mode active");
    CHECK(file_has(NET_TMP "/akr_ap.conf", "mode=2"), "AP config written");
    CHECK(log_has("udhcpd -S"), "DHCP server started for AP");
    CHECK(wait_mode("sta", 20), "AP timed out back to station");

    /* 4. AP extended while a client is connected */
    clear_log();
    CHECK(net_ap_start(1, "test") == 0, "AP start again");
    CHECK(wait_mode("ap", 10), "AP active again");
    put("sim_all_sta", "aa:bb:cc:dd:ee:ff\n");
    sleep(AP_MINUTE_S + 7);
    { char b[2048]; status(b, sizeof b); CHECK(strstr(b, "\"mode\":\"ap\"") != NULL, "AP kept while a client is connected"); }
    put("sim_all_sta", "");
    CHECK(wait_mode("sta", 25), "AP ends after client leaves");

    /* 5. AP that fails to come up -> back to station */
    clear_log(); ap_broken = 1;
    CHECK(net_ap_start(1, "test") == 0, "broken AP start accepted");
    sleep(1);
    CHECK(wait_mode("sta", 15), "broken AP reverted to station");
    CHECK(log_has("-c " NET_STA_CONF), "station config restarted after broken AP");
    ap_broken = 0;

    /* 6. connect attempted from AP mode fails -> back to AP, not offline */
    CHECK(net_ap_start(5, "test") == 0, "AP for setup");
    CHECK(wait_mode("ap", 10), "AP active for setup");
    sta_ok = 0; clear_log();
    CHECK(net_connect("Typo", "wrongpassword", &err) == 0, "connect from AP started");
    CHECK(wait_idle(15) && wait_mode("ap", 5), "failed connect from AP returns to AP");
    sta_ok = 1;

    /* 7. automatic fallback after Wi-Fi loss */
    net_ap_start(1, "x"); /* extend existing AP to 1 min then let it lapse */
    CHECK(wait_mode("sta", 30), "back to station before fallback test");
    sta_ok = 0; put("sim_wpa_status", "wpa_state=DISCONNECTED\n"); put("sim_ip", "");
    CHECK(wait_mode("ap", 20), "fallback AP after Wi-Fi loss");
    net_set_fallback(0);
    stop_sim = 1; pthread_join(t, NULL);
    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails != 0;
}
