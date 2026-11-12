#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "ak_ioctls.h"
#include "isp.h"
#include "ircut.h"
#include "ae.h"
#include "night.h"
#include "control.h"
#include "monotime.h"
#include "sdn.h"
#include "ctrl_params.h"
#include "md.h"
#include "events.h"
#include "live.h"
#include <math.h>
#include <string.h>
#include <errno.h>

/* ---- scene light measurement + stock decision (sdn.c) ----------------------
 * Every poll: AE run info + exposure attr -> stock EV (bigger = darker).
 * AWB block stats -> non-infrared ratio (share of blocks lit by visible,
 * coloured light), read every poll while the stock algorithm evaluates at
 * night, else once a second.
 * night.algo: "stock" = anyka_ipc's soft photosensor (EV > 6.3M for 10
 * samples -> night; EV < 2M and non-IR ratio > 40 % for 10 samples -> day;
 * anti-flap lock after 5 switches in 2 min), "exposure" = the original
 * hw_exp thresholds below. */
#define INNER_GET_AE_RUN_INFO 0x80044952
#define INNER_GET_EXPOSURE    0x8004498b
#define INNER_GET_AWB_STAT    0x80044959
static int s_stock_algo = 1;
static int algo_get(char *o, size_t c) { snprintf(o, c, "%s", s_stock_algo ? "stock" : "exposure"); return 0; }
static int algo_set(const char *v, const char **e)
{
    if (!strcmp(v, "stock")) s_stock_algo = 1; else if (!strcmp(v, "exposure")) s_stock_algo = 0;
    else { *e = "must be stock or exposure"; return -1; }
    return 0;
}
static volatile uint32_t g_ev;
static volatile int g_nonir = -1, g_light = -1;
static sdn_params g_sdn;

/* light level 0..100 %: log scale, EV 3e4 (bright) -> 100, 3e7 (very dark) -> 0 */
static int light_pct(uint32_t ev)
{
    if (!ev) return -1;
    double p = 100.0 * (log10(3e7) - log10((double)ev)) / 3.0;
    return p < 0 ? 0 : p > 100 ? 100 : (int)(p + 0.5);
}
int night_ev(void)          { return (int)g_ev; }
int night_light_pct(void)   { return g_light; }
int night_non_ir_ratio(void){ return g_nonir; }
void night_init(void)
{
    ctrl_param_t a = { "night.algo", algo_get, algo_set, CTRL_PERSIST };
    control_register(&a);
}

/* -------------------------------------------------------------------------
 * Auto day/night mode.
 *
 * Ghidra reverse-engineering of stock anyka_ipc (night_mode_check @
 * 0x0012583c, day_mode_check @ 0x00124aa0, driven by ht_night_mode_auto_th
 * @ 0x000d5abc) found it computes a synthetic "EV" from AE state
 * (ak_vpss_get_ev(): roughly gain_r/g/b * exposure_time) and compares
 * against config-driven thresholds from _ht_hw_settings.ini [night_mode]:
 *   ps_soft_day_to_night_lum   = 6,300,000  (day->night trigger)
 *   ps_soft_night_to_day_lum   = 2,000,000  (night->day trigger)
 *   ps_soft_day/night_check_frame_num = 10  (consecutive confirming samples)
 *   ps_soft_auto_lock_time_ms  = 60,000     (anti-flap lock after a switch)
 *
 * We don't reproduce ak_vpss_get_ev()'s exact formula/units, so reusing
 * anyka_ipc's literal threshold numbers would be meaningless here. Instead
 * this uses hw_exp directly as the EV proxy (gain stayed 1.0x in every
 * ak_rtsp AE capture so far, so hw_exp is already the dominant brightness
 * driver) — thresholds below are picked relative to each frame-rate
 * config's VTS ceiling (2284 day / 3426 night) and NEED HARDWARE
 * CALIBRATION, unlike the AE dead-band tuning which had ground truth to
 * reverse from. The debounce/hysteresis/lock-timer *structure* mirrors
 * stock; the numbers are a starting point, not a confirmed match.
 *
 * Fresh isp_hook capture confirmed the coordinated switch stock performs:
 * IR-cut solenoid + WB table +
 * AK_ISP_set_frame_rate({15,2284,8,15} day <-> {10,3426,8,10} night) all
 * change together.
 * ------------------------------------------------------------------------- */

#define DAY_FPS            15
#define DAY_VTS          2284
#define NIGHT_FPS          10
#define NIGHT_VTS        3426

/* hw_exp proxy thresholds — NEEDS CALIBRATION (see header comment above). */
#define NIGHT_TRIGGER_HW_EXP   3000  /* day, hw_exp above this -> switch to night (approaching day VTS ceiling) */
#define DAY_TRIGGER_HW_EXP      1200  /* night, hw_exp below this -> switch to day */

#define CONFIRM_SAMPLES    10        /* matches stock's ps_soft_day/night_check_frame_num */
#define LOCK_MS         60000        /* matches stock's ps_soft_auto_lock_time_ms */
#define POLL_US        100000        /* 100ms, matches stock's ht_night_mode_auto_th cadence */

static volatile int g_night_running;
static int          g_night_started;      /* night_start() created the thread */
static pthread_t    g_night_tid;

/* Live-tunable thresholds + manual override, exposed for control.c. Same
 * whole-struct-copy convention as ae.c's g_tuning — see that file's comment
 * for why a mutex isn't used here. */
static night_tuning_t g_tuning = {
    .trigger_hw_exp  = NIGHT_TRIGGER_HW_EXP,
    .day_hw_exp      = DAY_TRIGGER_HW_EXP,
    .confirm_samples = CONFIRM_SAMPLES,
    .lock_ms         = LOCK_MS,
    .override        = NIGHT_MODE_AUTO,
};
/* control.c updates g_tuning from a different pthread than night_loop().
 * Protect the multi-field tuning snapshot and use a generation counter so a
 * manual DAY/NIGHT/AUTO change invalidates stale debounce/lock state. */
static pthread_mutex_t g_night_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned        g_mode_generation = 0;
static volatile int g_is_night = 0;  /* current actual state, boot = day */

static unsigned night_get_snapshot(night_tuning_t *out)
{
    unsigned generation;
    pthread_mutex_lock(&g_night_lock);
    *out = g_tuning;
    generation = g_mode_generation;
    pthread_mutex_unlock(&g_night_lock);
    return generation;
}

void night_get_tuning(night_tuning_t *out)
{
    (void)night_get_snapshot(out);
}
void night_set_tuning(const night_tuning_t *in)
{
    pthread_mutex_lock(&g_night_lock);
    if (g_tuning.override != in->override)
        ++g_mode_generation;
    g_tuning = *in;
    pthread_mutex_unlock(&g_night_lock);
}
int  night_is_night(void) { return g_is_night; }

static long long now_ms(void)
{
    /* clock_gettime(CLOCK_MONOTONIC) returns garbage on this camera (see
     * monotime.h), which silently broke the anti-flap lock timing. */
    return (long long)mono_ms();
}

static void set_frame_rate(int fps, int vts)
{
    uint32_t frame_rate[4] = { (uint32_t)fps, (uint32_t)vts, 8, (uint32_t)fps };
    isp_tunnel(ISP_INNER_SET_FRAME_RATE, frame_rate);
    uint32_t sensor_fps[4] = { (uint32_t)fps, 0, 0, (uint32_t)fps };
    isp_tunnel(ISP_INNER_SET_SENSOR_FPS, sensor_fps);

    /* SET_FRAME_RATE appears to reset the exposure attr's dead-band/speed
     * fields (exp_stable_range/exp_hold_range/exp_speed) back to their
     * sensor_isp.conf hardware defaults (4/5/16) — the same narrow/fast
     * values that caused the original bistable AE hunting before that fix.
     * Symptom without this: AE hunts normally after a night<->day switch
     * even though ae.c's tuning is unchanged, because the on-chip values
     * were silently reverted underneath it. Re-apply whatever is currently
     * configured (default or live-tuned via control.c) so it survives the
     * frame-rate reconfiguration. */
    ae_tuning_t t;
    ae_get_tuning(&t);
    ae_set_tuning(&t);
}

/* ---- adaptive frame rate (video.idle_fps) ------------------------------
 * The sensor rate can change while streaming (this file already switches
 * 15 <-> 10 fps for night), and every consumer timestamps frames, so RTSP,
 * recordings and the web view play a rate change correctly. Bitrate falls
 * almost linearly with the frame count (measured, busy day scene: 15 fps
 * 3041 kbit/s, 10 fps 1998, 5 fps 937): with no motion for 10 s and nobody
 * watching the web view, run at the idle rate; back to full rate as soon as
 * motion starts. Off by default; needs motion detection on (otherwise the
 * camera could not know when to speed up). Any rate keeps the sensor's line
 * rate: fps x VTS = 15 x 2284 = 10 x 3426 = 34260. Night's own 10 fps stays
 * the ceiling at night. */
#define IDLE_AFTER_MS 10000
static volatile int s_idle_fps;          /* 0 = off */
static int g_fps_now = DAY_FPS;
static long long g_active_ms;
void night_set_idle_fps(int fps) { s_idle_fps = (fps >= 2 && fps < DAY_FPS) ? fps : 0; }
static volatile int s_wake_all;          /* video.idle_wake: 0 zones, 1 whole picture */
void night_set_idle_wake_all(int all) { s_wake_all = all; }
int  night_fps_now(void) { return g_fps_now; }

static void adapt_fps(void)
{
    int base = g_is_night ? NIGHT_FPS : DAY_FPS;
    long long now = now_ms();
    int idle_fps = s_idle_fps;
    int motion = s_wake_all ? md_motion_any() : events_motion_active();
    int busy = !idle_fps || !md_enabled() || motion || live_viewers() > 0;
    if (busy) g_active_ms = now;
    int want = (!busy && now - g_active_ms > IDLE_AFTER_MS && idle_fps < base) ? idle_fps : base;
    if (want != g_fps_now) {
        printf("[fps] %d -> %d fps (%s)\n", g_fps_now, want, want < base ? "idle" : "activity");
        set_frame_rate(want, 34260 / want);
        g_fps_now = want;
    }
}

static void switch_to_night(void)
{
    printf("[night] switching to NIGHT mode\n");
    ircut_switch(1);
    set_frame_rate(NIGHT_FPS, NIGHT_VTS);
    g_fps_now = NIGHT_FPS;
    /* Full night ISP calibration (2026-07-05) — replaces the old
     * isp_set_wb_manual(256,256,256)-only approach. That was a reasonable
     * guess before this was found, but it actively fights the real night
     * subfile's own WB config now: isp_pipeline_init_night() reloads all 24
     * modules (WDR/NR/gamma/CCM/sharpness/WB/etc — a real, separate
     * calibration found concatenated in sensor_isp.conf, not just a WB
     * tweak), so a manual WB override afterward would silently undo part of
     * it. See isp.c's isp_pipeline_init_night() for the full discovery
     * trail. If this subfile isn't present on some other unit/firmware,
     * isp_pipeline_init_night() logs and returns -1 without sending
     * anything — falls back to whatever calibration was already active
     * rather than risk sending garbage. */
    if (isp_pipeline_init_night() != 0) {
        fprintf(stderr, "[night] falling back: night ISP calibration unavailable, "
                        "day calibration + IR-cut/frame-rate switch only\n");
    } else {
        isp_reset_picture_baselines();
        control_reapply_picture();   /* sliders, WDR, masks back on top of the new calibration */
    }
    /* The pipeline reload just re-sent module 0x14 (AE), which includes its
     * own EXPOSURE sub-block — the exact same "reset the dead-band/exp_max
     * fields back to sensor_isp.conf's raw defaults" risk set_frame_rate()'s
     * own comment already documents for SET_FRAME_RATE, now happening AFTER
     * that reapplication instead of before it. Re-apply once more so
     * whatever AE tuning is currently configured survives the full reload,
     * not just the frame-rate call. */
    {
        ae_tuning_t t;
        ae_get_tuning(&t);
        ae_set_tuning(&t);
    }
}

static void switch_to_day(void)
{
    printf("[night] switching to DAY mode\n");
    ircut_switch(0);
    set_frame_rate(DAY_FPS, DAY_VTS);
    g_fps_now = DAY_FPS;
    /* Restore the day ISP calibration (see switch_to_night()'s comment) —
     * replaces the old isp_set_wb_manual(460,256,504)-only restore. */
    if (isp_pipeline_init() != 0) {
        fprintf(stderr, "[night] day ISP calibration reload failed\n");
    } else {
        isp_reset_picture_baselines();
        control_reapply_picture();   /* sliders, WDR, masks back on top of the new calibration */
    }
    /* See switch_to_night()'s matching comment — the reload can reset AE
     * dead-band/exp_max fields, re-apply current tuning to be safe. */
    {
        ae_tuning_t t;
        ae_get_tuning(&t);
        ae_set_tuning(&t);
    }
}

static void *night_loop(void *arg)
{
    (void)arg;
    int       confirm              = 0;
    long long last_switch_ms       = 0;
    unsigned  seen_mode_generation = 0;
    int       have_mode_generation = 0;

    printf("[night] auto day/night monitor started (poll=%dms)\n", POLL_US / 1000);
    static uint8_t ri[36], ex[0x720], awb[0x24a0];
    g_sdn = SDN_DEFAULTS;
    sdn_params_sanitize(&g_sdn);
    sdn_state st;
    sdn_reset(&st, 1000 / (POLL_US / 1000));     /* one step per poll */
    int awb_tick = 0, was_stock = -1;
    long long hold_until = 0;

    while (g_night_running) {
        usleep(POLL_US);
        adapt_fps();

        night_tuning_t t;
        unsigned mode_generation = night_get_snapshot(&t);

        if (!have_mode_generation) {
            seen_mode_generation = mode_generation;
            have_mode_generation = 1;
        } else if (mode_generation != seen_mode_generation) {
            seen_mode_generation = mode_generation;
            confirm = 0;
            last_switch_ms = 0;
            printf("[night] mode changed -> reset confirm/lock\n");
        }

        /* measurements (any mode) */
        unsigned ae = 0;
        int have_ev = 0;
        if (isp_tunnel(INNER_GET_EXPOSURE, ex) == 0 && isp_tunnel(INNER_GET_AE_RUN_INFO, ri) == 0) {
            uint64_t e = sdn_ev(ri, ex);
            g_ev = e > 0xffffffffu ? 0xffffffffu : (uint32_t)e;
            g_light = light_pct(g_ev);
            ae = sdn_ae_classify(ri, ex);
            have_ev = 1;
        }
        if (++awb_tick >= 10 || (s_stock_algo && g_is_night && t.override == NIGHT_MODE_AUTO)) {
            awb_tick = 0;
            if (isp_tunnel(INNER_GET_AWB_STAT, awb) == 0) {
                int r = sdn_non_ir_ratio(awb, 1920, 1080, &g_sdn, NULL);
                g_nonir = r < 0 ? -2 : r;
            } else g_nonir = -100 - errno;
        }

        if (t.override == NIGHT_MODE_FORCE_DAY) {
            if (g_is_night) {
                switch_to_day();
                g_is_night = 0;
            }
            confirm = 0;
            last_switch_ms = 0;
            continue;
        }

        if (t.override == NIGHT_MODE_FORCE_NIGHT) {
            if (!g_is_night) {
                switch_to_night();
                g_is_night = 1;
            }
            confirm = 0;
            last_switch_ms = 0;
            continue;
        }

        long long now = now_ms();

        int stock = s_stock_algo;
        if (stock) {
            if (was_stock != 1) { sdn_reset(&st, 1000 / (POLL_US / 1000)); was_stock = 1; }
            if (!have_ev || now < hold_until) continue;
            int r = sdn_step(&st, &g_sdn, g_ev, g_nonir < 0 ? 0 : (uint32_t)g_nonir, ae, (uint64_t)now, g_is_night);
            if (r == SDN_NOT_STABLE) { hold_until = now + 200; continue; }
            if (r != SDN_DAY && r != SDN_NIGHT) continue;
            if (r == g_is_night) continue;
            printf("[night] stock algorithm: EV %u, non-IR %d%% -> %s\n", (unsigned)g_ev, g_nonir, r ? "NIGHT" : "DAY");
            if (r) switch_to_night(); else switch_to_day();
            g_is_night = r;
            hold_until = now_ms() + 3000;              /* stock: 300 ms + 2.5 s after a switch */
            continue;
        }
        was_stock = 0;

        if (last_switch_ms != 0 && t.lock_ms > 0 &&
            now - last_switch_ms < (long long)t.lock_ms) {
            confirm = 0;
            continue;
        }

        int      lum;
        uint32_t hw_exp, hw_sgain, hw_isp;
        if (ae_get_last_stats(&lum, &hw_exp, &hw_sgain, &hw_isp) != 0)
            continue;

        int desired_night = g_is_night;

        if (g_is_night) {
            if (hw_exp < t.day_hw_exp)
                desired_night = 0;
        } else {
            if (hw_exp > t.trigger_hw_exp)
                desired_night = 1;
        }

        if (desired_night == g_is_night) {
            confirm = 0;
            continue;
        }

        int needed = t.confirm_samples > 0 ? t.confirm_samples : 1;
        if (++confirm < needed)
            continue;

        if (desired_night)
            switch_to_night();
        else
            switch_to_day();

        g_is_night = desired_night;
        confirm = 0;
        last_switch_ms = now_ms();

        printf("[night] transition complete -> state=%s lock=%dms\n",
               g_is_night ? "night" : "day", t.lock_ms);
    }

    printf("[night] auto day/night monitor stopped\n");
    return NULL;
}

int night_start(void)
{
    g_night_running = 1;
    if (pthread_create(&g_night_tid, NULL, night_loop, NULL) != 0) {
        perror("[night] pthread_create");
        g_night_running = 0;
        return -1;
    }
    g_night_started = 1;
    return 0;
}

void night_stop(void)
{
    /* Called on every exit path of main, including before night_start()
     * (e.g. device open failed). pthread_join(0) dereferences a NULL thread
     * pointer in musl: it was the segfault that turned one failed start into
     * a restart loop of segfaulting starts. */
    if (!g_night_started) return;
    g_night_started = 0;
    g_night_running = 0;
    pthread_join(g_night_tid, NULL);
}
