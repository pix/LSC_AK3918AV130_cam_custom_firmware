/* Hardware stand-ins for the host build (tests/host/build_host.sh): the video
 * input, encoder, ISP, IR cut, NPU, second stream, overlay and motion /
 * tamper detectors only exist on the camera. They keep plausible state so
 * the web page, ONVIF, MQTT and settings code behave as on the camera. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "ae.h"
#include "night.h"

/* ---- encoder (rtsp.c drain_venc_ring is not used: host_server pushes frames to live.c) ---- */
int g_chn0_fd = -1;
void *g_venc_virt;
uint32_t g_venc_phys;
int  venc_gop_now(void) { return 30; }
int  venc_kbps_now(void) { return 2048; }
const char *venc_note(void) { return ""; }
void venc_request_idr(void) {}
int  venc_smart_now(void) { return 0; }
void venc_startup_ok(void) {}

/* ---- ISP / AE / image extras ---- */
static pthread_mutex_t g_l = PTHREAD_MUTEX_INITIALIZER;
static ae_tuning_t g_ae = { 10, 20, 5, 2, 1 };
static night_tuning_t g_nt = { 3000, 1200, 3, 10000, NIGHT_MODE_AUTO };
void ae_get_tuning(ae_tuning_t *o) { pthread_mutex_lock(&g_l); *o = g_ae; pthread_mutex_unlock(&g_l); }
int  ae_set_tuning(const ae_tuning_t *i) { pthread_mutex_lock(&g_l); g_ae = *i; pthread_mutex_unlock(&g_l); return 0; }
void ae_tnr_confirm(void) {}
void imgx_apply_all(void) {}
void imgx_set_privacy(int on) { (void)on; }
int  isp_ae_debug_json(char *o, size_t c) { return snprintf(o, c, "{\"stub\":true}"); }
int  isp_debug_get_hex(const char *m, size_t len, char *o, size_t c) { (void)m; (void)len; return snprintf(o, c, "00"); }
int  isp_set_brightness(int v) { return v < -100 || v > 100 ? -1 : 0; }
int  isp_set_contrast(int v) { return v < -100 || v > 100 ? -1 : 0; }
int  isp_set_saturation(int v) { return v < -100 || v > 100 ? -1 : 0; }
int  isp_set_sharpness(int v) { return v < -100 || v > 100 ? -1 : 0; }
int  isp_set_flip_mirror(int f, int m) { (void)f; (void)m; return 0; }
static int g_ir;
void ircut_set_ir_mode(int m) { g_ir = m; }
int  ircut_get_ir_mode(void) { return g_ir; }

/* ---- day / night ---- */
void night_get_tuning(night_tuning_t *o) { pthread_mutex_lock(&g_l); *o = g_nt; pthread_mutex_unlock(&g_l); }
void night_set_tuning(const night_tuning_t *i) { pthread_mutex_lock(&g_l); g_nt = *i; pthread_mutex_unlock(&g_l); }
int  night_is_night(void) { return g_nt.override == NIGHT_MODE_FORCE_NIGHT; }
int  night_ev(void) { return 5000; }
int  night_fps_now(void) { return 15; }
int  night_light_pct(void) { return 60; }
int  night_non_ir_ratio(void) { return 40; }

/* ---- detectors, NPU, overlay, second stream ---- */
int  md_hw_raw_json(char *o, size_t c) { return snprintf(o, c, "{\"stub\":true}"); }
int  md_status_json(char *o, size_t c) { return snprintf(o, c, "{\"motion\":false,\"source\":\"stub\"}"); }
int  tamper_status_json(char *o, size_t c) { return snprintf(o, c, "{\"enabled\":false}"); }
int  npu_create_async(void) { return -1; }
int  npu_test(void) { return -1; }
void osd_hw_clear_guard(void) {}
const char *osd_hw_state(void) { return "stub"; }
int  substream_running(void) { return 0; }
int  substream_status_json(char *o, size_t c) { return snprintf(o, c, "{\"running\":false}"); }
int  substream_next(uint32_t *cur, uint8_t *buf, size_t cap, int *key, uint64_t *ts, int timeout_ms)
{ (void)cur; (void)buf; (void)cap; (void)key; (void)ts; (void)timeout_ms; return -1; }
int  qrwifi_start(int s, const char *w) { (void)s; (void)w; return 0; }

/* ---- video input: a synthetic 640x360 NV12 picture (gradient + moving bar) ---- */
static int g_tick;
static void fill(uint8_t *d, int w, int h)
{
    int bar = (g_tick++ * 7) % w;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) d[y * w + x] = (uint8_t)((x + y) / 4 + (x / 8 == bar / 8 ? 120 : 0));
    memset(d + w * h, 128, (size_t)(w * h / 2));
}
int vi_copy_sub_frame(uint8_t *dst, size_t cap, int *w, int *h)
{
    if (cap < 640 * 360 * 3 / 2) return -1;
    fill(dst, 640, 360);
    *w = 640; *h = 360;
    return 640 * 360 * 3 / 2;
}
int vi_copy_td_luma(uint8_t *dst, size_t cap, int *w, int *h)
{
    if (cap < 640 * 360) return -1;
    for (int y = 0; y < 360; y++)                  /* luminance only, as on the camera */
        for (int x = 0; x < 640; x++) dst[y * 640 + x] = (uint8_t)((x * 3 + y) / 4);
    *w = 640; *h = 360;
    return 640 * 360;
}
