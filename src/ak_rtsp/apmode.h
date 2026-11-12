#pragma once
/* Minimal OPEN access point over nl80211 (no hostapd, no libnl), for
 * initial Wi-Fi setup only. Caller must have stopped wpa_supplicant/udhcpc
 * and is responsible for the IP address and DHCP server.
 * All received frames go through apframe.c's bounds-checked parsers. */

/* Starts the AP (blocking setup, then a background thread answers
 * probe/auth/assoc). Returns 0 on success, negative errno-style on failure
 * (the interface is put back into station mode on failure). */
int  apmode_start(const char *ifname, const char *ssid, int channel);
/* Stops the AP and restores station iftype. Safe to call when not running. */
void apmode_stop(void);
int  apmode_running(void);
int  apmode_station_count(void);
/* Short human-readable diagnostic of the last step (for logs/UI). */
const char *apmode_last_error(void);

/* Driver capabilities relevant to AP mode, as text. Non-disruptive. */
#include <stddef.h>
int apmode_caps(const char *ifname, char *out, size_t cap);
