#pragma once
/* Wi-Fi setup from a QR code shown to the camera (the standard
 * "WIFI:T:WPA;S:<ssid>;P:<password>;;" format produced by Android/iOS
 * "Share Wi-Fi"), and from a wifi.txt file on the SD card.
 * Joining uses net_connect(), i.e. the same rollback as the web page. */
#include <stddef.h>
#include <stdint.h>

/* Parse a WIFI: payload (untrusted). Returns 0 and fills ssid/psk, or -1
 * with *err set. psk is "" for open networks. */
int qrwifi_parse(const char *payload, size_t len, char ssid[33], char psk[64], const char **err);

/* Find and decode QR codes in an 8-bit luminance image. Calls the parser on
 * each WIFI: payload; returns 1 and fills ssid/psk on the first valid one. */
int qrwifi_scan_gray(const uint8_t *y, int w, int h, char ssid[33], char psk[64]);

/* Start/stop looking for a Wi-Fi QR code (status LED blinks meanwhile). */
int  qrwifi_start(int seconds, const char *why);
void qrwifi_stop(void);
int  qrwifi_seconds_left(void);

/* Parse "ssid=..." / "password=..." lines of an SD-card wifi.txt. */
int qrwifi_parse_file(const char *text, char ssid[33], char psk[64], const char **err);
