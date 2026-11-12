#pragma once
#include <stdint.h>

/* Load the GC20C3 sensor I2C register table from /tmp/sensor_isp.conf and
 * program it into the sensor via ioctl on /dev/v4l-subdev0. */
int isp_load_sensor_conf(void);

/* Program all 24 ISP image-pipeline modules from /tmp/sensor_isp.conf via
 * the tunnel ioctl on /dev/isp-param-0. */
int isp_pipeline_init(void);

/* Program the NIGHT ISP calibration — a real, separate 24-module subfile
 * (mode=1) found concatenated after the day subfile inside the same
 * sensor_isp.conf, not a separate file. See isp.c's comment above
 * isp_pipeline_init_night() for the full discovery trail. Call this
 * instead of isp_pipeline_init() when entering night mode; call
 * isp_pipeline_init() again to restore day. Returns -1 (and logs, without
 * sending anything) if the second subfile isn't present or doesn't
 * validate as mode=1 — never sends unverified data. */
int isp_pipeline_init_night(void);

/* Re-send the AWB_EX sub-payload (5124 bytes at module-0x13 block offset +0x7e4)
 * from sensor_isp.conf. Needed in ae_setup() because 256-byte stub buffers
 * cause EFAULT — the kernel reads all 5124 bytes via copy_from_user. */
int isp_reapply_awb_ex(void);

/* Shared ISP tunnel call — wraps ioctl(g_isp_fd, ISP_IOCTL_CMD, {flag=1,
 * inner_cmd, payload}). Used by ae.c and night.c for runtime AE/WB/frame-rate
 * control (isp.c's own pipeline-init code uses the ISP_SEND macro directly
 * since it iterates over module blocks with computed offsets). */
int isp_tunnel(uint32_t inner_cmd, void *payload);

/* Set manual white balance gains (256 = 1.0x). Used at startup (day WB) and
 * by night.c when switching day/night (night mode uses neutral gains since
 * color is meaningless under IR illumination). */
int isp_set_wb_manual(uint16_t r_gain, uint16_t g_gain, uint16_t b_gain);

/* Live-tunable picture controls (-50..50, matching anyka_ipc's own UI
 * slider range). All GET-modify-SET against real hardware, verified via
 * Ghidra disassembly (not just decompiled C, which proved unreliable for
 * exact field offsets/case numbers in this area — see isp.c's comments on
 * each function for the full trace and what's verified vs. what's a known
 * gap):
 *   - saturation/contrast: small "effect" struct (module 0x0f).
 *   - brightness: the LIVE 0x720-byte exposure-attr struct ae.c's AE
 *     thread also polls — a different, riskier mechanism than the other
 *     three; see isp_set_brightness()'s comment for the safety reasoning.
 *   - sharpness: rescales the ~46KB blob (module 0x0b) from a cached
 *     baseline: block 0 and the 16 per-gain blocks, like stock. */
int isp_set_saturation(int value);
int isp_set_contrast(int value);
int isp_set_brightness(int value);
int isp_set_sharpness(int value);
/* Noise reduction strength -50..50 (0 = calibrated): scales the sigma of the
 * luma (NR2) and chroma (UVNR) filters by 2^(3*value/50) (1/8x..8x). */
int isp_set_nr(int value);
int isp_get_nr(void);

/* Sensor readout orientation: flip = vertical, mirror = horizontal;
 * both = 180 degree rotation (camera mounted upside down). */
int isp_set_flip_mirror(int flip, int mirror);

/* Debug: AE run info + exposure struct words as JSON (GET /api/isp/ae). */
int isp_ae_debug_json(char *out, size_t cap);
/* Debug: hex of the first len bytes of a read-only attribute (nr1, nr2, uvnr,
 * 3dnr, expo). Returns the hex length or -1. */
int isp_debug_get_hex(const char *m, size_t len, char *out, size_t cap);

/* Forget cached picture-control baselines — call after any full pipeline
 * reload (isp_pipeline_init()/isp_pipeline_init_night()) so a subsequent
 * slider move computes its delta against the freshly-loaded calibration,
 * not a stale one captured before the reload. See isp.c for detail. */
void isp_reset_picture_baselines(void);
