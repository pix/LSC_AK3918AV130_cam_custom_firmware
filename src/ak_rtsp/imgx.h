#pragma once
/* Extra image settings (reverse-engineered from stock anyka_ipc, see the
 * RE notes): WDR strength, anti-flicker frequency, privacy masks.
 *   image.wdr      -50..50   0 = calibrated tone curves (stock "off" = 0,
 *                            stock "on" = +50, -50 = linear/no WDR)
 *   image.flicker  50 | 60 | off   (exposure step = lines per half period)
 *   mask.areas     up to 4 "x0,y0,x1,y1" rectangles (1920x1080 pixels), ';'-separated
 *   mask.color     #rrggbb, or "mosaic"
 * Values load early; hardware is written once imgx_ready() was called and
 * after every day/night ISP reload (imgx_apply_all). */
#include <stddef.h>
void imgx_init(void);          /* register settings */
void imgx_ready(void);         /* ISP running: apply everything (crash-guarded at boot) */
void imgx_apply_all(void);     /* after an ISP calibration reload */
void imgx_set_privacy(int on);  /* black out the whole picture (schedule privacy mode) */
int  imgx_flicker_step(void);  /* exposure step for ae.c (0 = leave calibration) */
