#pragma once
/* SD card recording (fragmented MP4 under REC_DIR/<YYYY-MM-DD>/):
 *   record.mode       off | motion | sound | events (motion or sound) | continuous
 *   record.pre        seconds kept before a motion event (0-4)
 *   record.post       seconds recorded after motion ends (2-120)
 *   record.segment    minutes per file in continuous mode (1-60)
 *   record.audio      on | off
 *   record.snapshots  on | off  (JPEG on each motion/sound event)
 *   record.keep_free  MB to keep free; the oldest recordings are deleted
 *   timelapse.interval  0 = off, else one keyframe every 5..3600 s into
 *                       HH-MM-SS_timelapse.mp4 (one file per day)
 *   timelapse.fps       playback speed of the timelapse files (1..30)
 * Recording and timelapse follow sched.record and stop in privacy mode.
 * Files: HH-MM-SS_<motion|sound|event|cont|timelapse>.mp4, HH-MM-SS_<kind>.jpg */
#include <stddef.h>

#ifndef REC_ROOT
#define REC_ROOT "/tmp/sd"
#endif
#define REC_DIR REC_ROOT "/rec"

void record_init(void);                     /* register settings */
void record_start(void);                    /* recorder thread + event hooks */
void record_event_snapshot(const char *kind);   /* from the events worker */
int  record_status_json(char *out, size_t cap);
/* {"days":[{"day":..,"files":n,"bytes":n}]} or, with day, {"day":..,"files":[{"name":..,"size":..}]} */
int  record_list_json(const char *day, char *out, size_t cap);
/* Validated path of a recording (day + name), or -1. */
int  record_path(const char *day, const char *name, char *out, size_t cap);
int  record_delete(const char *day, const char *name);   /* name NULL/"" = whole day */
/* Every recording, newest first, as JSON (caller frees). */
char *record_list_all_json(size_t *len);
int  record_is_live(const char *day, const char *name);
int  record_active(void);                /* a recording file is being written */
