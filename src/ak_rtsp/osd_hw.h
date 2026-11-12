#pragma once
/* Burned-in video overlay: draws the osd.c text (prefix, date/time, day/night
 * mode, suffix) into the ISP's hardware OSD regions, so it is part of the
 * encoded H.264 (RTSP, live view) and the sub-stream snapshots.
 *
 * Reverse-engineered from stock anyka_ipc (ak_osd_* / ak_vpss_osd_set_param)
 * and ak_isp.ko: ION memory triple-buffered per region, 16-colour palette,
 * 4 bits per pixel, all through the ISP tunnel ioctl on g_isp_fd. */

/* Start the 1 Hz render thread (call after the video pipeline is running).
 * Skipped when /tmp/sd/maint/no_osd exists or the crash guard is set. */
void osd_hw_start(void);
/* Turn the regions off (process exit). */
void osd_hw_stop(void);
/* "on", "off", "starting", "guard" (skipped after an incomplete start),
 * "disabled" (SD kill switch) or "error". */
const char *osd_hw_state(void);
/* Clear the crash guard so the next boot tries again (called when the user
 * saves overlay settings). */
void osd_hw_clear_guard(void);
