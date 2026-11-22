/* Fuzz harness: qrwifi.c Wi-Fi setup inputs
 *   byte 0 % 3 == 0: qrwifi_parse() of a QR payload (as decoded from the image)
 *   byte 0 % 3 == 1: qrwifi_parse_file() of an SD-card wifi.txt
 *   byte 0 % 3 == 2: qrwifi_scan_gray() (quirc detect + decode + parse) of a
 *                    small grayscale image: [w u8][h u8][pixels, repeated].
 * Payload/text are exact-size heap copies so ASan catches over-reads. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "qrwifi.h"

static void check(const char *ssid, const char *psk)
{
    if (strlen(ssid) < 1 || strlen(ssid) > 32 || strlen(psk) > 63) __builtin_trap();
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 1) return 0;
    const uint8_t *p = d + 1;
    size_t len = n - 1;
    char ssid[33], psk[64];
    const char *err;
    switch (d[0] % 3) {
    case 0: {
        char *b = malloc(len ? len : 1);
        memcpy(b, p, len);
        if (qrwifi_parse(b, len, ssid, psk, &err) == 0) check(ssid, psk);
        free(b);
        break;
    }
    case 1: {
        char *b = malloc(len + 1);
        memcpy(b, p, len);
        b[len] = 0;
        if (qrwifi_parse_file(b, ssid, psk, &err) == 0) check(ssid, psk);
        free(b);
        break;
    }
    case 2: {
        if (len < 2) return 0;
        int w = p[0] + 1, h = p[1] + 1;         /* 1..256: quirc on odd / tiny frames */
        const uint8_t *px = p + 2;
        size_t pn = len - 2;
        uint8_t *img = malloc((size_t)w * h);
        for (size_t i = 0; i < (size_t)w * h; i++) img[i] = pn ? px[i % pn] : 0;
        if (qrwifi_scan_gray(img, w, h, ssid, psk)) check(ssid, psk);
        free(img);
        break;
    }
    }
    return 0;
}
