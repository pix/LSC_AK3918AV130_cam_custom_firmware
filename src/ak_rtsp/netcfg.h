#pragma once
/* Wi-Fi station settings with rollback, and access-point (AP) mode.
 *
 * Safety rules (see TESTING.md):
 *  - The saved station config (/etc/config/wpa_supplicant.conf) is only
 *    replaced after the new network associated AND got an IPv4 address;
 *    otherwise the previous config is restored automatically.
 *  - AP mode is always time-limited and returns to station mode.
 *  - "Connected" = wpa_state COMPLETED or wlan0 has an IPv4 address, so a
 *    wpa_cli hiccup alone never triggers the AP fallback. */
#include <stddef.h>

#ifndef NET_STA_CONF
#define NET_STA_CONF   "/etc/config/wpa_supplicant.conf"
#endif
#ifndef NET_AP_FILE
#define NET_AP_FILE    "/etc/config/ak_rtsp_ap"   /* AP password (0600) */
#endif
#define NET_AP_DEFAULT_PASS "lsccamera"
#define NET_AP_IP      "192.168.4.1"

void net_start(void);                 /* monitor thread (AP fallback) */

/* Async: returns 0 if the attempt was started, -1 with *err otherwise. */
int  net_connect(const char *ssid, const char *psk, const char **err);
int  net_ap_start(int minutes, const char *why);   /* 0 ok, -1 busy/failed */
int  net_ap_set_password(const char *pass, const char **err);
void net_set_fallback(int on);
int  net_get_fallback(void);

int  net_status_json(char *out, size_t cap);
int  net_scan_json(char *out, size_t cap);    /* blocks ~4 s */

/* Wi-Fi signal in dBm (0 if unknown). */
int  net_rssi(void);
/* DHCP host name the camera announces (e.g. "camera-1"). */
void net_hostname(char *out, size_t cap);
