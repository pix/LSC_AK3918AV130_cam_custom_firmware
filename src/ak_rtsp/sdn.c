/*
 * stock_daynight.c - plain-C port of anyka_ipc's automatic day/night decision
 * ("soft photosensor", libak_vpss statically linked into anyka_ipc).
 *
 * Source binary: out/app_extracted/bin/anyka_ipc (ARM32). All addresses below
 * are in that binary. Ported from the disassembly (not only Ghidra's C), in
 * particular the soft-float sequences in ak_vpss_get_non_infrared_ratio.
 *
 *   ak_vpss_set_soft_ps_param   0x128a6c  param check + copy to 0x5f3374
 *   ak_vpss_get_soft_ps_level   0x12a5d0  wait AE stable, then day/night check
 *   ak_vpss_wait_exp_stable     0x1284bc  "AE stable" wait (blocking)
 *   day_mode_check              0x129260
 *   night_mode_check            0x129ffc
 *   ak_vpss_get_ev              0x128fd8
 *   ak_vpss_get_blk_lumi        0x13d2dc
 *   ak_vpss_get_non_infrared_ratio 0x13d538
 *   ak_vpss_get_rgb_dis         0x129b7c  (only used when blk_check_flag == 0)
 *   lock thread                 0x126ae4  (started by set_soft_ps_param)
 *   app caller loop             0xd7a14   (thread in anyka_ipc, 100 ms cadence)
 *
 * No Anyka libraries are needed. The caller does the ISP queries itself with
 * isp_tunnel() and passes the raw payloads in:
 *   run_info : ISP_INNER_GET_AE_RUN_INFO  0x80044952, 36 bytes
 *   expo     : ISP_INNER_GET_EXPOSURE     0x8004498b, 0x720 bytes
 *   awb_stat : Ak_ISP_get_awb_stat_info   0x80044959, 0x24a0 bytes
 *
 * Floating point: the stock code uses IEEE double via __aeabi_d* soft-float.
 * Plain C double gives the same results as long as the compiler does not
 * fuse or reorder operations. Build with  -ffp-contract=off  and without
 * -ffast-math. All values are read little-endian.
 *
 * Confidence markers: [H] read from the instructions, [M] inferred, [L] guess.
 */
#include "sdn.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ------------------------------------------------------------------------ */
/* Parameters: 23 x u32 = 0x5c bytes, same layout as stock (copied to        */
/* 0x5f3374 = 0x5f3290+0xe4). The order matches _ht_hw_settings.ini           */
/* [ps_soft_*] and the checks in set_soft_ps_param [H on layout, H on ini     */
/* values].                                                                  */
/* ------------------------------------------------------------------------ */

/* Stock defaults from out/app_extracted/local/_ht_hw_settings.ini lines 181-203 */
const sdn_params SDN_DEFAULTS = {
    6300000, 2000000, 50, 850, 10, 10, 10, 60000,
    1280, 2048, 1280, 2048, 1280, 2048,
    1, 5, 245, 40, 240, 192, 5, 60000, 5
};

/* Mirror of the checks in ak_vpss_set_soft_ps_param 0x128a6c [H].
 * Returns -1 where stock returns 0x101 (param rejected), else 0 after the
 * same silent clamps stock applies. */
int sdn_params_sanitize(sdn_params *p)
{
    if (!p->day_to_night_lum || !p->night_to_day_lum || !p->awb_night_dis_max ||
        !p->day_check_frame_num || !p->night_check_frame_num ||
        !p->exp_stable_check_frame_num || !p->gain_r || !p->gain_b)
        return -1;                                            /* 0x128ad0..0x128b54 */
    if (p->gain_rb == 0)                     p->gain_rb = 0x400;        /* 0x780 */
    if (p->blk_check_flag > 1)               p->blk_check_flag = 0;     /* 0x786 */
    if (p->low_lumi_th > 0xff)               p->low_lumi_th = 0xc;      /* 0x78c */
    if (p->high_lumi_th - 1u > 0xfe)         p->high_lumi_th = 0xfc;    /* 0x792 */
    if (p->non_infrared_ratio_th - 1u > 0x62) p->non_infrared_ratio_th = 0x23; /* 0x798 */
    if (p->highlight_lumi_th - 1u > 0xfd)    p->highlight_lumi_th = 0xaa;   /* 0x79e */
    if (p->highlight_blk_cnt_max - 1u > 0x2ff) p->highlight_blk_cnt_max = 0x2d; /* 0x7a4 */
    if (p->revised_dis_max_ratio > 0x400)    p->revised_dis_max_ratio = 5;  /* 0x7aa */
    return 0;
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */
static inline uint32_t rd32(const uint8_t *b, unsigned off)
{
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

/* stock "abs": dcmpge(x, 0) ? x : flip sign bit */
static inline double absd(double x) { return (x >= 0.0) ? x : -x; }

/* __aeabi_d2uiz: truncate toward zero, negatives -> 0, saturate */
static inline uint32_t d2uiz(double x)
{
    if (!(x > 0.0)) return 0;
    if (x >= 4294967296.0) return 0xffffffffu;
    return (uint32_t)x;
}

/* __aeabi_d2iz: truncate toward zero, saturate */
static inline int32_t d2iz(double x)
{
    if (x != x) return 0;
    if (x >= 2147483648.0) return 0x7fffffff;
    if (x <= -2147483649.0) return (int32_t)0x80000000;
    return (int32_t)x;
}

/* ------------------------------------------------------------------------ */
/* Field offsets [H on offsets from the code, M on names]                    */
/* ------------------------------------------------------------------------ */
/* AE run info (0x80044952, 36 B) - AK_ISP_AE_RUN_INFO */
#define RI_LUM_A      1      /* u8, used only as |b1-b2| in get_ev [names L] */
#define RI_LUM_B      2      /* u8 */
#define RI_STABLE     3      /* u8, !=0 => wait_exp_stable returns at once */
#define RI_A_GAIN     4      /* u32, range-checked against pub[7]..pub[6] */
#define RI_D_GAIN     8      /* u32, against pub[3]..pub[2] */
#define RI_ISP_GAIN   0xc    /* u32, against pub[4]..pub[5] */
#define RI_EXP_TIME   0x10   /* u32, against pub[1]..pub[0] rounded down to pub[8] */
/* Exposure attr (0x8004498b, 0x720 B); ak_vpss_get_ae_attr copies from +0x1c */
#define EX_PUB        0x1c
#define EX_EXP_MAX    (EX_PUB + 0x00)
#define EX_EXP_MIN    (EX_PUB + 0x04)
#define EX_DG_MAX     (EX_PUB + 0x08)
#define EX_DG_MIN     (EX_PUB + 0x0c)
#define EX_ISPG_MIN   (EX_PUB + 0x10)
#define EX_ISPG_MAX   (EX_PUB + 0x14)
#define EX_AG_MAX     (EX_PUB + 0x18)
#define EX_AG_MIN     (EX_PUB + 0x1c)
#define EX_EXP_STEP   (EX_PUB + 0x20)
/* AWB stat (0x80044959, 0x24a0 B) */
#define AWB_TOT_R     0x24   /* u32 global sums used by non_ir / rgb_dis */
#define AWB_TOT_G     0x4c
#define AWB_TOT_B     0x74
#define AWB_TOT_CNT   0x9c
#define AWB_BLK_R     0xa0   /* u32 [24][32] per-block sums, row-major */
#define AWB_BLK_G     0xca0
#define AWB_BLK_B     0x18a0

/* ------------------------------------------------------------------------ */
/* EV - ak_vpss_get_ev 0x128fd8 [H]. Bigger EV = darker scene.               */
/* Needs the exposure attr as well as run_info (it reads pub[5], pub[6]).    */
/* Stock keeps EV in 32 bits (it wraps); the value is returned zero-extended. */
/* ------------------------------------------------------------------------ */
uint64_t sdn_ev(const uint8_t run_info[36], const uint8_t expo[0x720])
{
    uint32_t a   = rd32(run_info, RI_A_GAIN);
    uint32_t d   = rd32(run_info, RI_D_GAIN);
    uint32_t isp = rd32(run_info, RI_ISP_GAIN);
    uint32_t exp = rd32(run_info, RI_EXP_TIME);

    /* 0x1290d8: mul, asr #8, mul, asr #8, mul (32-bit, signed shifts) */
    int32_t t = (int32_t)(a * d) >> 8;
    t = (int32_t)(isp * (uint32_t)t) >> 8;
    uint32_t ev = exp * (uint32_t)t;

    /* 0x129110: boost only when isp gain >= isp_d_gain_max AND a gain >=
     * a_gain_max (both maxed out). Unsigned compares. */
    if (isp < rd32(expo, EX_ISPG_MAX) || a < rd32(expo, EX_AG_MAX))
        return ev;

    uint32_t diff = (uint32_t)abs((int)run_info[RI_LUM_A] - (int)run_info[RI_LUM_B]);
    if (ev == 0)              /* stock would call __aeabi_uidiv(.., 0); guard */
        return ev;
    uint32_t q = 0xffffffffu / ev;                          /* 0x1291c0 */
    if (q >= diff) {                                        /* 0x1291f4 */
        ev = ev + (ev * diff) / 100u;
    } else {                                                /* 0x1291cc */
        ev = ev + (q * ev) / 100u + (ev * (diff - q)) / 100u;
    }
    return ev;
}

/* ------------------------------------------------------------------------ */
/* "AE stable" classification of one (run_info, expo) sample, as tested in    */
/* ak_vpss_wait_exp_stable 0x1284bc [H].                                     */
/* ------------------------------------------------------------------------ */

unsigned sdn_ae_classify(const uint8_t run_info[36], const uint8_t expo[0x720])
{
    unsigned r = 0;
    uint32_t a = rd32(run_info, RI_A_GAIN), d = rd32(run_info, RI_D_GAIN);
    uint32_t isp = rd32(run_info, RI_ISP_GAIN), exp = rd32(run_info, RI_EXP_TIME);
    uint32_t step = rd32(expo, EX_EXP_STEP);
    if (step == 0) step = 1;   /* stock would divide by zero; not seen in practice */
    uint32_t exp_max_r = (rd32(expo, EX_EXP_MAX) / step) * step;

    if (run_info[RI_STABLE]) r |= SDN_AE_STABLE;

    /* 0x128660..0x12893c */
    if (exp > rd32(expo, EX_EXP_MIN) && exp < exp_max_r &&
        a > rd32(expo, EX_AG_MIN) && a < rd32(expo, EX_AG_MAX) &&
        d > rd32(expo, EX_DG_MIN) && d < rd32(expo, EX_DG_MAX) &&
        isp > rd32(expo, EX_ISPG_MIN) && isp < rd32(expo, EX_ISPG_MAX))
        r |= SDN_AE_IN_RANGE;

    /* 0x128748..0x128830 */
    if (exp >= exp_max_r && a == rd32(expo, EX_AG_MAX) &&
        d == rd32(expo, EX_DG_MAX) && isp == rd32(expo, EX_ISPG_MAX))
        r |= SDN_AE_AT_MAX;
    else if (exp == rd32(expo, EX_EXP_MIN) && a == rd32(expo, EX_AG_MIN) &&
             d == rd32(expo, EX_DG_MIN) && isp == rd32(expo, EX_ISPG_MIN))
        r |= SDN_AE_AT_MIN;
    return r;
}

/* ------------------------------------------------------------------------ */
/* Block luma - ak_vpss_get_blk_lumi 0x13d2dc [H].                           */
/* w,h = VI dev attr crop width/height (+0x1c/+0x20); stock fills them with   */
/* the sensor resolution (1920x1080 here) [M] -> n = 60*45 = 2700.            */
/* ------------------------------------------------------------------------ */
void sdn_blk_lumi(const uint8_t *awb, int w, int h, uint32_t y[SDN_BLKS])
{
    uint32_t n = (uint32_t)((w / 32) * (h / 24));   /* signed trunc divides */
    if (n == 0) n = 1;                              /* stock: div by 0 */
    for (int i = 0; i < SDN_BLKS; i++) {
        uint32_t r = rd32(awb, AWB_BLK_R + 4 * i) / n;
        uint32_t g = rd32(awb, AWB_BLK_G + 4 * i) / n;
        uint32_t b = rd32(awb, AWB_BLK_B + 4 * i) / n;
        y[i] = ((r >> 2) * 38 + (g >> 2) * 75 + (b >> 2) * 15) >> 7;
    }
}

/* One block's colour distance, shared by both passes of non_ir_ratio.
 * use_y = 1: first pass (offset scaled by Y/256), 0: widening pass. */
static uint32_t blk_dis(uint32_t r, uint32_t g, uint32_t b, uint32_t y, int use_y,
                        const sdn_params *p)
{
    double rn, gn, bn;
    uint32_t s = r + g + b;                        /* 32-bit wrap as stock */
    if (s == 0) {                                  /* 0x13d9c0 / 0x13de64 */
        rn = (double)(int32_t)r; gn = (double)(int32_t)g; bn = (double)(int32_t)b;
    } else {                                       /* 0x13dd38 / 0x13e198 */
        double sd = (double)(int32_t)s;
        rn = (double)(int32_t)r / sd;
        gn = (double)(int32_t)g / sd;
        bn = (double)(int32_t)b / sd;
    }
    double yd = (double)y;
    double tr, tg, tb, er, eg, eb;
    if (use_y) {                                   /* 0x13d9f4.. */
        tr = (double)p->offset_r  * yd; tr = tr * (1.0 / 1024); tr = tr * (1.0 / 256);
        tg = (double)p->offset_rb * yd; tg = tg * (1.0 / 1024); tg = tg * (1.0 / 256);
        tb = (double)p->offset_b  * yd; tb = tb * (1.0 / 1024); tb = tb * (1.0 / 256);
    } else {                                       /* 0x13de9c.. (no Y term) */
        tr = (double)p->offset_r  * (1.0 / 1024);
        tg = (double)p->offset_rb * (1.0 / 1024);
        tb = (double)p->offset_b  * (1.0 / 1024);
    }
    er = absd(rn - tr) * (double)p->gain_r;  er = er * (1.0 / 1024);
    eg = absd(gn - tg) * (double)p->gain_rb; eg = eg * (1.0 / 1024);
    eb = absd(bn - tb) * (double)p->gain_b;  eb = eb * (1.0 / 1024);
    double sum = absd(eg - er) + absd(eg - eb);    /* 0x13dc74 */
    sum = sum + absd(er - eb);                     /* 0x13dc84 */
    return d2uiz(sum * 10000.0);                   /* 0x13dc90 */
}

/* ------------------------------------------------------------------------ */
/* Non-IR ratio - ak_vpss_get_non_infrared_ratio 0x13d538 [H].               */
/* Returns the percentage (0..100) of usable blocks (low<=Y<=high) whose      */
/* colour distance exceeds the threshold, i.e. how much of the image is lit  */
/* by visible (coloured) light rather than IR. -1 = params rejected (stock    */
/* 0x101). Optional out_thr gets the final threshold for logging.             */
/* Stock reads the AWB stats twice (once here, once in get_blk_lumi), so Y   */
/* may come from a newer frame than the R/G/B sums; this port uses one buffer.*/
/* ------------------------------------------------------------------------ */
int sdn_non_ir_ratio(const uint8_t *awb, int w, int h, const sdn_params *p,
                     uint32_t *out_thr)
{
    /* argument checks, 0x13d5cc..0x13d63c (struct = p3,p8..p13,p15,p16,p17,p22) */
    if (p->awb_night_dis_max == 0 || p->gain_r == 0 || p->gain_b == 0 ||
        p->gain_rb == 0 || p->low_lumi_th > 0xff || p->high_lumi_th - 1u > 0xfe)
        return -1;
    uint32_t revised = p->revised_dis_max_ratio;
    if (revised > 0x400) revised = 5;                       /* 0x13e5c0 */

    /* ---- global AWB colour slope k, 0x13d7a4..0x13d8f4 / 0x13e610..0x13e6d8 */
    double k = 0.0;
    uint32_t cnt = rd32(awb, AWB_TOT_CNT);
    if (cnt != 0) {
        double c  = (double)cnt;
        double Rm = (double)rd32(awb, AWB_TOT_R) / c;
        double Gm = (double)rd32(awb, AWB_TOT_G) / c;
        double Bm = (double)rd32(awb, AWB_TOT_B) / c;
        double Gp, dd, th;
        if (Gm == 0.0) { Gp = 1.0; dd = Rm - 1.0; th = 0.08; }   /* 0x13d840 */
        else           { Gp = Gm;  dd = Rm - Gm;  th = Gm * 0.08; } /* 0x13e6a0 */
        if (dd > th) {
            double x = Rm / Gp - 1.0;
            if (fabs(x) < 1.0) k = 1.0 - Bm / Gp;           /* 0x13e680 */
            else               k = (Bm / Gp - 1.0) / x;     /* 0x13d8d8 */
        }
    }
    /* (fabs on the k path is libm fabs at 0x13d8b0, not the sign-flip absd) */

    /* ---- block luma */
    uint32_t y[SDN_BLKS];
    sdn_blk_lumi(awb, w, h, y);
    uint32_t n = (uint32_t)((w / 32) * (h / 24));            /* 0x13d914 */
    if (n == 0) n = 1;

    uint32_t dis[SDN_BLKS];
    memset(dis, 0, sizeof(dis));
    uint32_t low = p->low_lumi_th, high = p->high_lumi_th;
    uint32_t thr = p->awb_night_dis_max;                     /* sp+0x70 = s[0] */
    int32_t count = 0;

    /* ---- first pass, 0x13d99c..0x13ddc0 */
    for (int i = 0; i < SDN_BLKS; i++) {
        uint32_t Y = y[i];
        if (Y < low || Y > high) continue;                   /* unsigned */
        uint32_t r = rd32(awb, AWB_BLK_R + 4 * i) / n;
        uint32_t g = rd32(awb, AWB_BLK_G + 4 * i) / n;
        uint32_t b = rd32(awb, AWB_BLK_B + 4 * i) / n;
        count++;
        dis[i] = blk_dis(r, g, b, Y, 1, p);
    }

    /* ---- widening pass when no block was in the luma window, 0x13ddc4.. */
    if (count == 0 && ((int32_t)low > 0 || (int32_t)high <= 254)) {
        int32_t L = (int32_t)low, Hh = (int32_t)high, c2;
        do {
            L--; Hh++;                                       /* 0x13de00 */
            c2 = 0;
            for (int i = 0; i < SDN_BLKS; i++) {
                uint32_t Y = y[i];
                /* unsigned compares: once L < 0 nothing matches (stock quirk) */
                if (Y < (uint32_t)L || Y > (uint32_t)Hh) continue;
                uint32_t r = rd32(awb, AWB_BLK_R + 4 * i) / n;
                uint32_t g = rd32(awb, AWB_BLK_G + 4 * i) / n;
                uint32_t b = rd32(awb, AWB_BLK_B + 4 * i) / n;
                c2++;
                dis[i] = blk_dis(r, g, b, Y, 0, p);
            }
        } while (c2 == 0 && (Hh <= 254 || L > 0));           /* 0x13e23c */

        /* threshold rescale by window growth, 0x13e26c..0x13e2cc */
        int32_t wdt = (int32_t)(high + 1 - low), num, den;
        if (wdt == 256) { num = 100; den = 1; wdt = 255; }
        else            { den = 256 - wdt; num = 100 * den; }
        num = num + ((Hh - L + 1) - wdt) * 10;
        int32_t q = num / den;                               /* __aeabi_idiv */
        thr = (p->awb_night_dis_max * (uint32_t)q) / 100u;   /* umull magic */
        count = c2;
    }

    /* ---- k < 0 -> raise threshold by revised_dis_max_ratio %, 0x13e2d0/0x13e644 */
    if (k < 0.0)
        thr = thr + (revised * thr) / 100u;

    /* ---- count blocks above threshold over all 768, 0x13e2ec..0x13e32c */
    int32_t over = 0;
    for (int i = 0; i < SDN_BLKS; i++)
        if (thr < dis[i]) over++;
    if (out_thr) *out_thr = thr;

    /* 0x13e558 */
    return count ? (over * 100) / count : 0;
}

/* ------------------------------------------------------------------------ */
/* RGB distance - ak_vpss_get_rgb_dis 0x129b7c [H]. Only used by             */
/* night_mode_check when blk_check_flag == 0 (stock ini uses 1).             */
/* ------------------------------------------------------------------------ */
int32_t sdn_rgb_dis(const uint8_t *awb, const sdn_params *p)
{
    double R, G, B, dRB, dRG, dGB;
    uint32_t cnt = rd32(awb, AWB_TOT_CNT);
    if (cnt == 0) {                                          /* 0x129f2c */
        R = G = B = 1.0;
    } else {
        double c = (double)cnt;
        R = (double)rd32(awb, AWB_TOT_R) / c;
        G = (double)rd32(awb, AWB_TOT_G) / c;
        B = (double)rd32(awb, AWB_TOT_B) / c;
    }
    dRB = (R >= B) ? R - B : B - R;                          /* 0x129ce8 */
    dRG = (R >= G) ? R - G : G - R;                          /* 0x129d0c */
    dGB = (G <= B) ? B - G : G - B;                          /* 0x129d34 */
    double x = absd(dRG - (double)p->offset_r * (1.0 / 1024));
    x = x * (double)p->gain_r; x = x * (1.0 / 1024);
    x = x + dRB;                                             /* 0x129de0 */
    double z = absd(dGB - (double)p->offset_b * (1.0 / 1024));
    z = z * (double)p->gain_b; z = z * (1.0 / 1024);
    z = z + x;                                               /* 0x129e70 */
    return d2iz(z * 100.0);
}

/* ------------------------------------------------------------------------ */
/* Decision state machine.                                                   */
/*                                                                          */
/* Stock is blocking: one ak_vpss_get_soft_ps_level() call waits for AE      */
/* (wait_exp_stable), then takes N samples spaced by one frame period        */
/* (1000/fps ms), then returns. The app thread calls it again after 100 ms   */
/* (200 ms after an error). Here each sdn_step() call is ONE of those        */
/* per-frame reads: call it every frame_ms with fresh values. It returns     */
/* SDN_BUSY until the evaluation completes.                                  */
/*                                                                          */
/* Inputs per call:                                                          */
/*   ev            = sdn_ev(run_info, expo) of this frame                    */
/*   ratio_or_dis  = night only: sdn_non_ir_ratio() when blk_check_flag==1,   */
/*                   else (uint32_t)sdn_rgb_dis(). Ignored in day mode.       */
/*   ae            = sdn_ae_classify(run_info, expo) of this frame           */
/*   now_ms        = monotonic ms                                            */
/*   cur_is_night  = current applied mode (stock: ctx+0xcc), latched at the   */
/*                   start of each evaluation                                */
/* ------------------------------------------------------------------------ */

enum { PH_START, PH_WAIT, PH_LIMIT, PH_SAMPLE };


void sdn_reset(sdn_state *st, int fps)
{
    memset(st, 0, sizeof(*st));
    /* day_mode_check 0x1292a0 / wait_exp_stable 0x128590: idiv(1000, fps) */
    st->frame_ms = fps ? (uint32_t)(1000 / fps) : 100;
    st->phase = PH_START;
}

/* Lock thread 0x126ae4: runs while lock_cnt_th && auto_lock_time_ms (set in
 * set_soft_ps_param 0x128e2c). First pass at thread start, then every
 * 120000 ms (hard-coded 0x1d4c0, NOT auto_lock_time_ms). */
static void lock_thread_poll(sdn_state *st, const sdn_params *p, uint64_t now)
{
    if (!p->lock_cnt_th || !p->auto_lock_time_ms) { st->lock_thread = 0; return; }
    if (!st->lock_thread) { st->lock_thread = 1; st->lock_tick_ms = now; }
    while (now >= st->lock_tick_ms) {
        st->lock_req = (st->switch_cnt >= p->lock_cnt_th) ? 1 : 0;
        st->switch_cnt = 0;
        st->lock_tick_ms += 120000;
    }
}

/* end of evaluation: soft_ps_level 0x12a728 counts decisions that differ */
static int finish(sdn_state *st, int out)
{
    if (out != st->eval_night) st->switch_cnt++;
    st->phase = PH_START;
    return out;
}

/* night_mode_check tail, 0x12a2d0..0x12a52c */
static int night_decide(sdn_state *st, const sdn_params *p, uint32_t ev, uint64_t now)
{
    if (st->lock_req) {                         /* 0x12a3b0 */
        st->lock_req = 0;
        st->lock = 1;
        st->lock_time_ms = now;
    }
    if (st->hits != p->night_check_frame_num)   /* not all samples bright+coloured */
        return 1;                               /* stay night, last_ev untouched */
    int out = 0;
    if (st->lock) {                             /* 0x12a400 */
        uint32_t elapsed = (uint32_t)(now - st->lock_time_ms);
        if (st->last_ev == 0) st->last_ev = ev;
        if (p->auto_lock_time_ms <= elapsed) {  /* lock expired */
            st->lock = 0; st->switch_cnt = 0; out = 0;
        } else {
            uint32_t diff = (uint32_t)abs((int32_t)(st->last_ev - ev));
            uint32_t pct = st->last_ev ? (diff * 100u) / st->last_ev : 0;
            if (pct > 9) { st->lock = 0; st->switch_cnt = 0; out = 0; }
            else out = 1;                       /* locked: stay night */
        }
    }
    st->last_ev = ev;                           /* 0x12a3a0 */
    return out;
}

int sdn_step(sdn_state *st, const sdn_params *p, uint32_t ev, uint32_t ratio_or_dis,
             unsigned ae, uint64_t now_ms, int cur_is_night)
{
    lock_thread_poll(st, p, now_ms);

    if (st->skip) { st->skip--; return SDN_BUSY; }   /* a frame of ak_sleep_ms */

    switch (st->phase) {
    case PH_START:                                   /* wait_exp_stable entry */
        st->eval_night = cur_is_night ? 1 : 0;
        if (p->exp_stable_check_frame_num == 0) { st->phase = PH_START; return SDN_NOT_STABLE; }
        st->tries = (int32_t)(p->stable_wait_timeout_ms / st->frame_ms) + 1;   /* 0x1285c4 */
        st->stale_stable = 0;
        st->phase = PH_WAIT;
        /* fall through */
    case PH_WAIT:                                    /* loop top 0x1285c8 */
        if (st->stale_stable || st->tries <= 0) {    /* stock quirk + timeout: 0x119 */
            st->phase = PH_START;
            return SDN_NOT_STABLE;
        }
        if (ae & SDN_AE_STABLE) {                    /* 0x1289a4: return 0 at once */
            st->phase = PH_SAMPLE; st->idx = 0; st->hits = 0;
            break;                                   /* this frame is sample #1 */
        }
        if (ae & SDN_AE_IN_RANGE) {                  /* 0x12895c: sleep, tries-- */
            st->tries--;
            return SDN_BUSY;
        }
        /* at a limit: N more reads (first one is immediate, same instant) */
        st->tries_after = st->tries - (int32_t)p->exp_stable_check_frame_num - 1;
        st->inner_left = p->exp_stable_check_frame_num;
        st->cnt_max = st->cnt_min = 0;
        st->phase = PH_LIMIT;
        /* fall through */
    case PH_LIMIT:                                   /* inner loop 0x1286d0 */
        if (ae & SDN_AE_AT_MAX) st->cnt_max++;
        else if (ae & SDN_AE_AT_MIN) st->cnt_min++;
        st->stale_stable = (ae & SDN_AE_STABLE) ? 1 : 0;
        if (--st->inner_left)
            return SDN_BUSY;
        st->tries = st->tries_after;
        if (st->cnt_min == p->exp_stable_check_frame_num ||
            st->cnt_max == p->exp_stable_check_frame_num) {
            /* stable at a limit; sampling starts on the next frame */
            st->phase = PH_SAMPLE; st->idx = 0; st->hits = 0;
            return SDN_BUSY;
        }
        st->skip = 1;                                /* extra sleep 0x1288c8 */
        st->phase = PH_WAIT;
        return SDN_BUSY;
    case PH_SAMPLE:
        break;
    }

    /* PH_SAMPLE: one sample of day_mode_check / night_mode_check */
    uint32_t N = st->eval_night ? p->night_check_frame_num : p->day_check_frame_num;
    if (N == 0)   /* stock: day -> 1, night -> lock logic with ev 0 */
        return finish(st, st->eval_night ? night_decide(st, p, 0, now_ms) : 1);

    if (!st->eval_night) {
        if (p->day_to_night_lum < ev) st->hits++;                 /* 0x1292d8 */
    } else if (p->blk_check_flag) {
        if (ev < p->night_to_day_lum && ratio_or_dis > p->non_infrared_ratio_th)
            st->hits++;                                           /* 0x12a1f4 */
    } else {
        if (ev < p->night_to_day_lum && ratio_or_dis > p->awb_night_dis_max)
            st->hits++;                                           /* 0x12a0f4 */
    }
    if (++st->idx < N)
        return SDN_BUSY;

    if (!st->eval_night)                             /* 0x129330 */
        return finish(st, st->hits == N ? 1 : 0);
    return finish(st, night_decide(st, p, ev, now_ms));
}

/* ------------------------------------------------------------------------ */
/* Example driver (mirrors app thread 0xd7a14). Not compiled by default.     */
/* ------------------------------------------------------------------------ */
