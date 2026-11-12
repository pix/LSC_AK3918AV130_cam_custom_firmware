<!-- Reverse-engineering notes for the stock anyka_ipc (static analysis of out/app_extracted). Confidence tags: [H] read from instructions, [M] inferred, [L] guess. -->
# Stock anyka_ipc RE spec: MD, anti-flicker, privacy mask, WDR/NR, light level, sub-stream, JPEG

Static analysis only (Ghidra headless on `out/app_extracted/bin/anyka_ipc`, objdump of
`modules/ak_isp.ko`, `ak_venc_adapter.ko`, `ak_venc_bridge.ko`, plus the stock ini files in
`out/app_extracted/local/`). All addresses are for THIS anyka_ipc build (`nm -D` addresses).
Note: several address comments in `ak_ioctls.h`/`isp.c` (e.g. `AK_ISP_set_nr1_attr @ 0x00145920`,
`isp_set_effect @ 0x127e40`) do not match this binary (here: `AK_ISP_set_nr1_attr` = 0x14a0e0,
`isp_set_effect` = 0x12c600); the inner command constants do match.

Tunnel = `ioctl(g_isp_fd, 0xc0cc5616, {flag=1, inner_cmd, payload})` (= `isp_sdk_ioctl` 0x14a5bc).
Confidence tags: **[H]** high, **[M]** medium, **[L]** low.

Useful data found on the way:
- `out/app_extracted/local/_ht_hw_settings.ini` has stock's encoder table (main/sub), MD levels
  and all soft-photosensor (day/night) parameters. `_ht_sw_settings.ini` has the user defaults
  (`light_frequency = 0`, `wdr_switch = 0`, `md_switch = 1`, `md_sensitivity = 2`).

---------------------------------------------------------------------------------------------
## 1. Motion detection

### 1.1 Data source: the 3D-NR motion statistic [H]
`ak_vpss_md_get_stat(dev, buf)` 0x1324b4 → `isp_get_statinfo(dev, 0x20, tmp[0x608])` 0x12c0dc →
`AK_ISP_get_3d_nr_stat_info` 0x14a2c0 → **inner cmd `0x80044970`** (= `ISP_INNER_GET_3D_NR_STAT`,
already in ak_ioctls.h), payload **0x608 bytes**. ak_vpss copies `tmp+2 .. tmp+0x602` (0x600 B).

Layout of the 0x608-byte payload:

| offset | size | meaning |
|---|---|---|
| 0x000 | u16 | unknown header (skipped by stock) [M] |
| 0x002 | u16[24][32] | per-block motion value, row-major, 32 cols x 24 rows, row stride 64 B |
| 0x602 | 6 B | unused by MD |

Kernel side (`_get_3dnr_stat` ak_isp.ko 0x2e10c): copies 24 rows x 16 u32 from the HW stat buffer
(split into 2 u16 each) to `isp+0x51a2`, so the grid is **fixed in hardware at 32x24** whatever
the resolution. On 1920x1080 a block is 60x45 px. The kernel picks one of 4 HW buffers; there is
no argument validation (any buffer of 0x608 bytes works). The values only make sense while 3D NR
(TNR) is running. Our ae.c already sets `ISP_INNER_3D_NR_REF {1920,1080}`, and the conf's
module 0x04 sets TNR on. The 3D-NR set has `tnr_ref_statistic_en` (u16 index 0xcf) and `md_th`
(u32 at u16 index 0xcc), which probably gate or scale this statistic [L].

`isp_get_statinfo` type 0x18 also returns the same 0x608 B plus `AK_ISP_get_3d_nr_ref_size_stat_info`
(`0x80044985`, 0xc B) appended.

### 1.2 Stock algorithm (libak_md + libmrd, statically linked) [H unless noted]
Call chain: `ht_video_detect_init` 0xc3aa8 → `FUN_000c2bc0` (per chn: `ak_md_init`,
`ak_md_set_sensitivity(c, level)`, `ak_md_set_area(c, all-ones 32x24)`, `ak_md_enable(c,1)`,
then creates the ht poll thread `FUN_000c27a0`).

`ak_md_init` 0xaceb4 (per-chn ctx 0x344 B at 0x5cd26c, result 0x74 B):
- md fps = 10 → the md thread `FUN_000aca8c` sleeps `1000/fps` = **100 ms** per iteration.
- rows = 24 if `sensor_height % 24 == 0` else 16 (1080 → 24); cols always 32.
- `ak_mrd_set_filters(h, hi, lo)`: if w*h ≤ 3,500,000 → **hi=10000, lo=5000**, else 12000/6000.
  (`ak_mrd_init` defaults are 7000/4000. `ak_md_set_filters` requires hi ≥ lo.)
- `ak_mrd_set_motion_region_size(h, min, max)`: min = `sens_table[sensitivity]`, max = **300** cells.
- `ak_mrd_set_mode(h, 1)` + `ak_mrd_set_floating_wadding_params(h,3,2,4,36,2,3)` (day defaults).

Per-frame detection `FUN_000ac580`:
1. `ak_vpss_md_get_stat` → u16 stat[rows][32].
2. `ak_mrd(h, stat, rows, regions[20])` 0x1dee88: every cell with `stat >= hi` (10000) is a seed.
   Seeds are grown into connected regions by `mrd_expand` (presumably with the `lo` threshold [M]),
   then reduced to bounding boxes (col0,row0,col1,row1, inclusive, block units, 0-based after the
   final `-1`). A region is kept only if its bbox area in cells is in **[min, max]** = [sens_table[s], 300].
   Up to 20 regions, sorted by area (largest first).
   In **mode 0** (night) the regions also go through `mrd_alarm` (multi-frame "floating wadding"
   filter against insects or dust in IR). In mode 1 (day) they are reported as they are.
   `ak_md_set_day_night_mode(0, m)`: m=0 → floating params (4,2,4,9,2,3); m=1 → (3,2,4,36,2,3).
   Stock calls `ht_video_detect_set_mode(1)` after switching ISP to day and `(0)` when going
   to night (callers at 0xd6b38 and 0xd6b80) [M on which is day].
3. If an area mask was set (`ak_md_set_area`, u8[24][32] of 0/1, 0x300 B), `FUN_000aba64`
   re-scores each region by counting masked-in cells and keeps up to 5 regions whose count is in
   (min-1, max).
4. Result (0x5c bytes, `ak_md_get_result` 0xae614 / `get_md_result` 0xac978, read-once: the
   stored result is cleared after being copied):

| off | type | meaning |
|---|---|---|
| 0x00 | u32 | valid flag (1) |
| 0x04 | u32 | time of detection (`ak_date_to_seconds`) |
| 0x08 | i32 | region count (≤5) |
| 0x0c + 16*i | i32[4] | {x0, y0, x1, y1} in block units (0..31, 0..23) |

5. md thread latch: a positive frame sets the flag and posts a semaphore. The flag is
   **cleared 4 s after the last detection**.

Sensitivity table (101 entries, `md_set_sensitivity` 0xab984 reads `*(0x5c25f0)` → 0x410480):
sens 0→50 cells, 20→30, 40→15, 50→12, 60→11, 80→8, 100→4 (full table: read 101 x i32 at
vaddr 0x410480). Lower min-area = more sensitive.

UI mapping: `_ht_sw_settings.ini md_sensitivity` 0..4 → `_ht_hw_settings.ini [alarm] md_level_N`
= **20/40/60/80/100** → `ak_md_set_sensitivity`. Default index 2 → level 60 → min area 11 cells.
(`ht_video_set_md_sensitivity` 0xc3d54, table loaded by `ht_hw_param_alarm_get` into 0x5cff58.)
Tuya rect "md_area_rect = x,y,w,h in %" → `ht_sw_param_config_set(0x23)` → u8[24][32] mask.

ht poll thread `FUN_000c27a0`: every **100 ms** calls `ak_md_get_result(0, res, 0)`. On `count>0`
it calls the md callback(1, ...) and then suppresses further alarms for `interval` seconds
(default **20 s**, `ht_video_set_md_interval`). `ht_video_md_pause(ms)` mutes it for N ms. Stock
pauses MD for 5000 ms around the day/night switch (0xd7b20).

### 1.3 Recipe for ak_rtsp [M]
Poll `ISP_INNER_GET_3D_NR_STAT` (0x608 B) at 10 Hz. Take `u16 *g = buf+2` ([24][32]).
Mark cells with `g >= 10000`, AND them with the user mask, and grow 8-connected regions over
cells `>= 5000`. Report motion if any region's bbox area is in [min_cells(sens), 300]. Optionally
require N of M frames at night. Hold the motion state for 4 s.
Expect to re-tune thresholds on hardware, because the scale of the stat values is not verified.

---------------------------------------------------------------------------------------------
## 2. Anti-flicker 50/60 Hz

### 2.1 How stock selects the frequency [H]
- User setting `light_frequency` (sw ini key 7): 0=off, 1=50 Hz, 2=60 Hz. Default **0**.
- `ht_sw_param_set_light_frequency` 0xe33ec: `v==2 ? 60 : 50`, so **"off" and 1 both give 50 Hz**.
  Stock never disables anti-flicker. → `ht_video_codec_set_light_frequency(50|60)` 0xc1bcc →
  `ak_vpss_effect_set(dev, 7, hz)` 0x126e58 → `isp_set_hz(dev, hz)` 0x12e7a8. Only 50 and 60 are
  accepted.
- Global hz `*(0x5c5484)` is initialised to **50** in .data, so 50 Hz is active from boot via
  `isp_module_init` → `set_exptimemax_by_curfps`.
- `tkl_vi_isp_set_antiflicker` is a stub (returns -4).
  `ak_vpss_set_anti_flicker_strength` and `ak_vpss_set_force_anti_flicker_flag(_ex)` have **no
  callers** in stock.

### 2.2 What it actually writes: the exposure attr (`ISP_INNER_GET/SET_EXPOSURE`, 0x720 B) [H on code, M on field names]
`isp_set_hz` → `FUN_0012d658` (sensor fps) + `set_exptimemax_by_curfps` 0x12b548:
GET_EXPOSURE (0x8004498b) → modify → SET_EXPOSURE (0x8004498a). Offsets are absolute in the
0x720 buffer. `pub = buf+0x1c` as in ae.c.

| abs off | pub[] | written as |
|---|---|---|
| 0x1c | pub[0] | **exp_time_max** = max exposure lines at current fps (`L`, see below) |
| 0x20 | pub[1] | **exp_time_min**; = exp_step only when force flag == 1 |
| 0x3c | pub[8] | **exp_step** = lines per flicker period |
| 0x4c | pub[12] | anti_flicker_target_lumi (only force mode 2) |
| 0x58 | — | AE target lumi (read by strength calc) |

exp_step formula (`F` = sensor fps, `L` = exposure lines per frame at F):
- 60 Hz: `F∈{12,13}: L*125/1200`, else `L*F/120`.
- 50 Hz: `F∈{12,13}: L*125/1000`. If `F==14` or (`F==15` and force≠0): `L*1428/10000`, i.e. the
  sensor is dropped to ~14.28 fps (70 ms = 7 half-periods; `FUN_0012d658` sets sensor fps 14 instead
  of 15 at 50 Hz when forced). Else `L*F/100`.
- If the calibration's original exp_step (saved copy `fps_cfg+0x120`) == 1 and force==0,
  stock writes exp_step = **1** (anti-flicker effectively off). So the calibration file decides
  whether stock anti-flicker is on at all. Check ae.c's boot log `pub[step=...]`.

`L` comes from the sensor driver: `vi_dev_get_exptime_for_fps` 0x149bd0 →
`ioctl(g_subdev, 0xc0085300, u32[2]{fps_in, lines_out})` (same fd as ISP_SENSOR_INIT 0x40085301) [M on the fd].
For our day config (VTS 2284 @15 fps) expect roughly L≈2284: 50 Hz → step ≈ 342, 60 Hz → step ≈ 285.
Query the ioctl instead of hardcoding.

Force modes (`isp_set_force_anti_flicker_flag` 0x12ec90; mode stored at `fps_cfg+0xe0`):
- 0 off: restore pub[1], pub[12] (and pub[8] if the calibrated step==1) from the saved calibration copy.
- 1 force: pub[1] = pub[8], so exposure is never shorter than one flicker period (bright scenes may
  overexpose).
- 2 "strength" (`ak_vpss_set_anti_flicker_strength(dev, s)` s=0..100 → mode 2 when s>0):
  pub[12] = `(T*(100-s) + 100*s)/100` with T = u32 at 0x58. The kernel AE presumably leaves the
  flicker-step grid only when luma exceeds this [L on the kernel behaviour].
Every change re-runs `set_exptimemax_by_curfps` and the sensor-fps setter.

### 2.3 Recipe [M]
On a frequency change: GET_EXPOSURE, set `pub[8] = L*fps/100` (50 Hz) or `/120` (60 Hz). Keep
pub[0]=L and leave pub[1] (stock value 2) unless the user wants "force". Then SET_EXPOSURE.
Re-apply after every fps change (night.c switches to 10 fps / VTS 3426).
**Side finding (ae.c) [M-H]:** ae.c labels pub[1] "emax/exp_time_max", but stock's
`set_exptimemax_by_curfps` writes the fps-dependent maximum into **pub[0]** (0x1c), and force mode
copies exp_step into pub[1], which only makes sense for a minimum. The stock value 2 for pub[1] fits
exp_time_min = 2 lines. ae.c only writes the stock value back, so this is harmless today, but
don't "raise" pub[1] thinking it is the max.

---------------------------------------------------------------------------------------------
## 3. Privacy mask

Stock never calls `ak_vpss_mask_*` and `tkl_venc_set_mask` is a stub. The validation and layout
below come straight from the vpss code and the kernel.

### 3.1 Areas [H]
`ak_vpss_mask_set_area(dev, area[4])` 0x131a70; user struct 4 x 10 B:

| off | type | meaning |
|---|---|---|
| 0 | u16 | start_x |
| 2 | u16 | end_x |
| 4 | u16 | start_y |
| 6 | u16 | end_y |
| 8 | u8 | enable (0/1; >1 rejected) |
| 9 | u8 | pad |

- **4 areas.** Coordinates are in **MAIN channel pixels** (`ak_vi_get_chn_attr(0)`: w at +8, h at +0xc).
- Checks per enabled area: `start_x<W, end_x<W, start_y<H, end_y<H, end_x>start_x, end_y>start_y`.
  Disabled areas are sent as all-zero 10-byte entries. There is no per-area enable bit on the wire.
- Main: `isp_set_main_mask_area` → inner **`0x4004495a`**, payload **0xa0 bytes** (kernel copies
  160 B; only the first 0x28 = the 4 entries are used, rest zero).
- Sub: the same rects scaled `x*subW/mainW`, `y*subH/mainH` (integer div) → inner **`0x40044965`**,
  same 0xa0 payload. **Both must be sent**: the main and sub channels are masked independently.
- Getters: `0x8004495b` / `0x80044966`.
- Kernel (`node_pp_set_channel_mask_area_attr` 0x1e9f0): writes PP regions **12..15** as
  `{start_x, start_y, end_x, end_y}`. It does not check bounds (only the userland does). OSD uses
  other region slots, so there is no conflict with osd_hw.c [M].

### 3.2 Colour / mosaic [H on layout, M on semantics]
`ak_vpss_mask_set_color(dev, c)` 0x131ee4; user struct 10 B:
`{u8 color_type (0=solid colour, 1=mosaic); u8 alpha; u8 r, g, b; u8 pad; u16 mosaic_size_hor (0..7);
u16 mosaic_size_ver}`.
RGB is converted to YUV: `Y=(306R+601G+117B)>>10`, `U=((512B-173R-339G)>>10)+128`,
`V=((512R-429G-83B)>>10)+128`, clamped.

`isp_set_mask_color` 0x131440 builds a **0x68-byte** payload:

| off | size | meaning |
|---|---|---|
| 0x00 + 6*i (i=0..3) | u8 mode, u8 pad, u16 idx | per area. Solid: mode=2, idx=19 (palette entry). Mosaic: mode=1, idx=min(alpha,19) |
| 0x18 | 0x48 | zero |
| 0x60 | u16 | mosaic_size_hor (mosaic mode; 0 in solid) |
| 0x62 | u16 | mosaic_size_ver (copied; the kernel main handler doesn't read it) |
| 0x64 | u32 | 0xffffffff |

Sent with inner **`0x80044992`** (main) and **`0x80044994`** (sub), both always (note the
`0x8004...` "read-style" encoding is used for a SET here). Getters `0x80044993`/`0x80044995`.
Kernel `node_pp_set_channel_mask_info_attr` 0x1ebcc reads only byte0 and u16@2 of each entry plus
u16@0x60.

Solid mode additionally sets **palette entry 19** first, through the same OSD user-params path
osd_hw.c uses: `Ak_ISP_Set_User_Params` = inner `0x4004496d`, 0x84-byte block:
`block[0]=cmd 0x4005490a`, `u32 @0x50 = (alpha<<24)|(Y<<16)|(U<<8)|V` (same AYUV order as
k_palette), `u32 @0x54 = 2`. Kernel `node_pp_set_osd_mask_color_table_attr` 0x2f560 treats
`payload+0x50` (= block+0x54) as a selector: 0 → entries 0..15 (OSD palette, what osd_hw.c sends),
1 → entries 16..18 from payload+0x40, 2 → only entry 19 from payload+0x4c. So **mode 2 does not
disturb the OSD palette**. osd_hw.c's existing call is selector 0 (zeroed). [H]
Recommended order: palette(sel 2) → MASK_COLOR main/sub → MASK_AREA main/sub. To disable
everything, send all-zero areas. Sending entry mode 0 (the kernel default after memset) probably
also disables it [L].

---------------------------------------------------------------------------------------------
## 4. WDR, NR, sharpness

Stock UI exposes only WDR (on/off) and sharpness (we already have sharpness). There is no NR UI.
All of these modules are linkage sets: `u32 mode (0 manual, 1 linkage)`, then 1 manual set,
then 16 linkage sets indexed by gain level. To change "one field" safely: GET, patch the field in
**all 17 sets**, then SET.

| module id (conf) | set / get inner | payload | set size | "simple" field |
|---|---|---|---|---|
| 0x0a WDR | 0x4004491b / 0x8004491c | 0x4228 | 0x3e4 | `wdr_enable` u16 @ set+4 (abs **+8**, +0x3e4*i) |
| 0x04 3D NR | 0x4004492f / 0x80044930 | 0x1ba4 | 0x1a0 | `tnr_enable` u16 @ set+0 (abs **+4**, +0x1a0*i) |
| 0x03 NR1 (conf +4) | 0x4004490c / 0x8004490b | 0xbf8 | 0xb4 | `nr1_enable` u16 @ set+0 (abs +4), `nr1_k` u16 @ set+2 |
| 0x03 NR2 (conf +0xbfc) | 0x4004492b / 0x8004492c | 0x3e0 | 0x3a | `nr2_enable` u16 @ set+0 [M], `nr2_k` further in |
| 0x03 UVNR (conf +0xfdc) | 0x4004497d / 0x8004497e | — | — | not analysed |
| 0x0b sharp | 0x40044925 / 0x80044926 (+ex 0x40044927/0x80044928) | 0xb5f8 | — | done in isp.c |

### WDR detail [H]
`check_wdr_params` 0x134448; per set (u16 index): [0] hdr_uv_adjust_level <32, [1] hdr_cnoise_suppress_slop ≤128,
[2] **wdr_enable 0/1**, [3..7] wdr_th1..5 <256, [8..] area_tb1..6 each 65 entries ≤1023, then
area1..6_key[16] ≤64, [0x1ee] hdr_uv_adjust_enable, [0x1ef..0x1f1] cnoise yth1/yth2/gain.

Stock WDR switch (`ht_sw_param_set_wdr_switch` 0xe3cdc): on → `ht_video_codec_set_wdr(100)`,
off → `(50)` → `ak_vpss_effect_set(dev, 5, v-50)` → `FUN_0012b0d0`. That function does **not**
touch wdr_enable. It rescales the six 65-point `area_tb` curves in all 17 sets, using the first
GET_WDR as a cached baseline B and the identity curve D = `default_wdr_para` (0x425d24:
0,16,...,1008,1023):
`k = (v+50)*1024/50` (0..2048); `new = (k*B + (1024-k)*D) >> 10`, clamped to 1023.
So stock "off" (v=0) = the calibrated curve, "on" (v=+50) = twice the calibrated deviation from
linear, and v=-50 = linear (no WDR). Reproduce this exactly (same baseline-once pattern as
isp_set_sharpness). It gives a safe 0..100 slider.

### NR notes [M]
- 3D NR per-set fields (u16 idx): [0] tnr_enable, [1] update_ref, [2] tnr_refFrame_format,
  [3] t_mf_th1 ≤0x1fff, [4] t_mf_th2, [5] t_ex_mf, statScale tables, [0x97] ynr_k, [0xaa..] ylp_k1/2,
  t_y_*..., [0xb8] uvnr_k, ..., [0xca] motion_filter, u32 [0xcc] md_th, [0xce] tnr_debug_output,
  [0xcf] tnr_ref_statistic_en. **Don't disable TNR** if you use MD (§1).
  `ak_vpss_set_3d_nr_attr` copies manual (0x1a0) + linkage (0x1a00) into a GET buffer before SET.
- NR1 strings: nr1_mode, nr1_enable, nr1_k, nr_keepth, nr1_lc_lut_key/lc_lut, weight r/g/b tables.
  NR2: nr2_mode, nr2_enable, nr2_pass2_enable, nr2_k, ynr_keep_th, ynr_intensity1/2, weight_tbl,
  y_dpc fields. The `*_k` fields are the natural "strength" knobs. Their ranges weren't extracted
  (see `ak_vpss_set_nr1_attr` 0x139ec0, `ak_vpss_set_nr2_attr` 0x13a40c string table).
- Module payload sizes (`Isp_Struct_len` 0x5c5488, u16[24], include the 4-byte module header):
  316,212,888,4780,7080,144,22,1750,328,180,16940,46608,348,108,8,14,212,1504,46,7208,1852,42,306,2208.

---------------------------------------------------------------------------------------------
## 5. Light level / day-night (lower priority, mostly [M])
Stock uses the **soft photosensor**, not `ak_vpss_get_auto_day_night_level` (that one has no live
caller):
- `ak_vpss_set_soft_ps_param(dev, p[23])` 0x128a6c, 0x5c bytes, in ini order:
  day_to_night_lum (6.3e6), night_to_day_lum (2e6), awb_night_dis_min (50), awb_night_dis_max (850),
  day_check_frame_num (10), night_check_frame_num (10), exp_stable_check_frame_num (10),
  stable_wait_timeout_ms (60000), gain_r/offset_r/gain_b/offset_b/gain_rb/offset_rb (1280/2048 x3),
  blk_check_flag (1), low_lumi_th (5), high_lumi_th (245), non_infrared_ratio_th (40),
  highlight_lumi_th (240), highlight_blk_cnt_max (192), lock_cnt_th (5), auto_lock_time_ms (60000),
  revised_dis_max_ratio (5).
- `ak_vpss_get_soft_ps_level(dev, cur_state, &out)` 0x12a5d0: waits for AE stable, then runs
  `day_mode_check` 0x129260 (when currently day) or `night_mode_check` 0x129ffc (currently night).
  The caller loop (0xd7b20) runs it every ~100 ms.
- EV (`ak_vpss_get_ev` 0x128fd8) from AE run info (inner `0x80044952`, 36 B; u32 at +4,+8,+0xc,+0x10):
  `EV = r10 * ((r0c * ((r04 * r08) >> 8)) >> 8)`, then increased by `|b1-b2|` percent (bytes 1,2 of
  run info, current vs target luma) when gains are below the AE max. Larger EV = darker.
  Day→night when EV > day_to_night_lum. Night→day when EV < night_to_day_lum **and**
  `non_infrared_ratio > 40` (blk_check_flag=1) for all `night_check_frame_num` consecutive
  samples, with a lock of auto_lock_time_ms after a switch (released early if EV moves >9%).
- `ak_vpss_get_blk_lumi` 0x13d2dc: 32x24 u32 block luma from AWB stats (`Ak_ISP_get_awb_stat_info`
  inner `0x80044959`, 0x24a0 B): `Y = ((R/n>>2)*38 + (G/n>>2)*75 + (B/n>>2)*15) >> 7` per block,
  where `n = (W/32)*(H/24)`. `ak_vpss_get_non_infrared_ratio` 0x13d538 (not analysed in detail) uses
  these blocks plus the gain/offset params to estimate the share of non-IR (coloured) light.
- `ak_vpss_get_cur_lumi` = `isp_get_cur_lum_factor` 0x12ee6c (not analysed).
night.c's hw_exp proxy is a reasonable stand-in. Matching stock exactly would need `night_mode_check`
plus `ak_vpss_get_non_infrared_ratio` (~480 decompiled lines) ported.

---------------------------------------------------------------------------------------------
## 6. Second (sub) H.264 stream

### 6.1 What stock does [H]
`_ht_hw_settings.ini [sub_video_0]`: **640x360, 15 fps, gop 30, target 512 / max 512 kbps,
br_mode 2 (AVBR → wire VBR+), minqp 20, maxqp 40, initqp 90, level 50, H.264, bind_flag = 0**.
`ht_video_codec_start_encode(chn=1, idx)` 0xbf778 builds the 0x40-byte `venc_param` and:
- bind_flag 0 (sub) → `ak_venc_open()` = `ak_venc_open_ex(attr, NULL)`, so **no RC struct
  (rc_flag=0)**. The thread `FUN_000be2a0` does **user-mode encoding**: `ak_vi_get_frame(vi_chn 1)`
  → `ak_venc_encode_frame` → `ak_venc_release_stream`. There is no BIND_VI_CHN for the sub stream.
- bind_flag 1 (main) → `ak_venc_open_ex(attr, rc)` with rc = 7 words, all 0 except word[6]=1. The
  thread `FUN_000be668` binds through `ak_app_video_bind_chn({7,vi},{0xd,venc},{fps,200,1})`.

venc_param (attr) layout as validated by `FUN_001d4a1c` (format → CREATE_ENC):
`u16 w@0, h@2, fps@4, gop@6, target_kbps@8, max_kbps@0xa; u32 profile@0xc (0 Main77, 1 High100,
2 Base66, 3 CHigh0x242, 7=JPEG); u32 br_mode@0x10 (0 CBR→rc1, 1 VBR→rc2, 2 →rc0, 4 VBR+→rc5);
u16 initqp@0x14 (10..100), minqp@0x16, maxqp@0x18 (min≤max≤51); u32 @0x1c (<4); u32 chroma@0x20 (=1);
u32 enc_out_type@0x24 (0 H264, 1 JPEG); u32 stream_buf_KB@0x28 (0=auto, ≤0xc800); u32 level@0x2c (10..62);
u32 smart_mode@0x30; u16 smart_gop@0x34 (=100), smart_quality@0x36 (=50), smart_static@0x38; u32 @0x3c (0,2,3)`.
ini br_mode 0→0, 2→4, else→1.

### 6.2 Exact sub-channel wire sequence (from ak_venc_open_ex 0x1d9200) [H]
1. Global `/dev/venc` fd (already open). Pick the first free slot → **chn index 1** (main = 0;
   stock's JPEG snapshot encoder takes the next free slot, 2).
2. `QUERY_CHN 0x406056ce` on the global fd: 0x70-byte struct with name `"venc-chn1"` at offset 0.
   Stock requires `u32 @0x6c == 1` on return.
3. `open("/dev/venc-chn1", O_RDWR)`. **Every channel has its own fd**; pools/mmap/ioctls go on that fd.
4. `QUERY_DMA 0x805456ec` (global fd), u32[21]: {profile 77, chroma 1, u16 w=640, u16 h=360,
   **u16 @12 = 1** (stock; venc.c uses 60), u16 @14 smart_mode 0, u32 @16..24 = rc I/P/B pic size (0),
   u8 @28 = 1, u8 @29 = rc.flag (MMA) = **0 for sub**, u8 @32 = rc.flag2 (MVV) = **0 for sub**}.
   Ring pool = Σ align32(block[i]).
5. `CREATE_POOL 0x402056ed` {create=1, is_external=0, size@0x1c = ring}, then
   {create=1, is_external=1, size = ext}. ext = attr@0x28 ? KB<<10 :
   align32(max(w*h ≤ 0x38400 ? 0x20000 : w*h*3/8, max_kbps*256)). **640x360 → 131072 B**
   (main: 777600 = ENC_VENC_SIZE). mmap(ext, PROT_RW, MAP_SHARED, chn_fd, 0). Outputs @0x10/0x14 as in venc.c.
6. `CREATE_ENC 0x406056e0` (96 B): chn_id=**1**; enc: codec 77, chroma 1, w 640, h 360,
   rc_mode **5**, fps 15, gop 30, @0x1c = 0, level **50**, qp_or_kbps(minqp)=**20**, qp2(maxqp)=**40**,
   max_kbps_vbrp=**512**, initqp_vbrp=**90**; smart {0,100,50,0}; **rc_flag=0, rc all zero**.
   With rc_flag=0, the rc fields that crashed main (venc.c history) should not be read [M].
7. GET_STREAM `0x402056ea`: {chn=1, maxlen=**0x40000** (stock uses 256 KiB when w*h < 0xe1000)}.
   Output kaddr@0x18 → user ptr = mmap_base + (kaddr - pool.phys_base). len@4, pts@8/0xc,
   seq@0x10, type@0x14 (1 P→0, 2 I→1, 3 B→2). RELEASE_STREAM `0x402056ff` as for main.

### 6.3 Feeding frames: two options
**(a) Stock way, user mode [H on the ioctl, L on doing it with our V4L2 buffers]:**
`ENCODE_FRAME = 0x463856e6` (`_IOW('V',0xe6,0x638)`) on the chn fd, 0x638 B:

| off | meaning |
|---|---|
| 0x00 | chn_id |
| 0x04 | frame user vaddr (NV12) |
| 0x08 | frame **phys** addr (`ak_mem_dma_vaddr2paddr`) |
| 0x0c | len |
| 0x10 | dma-buf/ION **share fd** of the frame buffer |
| 0x14 | md_valid |
| 0x18 | u16 cols (0x20) |
| 0x1a | u16 rows (24/16) |
| 0x1c | 0x600 md stat (optional ROI hint, the VI frame carries it at frame+0x20) |
| out 0x620..0x634 | kaddr, len, ts lo/hi, seq, type, same as GET_STREAM |

This needs the VI frame in app-owned ION/DMA memory with a share fd. Our vi.c uses V4L2 MMAP
buffers, so we'd need their phys address and an fd. Not verified.

**(b) Kernel bind on VI chn 1 [M-L, untested by stock]:**
`BIND_VI_CHN 0x401456e1` on chn1 fd {venc_chn=1, vi_packed = (dev<<8)|1, param0 = **15 (target fps)**,
param1 = **200**, encode_mode 1}, then `ACTIVATE 0x400456e2`.
The adapter (`venc_chn_dev_ioctl`, case @0xc85c) takes `vi_chn & 1`. The bridge
(`b2e_encode_bind_isp_channel`) accepts dev/chn ≤2 and keeps a **list** of encoders per ISP
channel. The codec may be H.264 (77/100/66/0x242), H.265 (0x1000001/0x1fa0004) or **JPEG
(0x4000000)**.
**param0 is the encoder fps limit** (adapter calls bridge op+0x20 with `1000000/param0` µs).
Open risks: VI sub currently runs in FRAME done_mode for DQBUF (the main bind uses SLICE mode), and
kernel-bound consumption may starve or conflict with vi.c's V4L2 DQBUF on the same channel.
Try (b) first on hardware (behind a guard file, per the test-first rule).

### 6.4 Differences vs our venc.c (main) [H on values]
- `venc_bind_vi()` passes `param0=1920, param1=1080`. Stock passes **{fps=15, 200}**. With 1920 the
  kernel interval is 520 µs, i.e. no rate limiting. Harmless but semantically wrong. Use the fps for
  the sub stream.
- QUERY_DMA u16@12: stock 1, venc.c 60.
- Stock main uses rc_flag=1 with an rc struct that is almost all zero (venc.c notes it crashed for us).
  The sub stream sidesteps this because stock uses rc_flag=0.

---------------------------------------------------------------------------------------------
## 7. Hardware JPEG [H on stock flow, M on 1080p]
Stock snapshots (`ht_video_codec_snap_picture` 0xc0358, always called with chn **1**) are **640x360**.
Stock never makes a 1080p JPEG. Flow: `ak_venc_open(attr copy with fps=1, profile=7,
enc_out_type=1)` lazily (third channel, `/dev/venc-chn2`). Then per snapshot: `ak_vi_get_frame(sub)`
→ `ak_venc_encode_frame(jpeg_chn, frame)` (ENCODE_FRAME 0x463856e6) → copy the stream →
release stream and frame.
JPEG CREATE_ENC enc cfg: `codec = 0x4000000`, chroma 1, w, h, **u16 @0x14 = quality = attr initqp
(10..100; sub ini = 90)**. rc/fps fields are unused. JPEG limits: w ≤ 0x4000, h ≤ 0x2000, w%32==0.
For 1080p you'd either bind a JPEG channel to VI chn 0 in kernel mode (adapter accepts codec
0x4000000 in BIND; the bridge allows several encoders per ISP channel [L]), or ENCODE_FRAME a main
frame (not possible today, because main is slice-mode and kernel-bound).
Also present: `ak_venc_set_jpeg_slice` / `ak_venc_jpeg_slice_encode` (not analysed).

---------------------------------------------------------------------------------------------
## Address index (this binary)
MD: ak_vpss_md_get_stat 0x1324b4, isp_get_statinfo 0x12c0dc, AK_ISP_get_3d_nr_stat_info 0x14a2c0,
ak_md_init 0xaceb4, md thread 0xaca8c, detect 0xac580, area filter 0xaba64, md_set_sensitivity 0xab984,
sens table 0x410480, ak_mrd 0x1dee88, ak_mrd_init 0x1ddf30, ak_md_set_area 0xae1fc, ak_md_get_result 0xae614,
ak_md_set_day_night_mode 0xadf48, ht init 0xc2bc0, ht poll thread 0xc27a0, ht_video_set_md_sensitivity 0xc3d54;
kernel _get_3dnr_stat 0x2e10c.
Flicker: ak_vpss_effect_set 0x126e58, isp_set_hz 0x12e7a8 (hz @0x5c5484), set_exptimemax_by_curfps 0x12b548,
FUN_0012d658, isp_set_force_anti_flicker_flag 0x12ec90, isp_calc_anti_flicker_target_lumi 0x12ea50,
ak_vpss_set_anti_flicker_strength 0x128244, vi_dev_get_exptime_for_fps 0x149bd0, fps_cfg 0x5f3544,
ht_sw_param_set_light_frequency 0xe33ec.
Mask: ak_vpss_mask_set_area 0x131a70, ak_vpss_mask_set_color 0x131ee4, isp_set_main_mask_area 0x131168,
isp_set_sub_mask_area 0x1312d4, isp_set_mask_color 0x131440; kernel 0x1e9f0 / 0x1ebcc / 0x2f560;
kernel copy sizes 0xa0 (area) / 0x68 (color).
WDR/NR: ak_vpss_set_wdr_attr 0x134ab4, check_wdr_params 0x134448, WDR strength FUN_0012b0d0,
default_wdr_para 0x425d24, ht_sw_param_set_wdr_switch 0xe3cdc, check_3dnr_params 0x135540,
ak_vpss_set_nr1_attr 0x139ec0, ak_vpss_set_nr2_attr 0x13a40c, isp_module_init 0x12e0e0, Isp_Struct_len 0x5c5488.
Light: ak_vpss_set_soft_ps_param 0x128a6c, ak_vpss_get_soft_ps_level 0x12a5d0, ak_vpss_get_ev 0x128fd8,
night_mode_check 0x129ffc, day_mode_check 0x129260, ak_vpss_get_blk_lumi 0x13d2dc.
Venc: ht_video_codec_start_encode 0xbf778, ak_venc_open_ex 0x1d9200, attr check FUN_001d4a1c,
query-DMA FUN_001d5448, ak_venc_encode_frame 0x1d9dd8, ak_venc_get_stream 0x1da5bc, ak_venc_bind_vi_chn 0x1dad80,
app_video_bind_vi_venc 0x11b12c, sub thread 0xbe2a0, main thread 0xbe668, snap 0xc0358;
adapter venc_chn_dev_ioctl 0xbffc (BIND case 0xc85c), bridge b2e_encode_bind_isp_channel 0x700.
