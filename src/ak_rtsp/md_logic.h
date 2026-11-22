#pragma once
/* Motion decision logic (pure: no hardware, no globals, fixed memory).
 *
 * md.c feeds one analysis frame at a time and acts on the result:
 *  - software source: 32x18 block luma means of the sub stream (~4/s); the
 *    background model lives here, change = |mean - background|;
 *  - hardware source: the ISP's 3D-NR per-block motion statistic u16[24][32]
 *    (~10/s), already a change value per cell.
 * With every option below at 0 / empty the decisions are bit-for-bit those
 * of the original md.c (tests/motion checks this against a copy of it).
 *
 * Options (all off by default):
 *  zones      up to 8 rectangles on the 32x18 grid with their own sensitivity
 *  min_size   a change counts only in a connected blob of >= N cells
 *             (software: 4-neighbour; hardware: the region-growing regions)
 *  persist_ms motion must be seen continuously this long before it counts
 *  light_pct  a light change (>= pct % of the picture at once, or a uniform
 *             brightness step) suppresses detection for 3 s
 *  line       a line (0..1000 picture coordinates); tracked blobs whose
 *             centroid crosses it give a crossing event with its direction
 *  loiter_s   a tracked blob staying in a zone (or the watched area) this
 *             long gives a loitering event
 * Tracking is nearest-centroid matching between frames (docs/motion.md). */
#include <stdint.h>

#define MDL_COLS        32
#define MDL_ROWS_SW     18
#define MDL_ROWS_HW     24
#define MDL_MAX_CELLS   (MDL_COLS * MDL_ROWS_HW)
#define MDL_MAX_ZONES   8
#define MDL_MAX_TRACKS  8
#define MDL_MAX_BLOBS   16
#define MDL_LIGHT_QUIET_MS 3000
#define MDL_TRACK_TTL_MS   1500        /* a track survives this long without a match */
#define MDL_GATE_CELLS     8           /* max centroid jump between two frames */
#define MDL_TRACK_MIN_CELLS 2          /* blobs smaller than this (or md.min_size) are not tracked */
#define MDL_TRACK_MIN_SEEN  3          /* a track counts for rules after this many matches */

/* rectangle on the 32x18 settings grid, inclusive; sens 1..100 */
typedef struct { int c0, r0, c1, r1, sens; } mdl_zone_t;

/* all ints (no padding: md_logic compares it with memcmp to notice changes) */
typedef struct {
    int sens;                    /* md.sensitivity 1..100 */
    int hold_s;                  /* md.hold 2..300 */
    int nzones;                  /* 0 = off */
    mdl_zone_t zone[MDL_MAX_ZONES];
    int min_size;                /* 0 = off, cells */
    int persist_ms;              /* 0 = off */
    int light_pct;               /* 0 = off (original rule only), else 10..90 */
    int line_on;                 /* 0 = off */
    int line[4];                 /* x0,y0,x1,y1 in 0..1000 */
    int line_dir;                /* 0 both, 1 only toward the left side, 2 only toward the right */
    int loiter_s;                /* 0 = off */
    int loiter_zone;             /* 0 = watched area, 1..8 = that zone */
} mdl_cfg_t;

typedef struct {
    int64_t now_ms;
    int night;                   /* hardware: 2 of the last 3 polls must hit */
    const uint8_t *watch;        /* per source cell: in the zones and not the overlay */
    const uint8_t *watch_all;    /* per source cell: not the overlay */
} mdl_in_t;

typedef struct { int id, x, y; } mdl_track_out_t;
typedef struct { int id, dir; } mdl_cross_t;        /* dir 1 = toward the left side, 2 = right */

typedef struct {
    int motion, motion_any;      /* states after this frame */
    int score;                   /* changed watched cells */
    uint8_t active[MDL_MAX_CELLS];   /* source grid: cells counted as motion in the zones */
    int light;                   /* this frame was a light change (suppressed) */
    int hwmax, hwp90;            /* hardware diagnostics */
    int ncross;                  /* line crossings in this frame */
    mdl_cross_t cross[MDL_MAX_TRACKS];
    int line;                    /* a crossing within the last hold seconds */
    int loiter, loiter_id;       /* someone loitering (state), and the track */
    int ntracks;
    mdl_track_out_t track[MDL_MAX_TRACKS];
} mdl_out_t;

typedef struct {
    int id, x, y, rx, ry, side;  /* rx,ry: position where 'side' was last confirmed */
    int nseen, in_zone;
    int64_t first_ms, last_ms, in_since;
} mdl_track_t;

typedef struct {
    mdl_cfg_t raw, cfg;          /* as given / sanitised */
    int grid_rows;               /* rows the zone map was built for */
    uint8_t zmap[MDL_MAX_CELLS]; /* 0 = no zone, k = zone k (later rectangles win) */
    /* shared decision state (the original md_thread locals) */
    int motion, motion_any;
    int64_t last_hit, last_any, quiet_until;
    /* software */
    int have_bg, run, run_any;
    int64_t run_t0, run_any_t0;
    float bg[MDL_COLS * MDL_ROWS_SW];
    /* hardware */
    int hist, hist_all, base50, glob_n, base90;
    int ok_prev, ok_any_prev;
    int64_t ok_t0, ok_any_t0;
    /* tracking / rules */
    mdl_track_t trk[MDL_MAX_TRACKS];
    int next_id, line_on, loiter_on, loiter_id;
    int64_t last_cross, last_loiter;
    /* scratch (kept here: fixed memory, no big stack frames) */
    uint8_t hit[MDL_MAX_CELLS], hitz[MDL_MAX_CELLS], act_all[MDL_MAX_CELLS];
    uint8_t seen[MDL_MAX_CELLS];
    uint16_t stack[MDL_MAX_CELLS], cells[MDL_MAX_CELLS];
} mdl_t;

void mdl_init(mdl_t *st);
/* motion detection switched off (the original thread's disabled branch) */
void mdl_disabled(mdl_t *st);
/* day/night switch: quiet for 10 s (hardware) / 5 s, background re-seeded */
void mdl_night_switch(mdl_t *st, int64_t now_ms, int hw);
/* cur: 32x18 block means. Returns 0 with *out filled, 1 when the frame only
 * seeded the background (nothing to act on). */
int  mdl_sw_frame(mdl_t *st, const mdl_cfg_t *cfg, const float *cur, const mdl_in_t *in, mdl_out_t *out);
/* stat: u16[24][32] ISP statistic. *out always filled. */
void mdl_hw_frame(mdl_t *st, const mdl_cfg_t *cfg, const uint16_t *stat, const mdl_in_t *in, mdl_out_t *out);

/* settings text <-> values (md.zone_sens, md.line); return -1 if invalid.
 * zones: "" or "c0,r0,c1,r1,sens;..." (0<=c<32, 0<=r<18, c0<=c1, r0<=r1, sens 1..100, <= 8)
 * line:  "" or "x0,y0,x1,y1" (0..1000, end points at least 20 apart) */
int  mdl_parse_zones(const char *v, mdl_zone_t *z, int max);
int  mdl_parse_line(const char *v, int line[4]);
