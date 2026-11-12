#pragma once
/* Stock anyka_ipc automatic day/night ("soft photosensor"), ported from the
 * disassembly (see sdn.c and docs/re/re_daynight.md). The caller reads the ISP
 * payloads (AE run info, exposure attr, AWB stats) and passes them in. */
#include <stdint.h>

typedef struct {
    uint32_t day_to_night_lum;           /* p0  +0x00 (g+0xe4)  6300000 */
    uint32_t night_to_day_lum;           /* p1  +0x04 (g+0xe8)  2000000 */
    uint32_t awb_night_dis_min;          /* p2  +0x08 (g+0xec)  50  (never read by IR path) */
    uint32_t awb_night_dis_max;          /* p3  +0x0c (g+0xf0)  850 */
    uint32_t day_check_frame_num;        /* p4  +0x10 (g+0xf4)  10 */
    uint32_t night_check_frame_num;      /* p5  +0x14 (g+0xf8)  10 */
    uint32_t exp_stable_check_frame_num; /* p6  +0x18 (g+0xfc)  10 */
    uint32_t stable_wait_timeout_ms;     /* p7  +0x1c (g+0x100) 60000 */
    uint32_t gain_r;                     /* p8  +0x20 (g+0x104) 1280 */
    int32_t  offset_r;                   /* p9  +0x24 (g+0x108) 2048 (converted with i2d = signed) */
    uint32_t gain_b;                     /* p10 +0x28 (g+0x10c) 1280 */
    int32_t  offset_b;                   /* p11 +0x2c (g+0x110) 2048 */
    uint32_t gain_rb;                    /* p12 +0x30 (g+0x114) 1280 (applied to the G term) */
    int32_t  offset_rb;                  /* p13 +0x34 (g+0x118) 2048 */
    uint32_t blk_check_flag;             /* p14 +0x38 (g+0x11c) 1 */
    uint32_t low_lumi_th;                /* p15 +0x3c (g+0x120) 5 */
    uint32_t high_lumi_th;               /* p16 +0x40 (g+0x124) 245 */
    uint32_t non_infrared_ratio_th;      /* p17 +0x44 (g+0x128) 40 */
    uint32_t highlight_lumi_th;          /* p18 +0x48 (g+0x12c) 240 (white-LED path only, unused) */
    uint32_t highlight_blk_cnt_max;      /* p19 +0x4c (g+0x130) 192 (white-LED path only, unused) */
    uint32_t lock_cnt_th;                /* p20 +0x50 (g+0x134) 5 */
    uint32_t auto_lock_time_ms;          /* p21 +0x54 (g+0x138) 60000 */
    uint32_t revised_dis_max_ratio;      /* p22 +0x58 (g+0x13c) 5 */
} sdn_params;

extern const sdn_params SDN_DEFAULTS;

#define SDN_BLKS      768    /* 32 x 24 */

enum {
    SDN_AE_STABLE   = 1,  /* run_info[3] != 0 (AE reports converged)         */
    SDN_AE_IN_RANGE = 2,  /* every gain/exposure strictly inside its limits  */
    SDN_AE_AT_MAX   = 4,  /* exp >= max (step-rounded) and all gains == max  */
    SDN_AE_AT_MIN   = 8,  /* not AT_MAX, exp == min and all gains == min     */
};

enum { SDN_BUSY = -1, SDN_DAY = 0, SDN_NIGHT = 1, SDN_NOT_STABLE = -2 };

typedef struct {
    /* persistent per-device state, stock 0x5f3290 + dev*0x24 + ... */
    uint32_t lock_thread;   /* +0x58 lock thread running            */
    uint32_t lock;          /* +0x5c anti-flap lock active           */
    uint32_t lock_req;      /* +0x60 set by lock thread              */
    uint32_t switch_cnt;    /* +0x68 decisions != current state      */
    uint32_t last_ev;       /* +0x6c                                 */
    uint64_t lock_time_ms;  /* +0x70 ak_get_ostime at lock start     */
    uint64_t lock_tick_ms;  /* next 120 s tick of thread 0x126ae4    */

    /* emulation of the blocking loops */
    uint32_t frame_ms;      /* 1000/fps, 100 if fps==0               */
    int      phase;
    int      eval_night;    /* cur_state latched for this evaluation */
    int      skip;          /* frames to sleep without reading       */
    int32_t  tries;         /* r7 in wait_exp_stable                 */
    int32_t  tries_after;
    uint32_t inner_left, cnt_max, cnt_min;
    int      stale_stable;  /* sp+0x43 after the inner loop          */
    uint32_t idx, hits;
} sdn_state;

int      sdn_params_sanitize(sdn_params *p);
uint64_t sdn_ev(const uint8_t run_info[36], const uint8_t expo[0x720]);
unsigned sdn_ae_classify(const uint8_t run_info[36], const uint8_t expo[0x720]);
void     sdn_blk_lumi(const uint8_t *awb, int w, int h, uint32_t y[SDN_BLKS]);
int      sdn_non_ir_ratio(const uint8_t *awb, int w, int h, const sdn_params *p, uint32_t *out_thr);
int32_t  sdn_rgb_dis(const uint8_t *awb, const sdn_params *p);
void     sdn_reset(sdn_state *st, int fps);
/* One call per frame period; returns SDN_BUSY, SDN_NOT_STABLE, SDN_DAY or SDN_NIGHT. */
int      sdn_step(sdn_state *st, const sdn_params *p, uint32_t ev, uint32_t ratio_or_dis,
                  unsigned ae, uint64_t now_ms, int cur_is_night);
