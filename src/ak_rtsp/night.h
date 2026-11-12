#pragma once
#include <stdint.h>

/* Auto day/night mode monitor.
 *
 * Watches ae.c's AE state (via ae_get_last_stats()) and, on a sustained
 * brightness change, coordinates: IR-cut filter position, IR LED, sensor
 * frame-rate/VTS, and WB gains — mirroring stock anyka_ipc's night_mode_check
 * / day_mode_check (Ghidra-reversed from stock; this reimplementation uses
 * a different EV proxy/thresholds than stock).
 *
 * Call night_start() after ae_start() (needs AE samples to already be
 * flowing). night_stop() on shutdown. */

int  night_start(void);
void night_stop(void);

/* Live-tunable thresholds + manual override, for control.c's TCP tuning
 * server. override=AUTO runs the normal threshold logic; FORCE_DAY/
 * FORCE_NIGHT pin the mode regardless of hw_exp (applied once per change,
 * checked first in night_loop()). */
typedef enum {
    NIGHT_MODE_AUTO        = 0,
    NIGHT_MODE_FORCE_DAY   = 1,
    NIGHT_MODE_FORCE_NIGHT = 2,
} night_override_t;

typedef struct {
    uint32_t         trigger_hw_exp;   /* day->night hw_exp threshold */
    uint32_t         day_hw_exp;       /* night->day hw_exp threshold */
    int              confirm_samples;  /* consecutive samples before switching */
    int              lock_ms;          /* anti-flap lock duration after a switch */
    night_override_t override;
} night_tuning_t;

void night_get_tuning(night_tuning_t *out);
void night_set_tuning(const night_tuning_t *in);

/* Current actual day/night state (1=night, 0=day) — distinct from
 * night_tuning_t.override, which is the requested mode, not the applied one. */
int night_is_night(void);
/* Adaptive frame rate: idle rate (2..14, 0 = off) used after 10 s without
 * motion and with no web viewer; full rate again on motion. */
void night_set_idle_fps(int fps);
void night_set_idle_wake_all(int all);   /* 1: motion anywhere wakes it, 0: only in motion zones */
int  night_fps_now(void);

/* Registers night.algo (stock|exposure). Call before settings are loaded. */
void night_init(void);
/* Scene light from the AE state (stock EV formula; bigger = darker), its
 * 0..100 % log-scale level (-1 until measured; includes IR light at night),
 * and the share of blocks lit by visible light (non-IR ratio, -1 unknown). */
int night_ev(void);
int night_light_pct(void);
int night_non_ir_ratio(void);
