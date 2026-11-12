# Stock anyka_ipc automatic day/night (soft photosensor): RE spec

Binary: `out/app_extracted/bin/anyka_ipc`. Static analysis only (Ghidra decompiles plus objdump disassembly;
the soft-float code in the non-IR ratio was read from the instructions, not from Ghidra's output).
C port: `src/ak_rtsp/sdn.c` (+ sdn.h). Confidence: **[H]** read from the instructions, **[M]** inferred,
**[L]** guess.

## 0. Corrections to re_spec.md section 5
- The EV boost is applied only when the gains are **at** their maxima (`isp_gain >= pub[5]` **and**
  `a_gain >= pub[6]`), not below them. [H]
- The lock check runs every **120 s** (hard-coded `0x1d4c0`), not every `auto_lock_time_ms`. `auto_lock_time_ms`
  is how long the lock lasts. [H]
- `highlight_lumi_th` / `highlight_blk_cnt_max` are **not** used by the IR day/night decision. Only
  `white_led_night_mode_check` 0x129464 reads them, and its only entry point is `ak_vpss_get_soft_wh_level`
  0x129958, which nothing in anyka_ipc calls. [H]
- `awb_night_dis_min` (p2) is never read on the IR path. [H]
- Our `ae.c` labels `run_info+4` as `hw_exp`. Stock code compares `+4` against the **analog gain** limits
  (pub[6]/pub[7]) and `+0x10` against the **exposure-time** limits (pub[0]/pub[1]). night.c's `hw_exp`
  thresholds therefore act on a gain value. See section 1.1. [H on which limits each field is compared to,
  M on the names]

## 1. Inputs (all through `isp_tunnel(inner, payload)`)

### 1.1 AE run info: inner `0x80044952`, 36 B (`AK_ISP_get_ae_run_info` 0x149eb8, passed through unchanged) [H]
| off | type | use | name (M) |
|---|---|---|---|
| +1 | u8 | `abs(b1-b2)` = boost percent in get_ev | luma A (current or target? **L**) |
| +2 | u8 | as above | luma B |
| +3 | u8 | `!= 0`: wait_exp_stable returns "stable" at once | AE stable/converged flag |
| +4 | u32 | EV factor; compared to pub[7] (min) and pub[6] (max) | a_gain (256 = 1x, M) |
| +8 | u32 | EV factor; compared to pub[3] and pub[2] | d_gain |
| +0xc | u32 | EV factor; compared to pub[4] and pub[5] | isp_d_gain |
| +0x10 | u32 | EV factor; compared to pub[1] and `(pub[0]/pub[8])*pub[8]` | exp_time (lines) |
| +0x14..+0x23 | | unused here | (step fields, M) |

### 1.2 Exposure attr: inner `0x8004498b`, 0x720 B; `pub = buf+0x1c` (`ak_vpss_get_ae_attr` copies from +0x1c) [H offsets, M names]
pub[0] exp_time_max, pub[1] exp_time_min, pub[2] d_gain_max, pub[3] d_gain_min, pub[4] isp_d_gain_min,
pub[5] isp_d_gain_max, pub[6] a_gain_max, pub[7] a_gain_min, pub[8] exp_step.
(The min/max order is swapped for the isp gain at pub[4]/pub[5], as the code uses them.)

### 1.3 AWB stats: inner `0x80044959`, 0x24a0 B (`Ak_ISP_get_awb_stat_info` 0x149fa8, unchanged) [H]
| off | content |
|---|---|
| +0x24 | u32 total R  (global; M: element [9] of a 10-entry `total_R[]`) |
| +0x4c | u32 total G |
| +0x74 | u32 total B |
| +0x9c | u32 pixel count for those totals |
| +0xa0 | u32 R[24][32] per-block sums, row-major, block i at `+0xa0+4*i` |
| +0xca0 | u32 G[24][32] |
| +0x18a0 | u32 B[24][32] (ends at 0x24a0) |

`ak_vpss_get_awb_stat_info` 0x1342d0 also appends AWB run info (inner `0x80044990`, 0x26 B) at +0x24a0.
Day/night does not read it. [H]

### 1.4 Other inputs
- Sensor fps: `ak_vpss_get_sensor_fps` → `isp_get_sensor_fps()` (a cached library value, not an ioctl here [M]).
  `frame_ms = 1000/fps` (signed idiv), or 100 when fps == 0. In our firmware use the configured fps (15 day / 10 night),
  or `ISP_INNER_GET_SENSOR_FPS 0x8004497b`. [H on the formula]
- Block divisor `n = (W/32)*(H/24)`, where W,H are VI dev attr +0x1c/+0x20. The app fills these with the sensor
  resolution (`ak_vi_get_sensor_resolution`, code at 0xbd8xx): 1920x1080 gives **n = 2700**. [M]

## 2. Algorithm (ported in sdn.c)

### 2.1 EV, `ak_vpss_get_ev` 0x128fd8 [H]
```
EV = exp * ((isp * ((a*d) >> 8)) >> 8)          // 32-bit multiplies, arithmetic shifts
if (isp >= pub[5] && a >= pub[6]) {              // gains maxed out
   diff = |b1 - b2|; q = 0xffffffff / EV
   EV += (q >= diff) ? EV*diff/100 : q*EV/100 + EV*(diff-q)/100   // u32 math
}
```
Larger EV = darker. At 1x gains and exp 2284, EV = 584704.

### 2.2 "AE stable", `ak_vpss_wait_exp_stable(dev, N=p6, timeout=p7)` 0x1284bc [H]
`tries = timeout/frame_ms + 1`. Loop:
1. Read run_info and expo. If byte +3 != 0, return **stable** with no sleep.
2. If every value is strictly inside its limits (exp in `(pub1, round(pub0))`, a in `(pub7, pub6)`, d in `(pub3, pub2)`,
   isp in `(pub4, pub5)`), the AE is still moving: sleep one frame, `tries--`.
3. Otherwise (at least one value is at a limit), read N more frames, one frame apart. Count frames where
   everything is at max (`exp >= round(pub0)`, a/d/isp == max) and frames where everything is at min. If either
   count == N, return **stable**: AE is saturated, which also counts as settled (very dark or very bright).
   Otherwise `tries -= N+1`, sleep one more frame and loop.
4. If `tries <= 0`, return 0x119 (not stable). Quirk: the same 0x119 is returned if the stable byte was set on the last
   read of step 3.
With defaults (60 s timeout) an evaluation can block for up to 60 s.

### 2.3 day_mode_check 0x129260 (current = day) [H]
Take N = `day_check_frame_num` (10) EV samples, sleeping one frame between them. If **all** satisfy
`EV > day_to_night_lum`, the result is night (1). Otherwise day (0).

### 2.4 night_mode_check 0x129ffc (current = night) [H]
Take N = `night_check_frame_num` samples, one frame apart. A sample counts when:
- `blk_check_flag == 1` (stock): `EV < night_to_day_lum && non_ir_ratio > non_infrared_ratio_th` (40), or
- `blk_check_flag == 0`: `EV < night_to_day_lum && (u32)rgb_dis > awb_night_dis_max`.

After the loop:
1. If `lock_req` is set: `lock = 1`, `lock_req = 0`, `lock_time = now`.
2. If fewer than N samples counted: the result is night. `last_ev` is not updated.
3. If all N counted: when not locked, the result is day. When locked:
   - if `last_ev == 0`, set `last_ev = ev`;
   - if `now - lock_time >= auto_lock_time_ms`: unlock, `switch_cnt = 0`, result day;
   - else if `|last_ev - ev| * 100 / last_ev > 9`: unlock, `switch_cnt = 0`, result day;
   - else the result is night (still locked).

   Then `last_ev = ev`, the EV of the last sample.

### 2.5 Lock (anti-flap), thread 0x126ae4 plus get_soft_ps_level 0x12a5d0 [H]
- `get_soft_ps_level` does `switch_cnt++` whenever the result differs from the passed current state.
- The thread runs when `lock_cnt_th && auto_lock_time_ms`. At start and then every 120 s it sets
  `lock_req = (switch_cnt >= lock_cnt_th)` and `switch_cnt = 0`.
- The lock is only applied in night mode and only blocks night→day. It lasts until `auto_lock_time_ms` passes or the EV
  changes by more than 9 %.

### 2.6 Non-IR ratio, `ak_vpss_get_non_infrared_ratio` 0x13d538 [H]
- `Y[i]` = block luma (`ak_vpss_get_blk_lumi` 0x13d2dc): `((R/n>>2)*38 + (G/n>>2)*75 + (B/n>>2)*15) >> 7`.
- For each block with `low_lumi_th <= Y <= high_lumi_th`:
  `r,g,b = sums/n` (u32), then `rn,gn,bn = r/(r+g+b)` etc. (double), and with `t_c = offset_c*Y/1024/256`:
  `e_r = |rn - t_r|*gain_r/1024`, `e_g = |gn - t_rb|*gain_rb/1024`, `e_b = |bn - t_b|*gain_b/1024`, and
  `dis = (u32)((|e_g-e_r| + |e_g-e_b| + |e_r-e_b|) * 10000)`.
  With the default equal gains/offsets this is `1.25*(|r-g|+|g-b|+|r-b|)` in chromaticity units. Grey (IR-lit)
  blocks give ≈0. Coloured blocks give a large value.
- If no block falls in the window, the window grows by ±1 per iteration. In those passes `t = offset/1024` (no Y
  term). The threshold is then rescaled: `thr = dis_max * ((100*(256-w) + 10*(w'-w)) / (256-w)) / 100`.
  Quirk: once low < 0 nothing matches, because the compare is unsigned.
- Global slope `k` from the AWB totals (+0x24/+0x4c/+0x74/+0x9c). If `k < 0`, `thr += thr*revised_dis_max_ratio/100`.
- Result = `(#blocks with dis > thr) * 100 / #blocks in window`, or 0 when the window is empty.
- Cost: about 768 × 40 soft-float operations for each sample, taken 10 times per night evaluation.
- Stock reads the AWB stats twice, once here and once inside blk_lumi. The port uses a single buffer. [H on stock]

## 3. Polling cadence (app thread 0xd7a14, started by `ht_night_mode_init` when `ps_mode` selects soft) [H]
```
loop:
  if ctx+0x18 or ctx+0xec: sleep 100; continue          (paused / other mode; meaning M)
  r = ak_vpss_get_soft_ps_level(0, cur(ctx+0xcc), &out)  (blocking, see 2.2-2.4)
  ak_vpss_get_ev(0, &ev)                                 (log only)
  if r != 0: sleep 200; continue                         (AE not stable or error)
  if out != cur: ht_video_md_pause(5000); if ctx+0xc == 0: switch(out)
  sleep 100
```
One evaluation takes 1 frame of AE check (when stable) plus 10 frames of sampling, then 100 ms: about 0.77 s at 15 fps
and about 1.1 s at 10 fps. A switch at day→night therefore needs about 10 consecutive dark frames in a single
evaluation, with no averaging across evaluations.

## 4. What happens on a switch (FUN_000d6bc4 → FUN_000d6ad0) [H on order/delays, M on which GPIO is what]
1. `ht_video_md_pause(5000)`: motion detection is suppressed for 5 s.
2. sleep 500 ms.
3. To day: `FUN_d64d8(0)` (IR-cut, pulsed GPIO with usleep, M) → `ht_video_codec_switch_mode(0)` (ISP day
   config/frame-rate) → `ht_video_detect_set_mode(1)` → `FUN_d688c(0)` (IR LED, M).
   To night: `FUN_d688c(1)` → `ht_video_detect_set_mode(0)` → `ht_video_codec_switch_mode(1)` → `FUN_d64d8(1)`.
4. sleep 300 ms. Then `cur = out`. Then sleep **2500 ms**. Then the 100 ms loop sleep.

So after a switch, nothing is evaluated for about 3.4 s, and MD is muted for 5 s.

`ht_night_mode_set` (manual/other path): under some mode flag (white-light/full-colour mode, **L**) it replaces
p0/p1 with alternate values from the app config struct (+0x60/+0x64) and calls `set_soft_ps_param` again. With the
default ini this does not appear to happen. [M]

## 5. Port notes
- `sdn_step()` replaces the blocking loops. Call it once per frame (`st.frame_ms`) with that frame's EV, its AE
  classification and, at night, the ratio. It returns BUSY / DAY / NIGHT / NOT_STABLE. After a decision, wait 100 ms
  (and do the switch sequence) before the next call. After NOT_STABLE, wait 200 ms. Timing inside an evaluation
  matches stock to within one frame.
- Build with `-ffp-contract=off` and without `-ffast-math`. Every double op is in the stock order, so results should
  be bit-exact with the soft-float code.
- Smoke test (host): all-grey blocks give a ratio of 0, half coloured give 50, all coloured give 100; EV at 1x gains
  and exp 2284 is 584704. This has not been validated against the device.
- Division-by-zero cases (exp_step == 0, EV == 0, n == 0) are guarded in the port. Stock would hit
  `__aeabi_uidiv(x, 0)`.

## 6. Not determined
- The meaning of run_info bytes +1/+2 (which one is the current luma and which the target), and of byte +0
  (ae.c logs it as luma). Only `|b1-b2|` matters to the port. [L]
- The exact semantics of the AWB header totals (index [9] of which array, and whether they are a statistics window or
  the whole frame). Only their ratios are used. [M]
- What ctx+0x18 / ctx+0xec / ctx+0xc in the app thread are (likely pause, hardware-photosensor mode and manual
  override). [L]
- Which of FUN_d64d8/FUN_d688c is the IR-cut and which the IR LED. d64d8 pulses two GPIOs with usleep, which looks
  like an IR-cut solenoid. [M]
- Whether `isp_get_sensor_fps()` returns the night fps (10) after the switch. It is a cached library value. [M]
- No dynamic verification: the port has not been run against real ISP payloads.

## Address index
get_soft_ps_level 0x12a5d0, set_soft_ps_param 0x128a6c, wait_exp_stable 0x1284bc, day_mode_check 0x129260,
night_mode_check 0x129ffc, get_ev 0x128fd8, get_blk_lumi 0x13d2dc, get_non_infrared_ratio 0x13d538,
get_rgb_dis 0x129b7c, lock thread 0x126ae4, get_awb_stat_info 0x1342d0, get_ae_run_info 0x132e7c-ish (wrapper of
AK_ISP_get_ae_run_info 0x149eb8), AK_ISP_get_exposure_attr 0x149ea0, Ak_ISP_get_awb_stat_info 0x149fa8,
Ak_ISP_get_awb_run_info 0x149fc0, ak_vi_get_dev_attr 0x140124, app thread 0xd7a14, app switch 0xd6bc4/0xd6ad0,
ht_night_mode_init / ht_night_mode_set 0xd7f.. / 0xd80..; unused: white_led_night_mode_check 0x129464,
ak_vpss_get_soft_wh_level 0x129958. Globals: base 0x5f3290; params at +0xe4 (0x5f3374); per-dev state at
+0x54+dev*0x24 (+0x58 thread, +0x5c lock, +0x60 lock_req, +0x68 switch_cnt, +0x6c last_ev, +0x70 lock time),
+0x290 "params set".
