#pragma once
/* Software motion detection on the 640x360 sub stream: a 32x18 grid of
 * luma blocks compared with a slowly adapting background, ~4 checks/s.
 * Settings: md.enabled, md.sensitivity (1-100), md.hold (s), md.zones (144
 * hex digits = 576-bit grid mask, row-major, 1 = watched; empty = all).
 * Big global changes (IR switching, lights on/off) re-seed the background
 * instead of triggering. The decision itself is in md_logic.c, with the
 * options md.zone_sens, md.min_size, md.persist, md.light, md.line,
 * md.line_dir, md.loiter, md.loiter_zone (all off by default; docs/motion.md). */
#include <stddef.h>
#define MD_COLS 32
#define MD_ROWS 18
void md_init(void);       /* register settings */
void md_start(void);      /* detection thread */
/* {"enabled":..,"motion":..,"score":n,"active":"hex","zones":"hex",...,
 *  "line":..,"loiter":..,"light":..,"tracks":[[id,x,y],..]} (x, y 0..1000) */
int  md_status_json(char *out, size_t cap);
int  md_enabled(void);     /* motion detection switched on */
int  md_motion_any(void);  /* motion anywhere in the picture (zones ignored; no events) */
int  md_hw_raw_json(char *out, size_t cap);
