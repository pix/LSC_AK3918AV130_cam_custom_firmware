#include "qrwifi.h"
#include "third_party/quirc/quirc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#ifndef QRWIFI_NO_CAMERA
#include "netcfg.h"
#include "system.h"
#include "vi.h"
#include "monotime.h"
#endif

/* ---- WIFI: payload parser (untrusted) ------------------------------------- */
/* Copies one escaped field value starting at *pp until an unescaped ';'.
 * Returns length or -1 on overflow / unterminated. */
static int take_value(const char **pp, const char *end, char *out, size_t cap)
{
    const char *p = *pp;
    size_t n = 0;
    while (p < end && *p != ';') {
        char c = *p++;
        if (c == '\\') {
            if (p >= end) return -1;
            c = *p++;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = c;
    }
    if (p >= end) return -1;            /* field must end with ';' */
    out[n] = 0;
    *pp = p + 1;
    return (int)n;
}

static void unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = 0;
    }
}

int qrwifi_parse(const char *payload, size_t len, char ssid[33], char psk[64], const char **err)
{
    char t[16] = "", s[80] = "", pw[96] = "", tmp[96];
    int have_s = 0;
    *err = "not a Wi-Fi QR code";
    if (!payload || len < 5 || len > 512 || memcmp(payload, "WIFI:", 5) != 0) return -1;
    const char *p = payload + 5, *end = payload + len;
    while (p < end && *p != ';') {
        if (end - p < 2 || p[1] != ':') { *err = "malformed Wi-Fi QR code"; return -1; }
        char key = p[0];
        p += 2;
        if (take_value(&p, end, tmp, sizeof(tmp)) < 0) { *err = "malformed or too long field"; return -1; }
        switch (key) {
        case 'T': snprintf(t, sizeof(t), "%s", tmp); break;
        case 'S': snprintf(s, sizeof(s), "%s", tmp); have_s = 1; break;
        case 'P': snprintf(pw, sizeof(pw), "%s", tmp); break;
        default: break;                         /* H:, R:, I:, etc. ignored */
        }
    }
    if (!have_s) { *err = "QR code has no network name"; return -1; }
    unquote(s);
    unquote(pw);
    size_t sl = strlen(s), pl = strlen(pw);
    if (sl < 1 || sl > 32) { *err = "network name must be 1-32 bytes"; return -1; }
    if (!strcmp(t, "WEP")) { *err = "WEP networks are not supported"; return -1; }
    if (!strcmp(t, "SAE") || !strcmp(t, "WPA3")) { *err = "WPA3-only networks are not supported by the camera"; return -1; }
    int open = !t[0] || !strcmp(t, "nopass");
    if (open) pl = 0, pw[0] = 0;
    else if (pl < 8 || pl > 63) { *err = "Wi-Fi password must be 8-63 characters"; return -1; }
    memcpy(ssid, s, sl + 1);
    memcpy(psk, pw, pl + 1);
    return 0;
}

int qrwifi_parse_file(const char *text, char ssid[33], char psk[64], const char **err)
{
    char s[80] = "", pw[96] = "";
    int have_s = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char line[160];
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        line[strcspn(line, "\r")] = 0;
        if (!strncmp(line, "ssid=", 5)) { snprintf(s, sizeof(s), "%s", line + 5); have_s = 1; }
        else if (!strncmp(line, "password=", 9)) snprintf(pw, sizeof(pw), "%s", line + 9);
        p = eol ? eol + 1 : p + n;
    }
    if (!have_s || !s[0] || strlen(s) > 32) { *err = "wifi.txt needs ssid=<1-32 bytes>"; return -1; }
    size_t pl = strlen(pw);
    if (pl && (pl < 8 || pl > 63)) { *err = "wifi.txt password must be 8-63 characters (or empty)"; return -1; }
    snprintf(ssid, 33, "%s", s);
    snprintf(psk, 64, "%s", pw);
    return 0;
}

/* ---- QR detection --------------------------------------------------------- */
static struct quirc *g_q;
static int g_qw, g_qh;

int qrwifi_scan_gray(const uint8_t *y, int w, int h, char ssid[33], char psk[64])
{
    if (!g_q) { g_q = quirc_new(); if (!g_q) return 0; }
    if (w != g_qw || h != g_qh) {
        if (quirc_resize(g_q, w, h) < 0) return 0;
        g_qw = w; g_qh = h;
    }
    int qw, qh;
    uint8_t *img = quirc_begin(g_q, &qw, &qh);
    if (qw != w || qh != h) return 0;
    memcpy(img, y, (size_t)w * (size_t)h);
    quirc_end(g_q);
    int n = quirc_count(g_q);
    for (int i = 0; i < n; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(g_q, i, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
            quirc_flip(&code);                  /* mirrored view */
            if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
        }
        const char *err;
        if (data.payload_len > 0 &&
            qrwifi_parse((const char *)data.payload, (size_t)data.payload_len, ssid, psk, &err) == 0)
            return 1;
        if (data.payload_len >= 5 && !memcmp(data.payload, "WIFI:", 5))
            printf("[qr] Wi-Fi QR code rejected: %s\n", err);
    }
    return 0;
}

#ifndef QRWIFI_NO_CAMERA
/* ---- scanning thread (camera only) ---------------------------------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_until;
static int g_running;

int qrwifi_seconds_left(void)
{
    pthread_mutex_lock(&g_lock);
    long left = g_running ? (long)g_until - (long)mono_s() : 0;
    pthread_mutex_unlock(&g_lock);
    return left > 0 ? (int)left : 0;
}

void qrwifi_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_until = 0;
    pthread_mutex_unlock(&g_lock);
}

static void *qr_thread(void *arg)
{
    (void)arg;
    static uint8_t frame[640 * 368 * 3 / 2];
    int led_before = sys_led_get(), blink = 0, w, h;
    char last_ssid[33] = "";
    uint32_t last_try = 0;
    printf("[qr] looking for a Wi-Fi QR code\n");
    while (mono_s() < g_until) {
        sys_led_set(blink ^= 1);
        /* alternate: 3x zoomed centre (sharper modules) and the full view */
        int got = (blink && vi_td_zoom_active())
                ? vi_copy_td_luma(frame, sizeof(frame), &w, &h)
                : vi_copy_sub_frame(frame, sizeof(frame), &w, &h);
        if (got > 0) {
            char ssid[33], psk[64];
            if (qrwifi_scan_gray(frame, w, h, ssid, psk)) {
                /* don't hammer the same network while a previous try runs */
                if (strcmp(ssid, last_ssid) != 0 || mono_s() - last_try > 60) {
                    const char *err;
                    printf("[qr] Wi-Fi QR code for \"%s\" found\n", ssid);
                    if (net_connect(ssid, psk, &err) == 0) {
                        snprintf(last_ssid, sizeof(last_ssid), "%s", ssid);
                        last_try = mono_s();
                        memset(psk, 0, sizeof(psk));
                        break;          /* the connect thread reports the result */
                    }
                    printf("[qr] cannot start connect: %s\n", err);
                }
                memset(psk, 0, sizeof(psk));
            }
        }
        usleep(400000);
    }
    sys_led_set(led_before);
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    printf("[qr] stopped looking for QR codes\n");
    return NULL;
}

int qrwifi_start(int seconds, const char *why)
{
    if (seconds < 10) seconds = 10;
    if (seconds > 900) seconds = 900;
    pthread_mutex_lock(&g_lock);
    g_until = mono_s() + (uint32_t)seconds;
    if (g_running) { pthread_mutex_unlock(&g_lock); return 0; }   /* just extend */
    g_running = 1;
    pthread_mutex_unlock(&g_lock);
    printf("[qr] start (%d s, %s)\n", seconds, why);
    pthread_t t;
    if (pthread_create(&t, NULL, qr_thread, NULL) != 0) {
        pthread_mutex_lock(&g_lock); g_running = 0; pthread_mutex_unlock(&g_lock);
        return -1;
    }
    pthread_detach(t);
    return 0;
}
#endif
