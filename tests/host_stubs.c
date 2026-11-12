/* Host test stubs for camera hardware used by control.c / web.c. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "ae.h"
#include "night.h"
#include "isp.h"

static ae_tuning_t    g_ae = { 10, 20, 5, 2, 1 };
static night_tuning_t g_nt = { 3000, 1200, 3, 10000, NIGHT_MODE_AUTO };
void ae_get_tuning(ae_tuning_t *o) { *o = g_ae; }
int  ae_set_tuning(const ae_tuning_t *i) { g_ae = *i; return 0; }
void night_get_tuning(night_tuning_t *o) { *o = g_nt; }
void night_set_tuning(const night_tuning_t *i) { g_nt = *i; }
int  night_is_night(void) { return g_nt.override == NIGHT_MODE_FORCE_NIGHT; }
int isp_set_saturation(int v) { (void)v; return 0; }
int isp_set_contrast(int v)   { (void)v; return 0; }
int isp_set_brightness(int v) { (void)v; return 0; }
int isp_set_sharpness(int v)  { (void)v; return 0; }

static const char *g_nv12_path;
void stub_set_frame(const char *p) { g_nv12_path = p; }
int vi_copy_sub_frame(uint8_t *dst, size_t cap, int *w, int *h)
{
    FILE *f = fopen(g_nv12_path, "rb");
    if (!f || cap < 345600) { if (f) fclose(f); return -1; }
    size_t n = fread(dst, 1, 345600, f);
    fclose(f);
    *w = 640; *h = 360;
    return n == 345600 ? (int)n : -1;
}
static int g_fm[2];
int isp_set_flip_mirror(int flip, int mirror) { g_fm[0] = flip; g_fm[1] = mirror; return 0; }
/* ircut + netcfg stubs: the real netcfg runs killall wpa_supplicant etc. —
 * it must NEVER run on the development PC. */
static int g_irm;
void ircut_set_ir_mode(int m) { g_irm = m; }
int  ircut_get_ir_mode(void) { return g_irm; }
static int g_fb = 1;
void net_set_fallback(int on) { g_fb = on; }
int  net_get_fallback(void) { return g_fb; }
int  net_connect(const char *s, const char *p, const char **err) { (void)s; (void)p; *err = "stub"; return 0; }
int  net_ap_start(int m, const char *w) { (void)m; (void)w; return 0; }
int  net_ap_set_password(const char *p, const char **err) { *err = "too short"; return strlen(p) >= 8 ? 0 : -1; }
int  net_status_json(char *o, size_t c) { return snprintf(o, c, "{\"mode\":\"sta\",\"ssid\":\"stub\",\"ip\":\"10.0.0.2\",\"mac\":\"00:11:22:33:44:55\",\"rssi_dbm\":-55,\"ap_ssid\":\"LSC-Camera-4455\",\"ap_seconds_left\":0,\"fallback\":true,\"state\":\"COMPLETED\",\"busy\":false,\"last\":\"\"}"); }
int  net_scan_json(char *o, size_t c) { return snprintf(o, c, "[{\"ssid\":\"Home <b>x</b>\",\"signal\":-50,\"secured\":true}]"); }
int  rtsp_client_connected(void) { return 0; }
static int g_qr;
int qrwifi_start(int s, const char *w) { (void)w; g_qr = s; return 0; }
int qrwifi_seconds_left(void) { return g_qr; }
int vi_copy_td_luma(uint8_t *d, size_t c, int *w, int *h) { (void)d; (void)c; (void)w; (void)h; return -1; }
int vi_td_zoom_active(void) { return 0; }

/* burned-in overlay (hardware only) */
const char *osd_hw_state(void) { return "error"; }
void osd_hw_clear_guard(void) {}
