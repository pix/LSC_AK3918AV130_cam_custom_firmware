#pragma once
/* The SD recordings as ONVIF Profile G sees them (onvif_g.c, replay.c).
 *
 * A recording token is either a day folder ("2026-11-22": every event and
 * continuous file of that day, in time order) or one file
 * ("2026-11-22_10-31-05_motion": that file only). Timelapse files are left
 * out (their media time is not wall-clock time) and so are snapshots.
 * Names and days are validated with record.c's checks (record_path,
 * record_day_ok): nothing outside REC_DIR can be named.
 *
 * Times: a file starts at the local time in its name (the recorder names it
 * when the first keyframe is written; event clips include up to record.pre
 * seconds before that) and ends at its last modification (fsync at every
 * keyframe), or now while it is being written. */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define RECIDX_MAX_DAYS  400      /* as record.c's listings */
#define RECIDX_MAX_FILES 2000     /* per day */
#define RECIDX_TOKEN_MAX 64

enum { RK_CONT, RK_MOTION, RK_SOUND, RK_EVENT };

typedef struct {
    char     name[44];            /* HH-MM-SS_<kind>.mp4 */
    time_t   start, end;          /* UTC */
    int64_t  size;
    uint8_t  kind, live;
} recfile_t;

/* Day folders with at least one candidate entry name, oldest first; returns the count. */
int recidx_days(char (*days)[11], int max);
/* Files of a day (validated, .mp4, no timelapse), oldest first; returns the count (0 if none / bad day). */
int recidx_files(const char *day, recfile_t *out, int max);
/* Token -> day (+ file name, "" for a day token). 0 ok, -1 not a valid token. */
int recidx_parse_token(const char *token, char day[11], char name[44]);
/* Day + file -> one recfile_t (stat'ed). 0 ok, -1 missing/invalid. */
int recidx_file(const char *day, const char *name, recfile_t *out);
/* "HH-MM-SS" of a name in local time on that day -> UTC (-1 if unreadable). */
time_t recidx_name_time(const char *day, const char *name);
/* File token of a day + name ("2026-11-22_10-31-05_motion"). */
void recidx_file_token(const char *day, const char *name, char *out, size_t cap);
/* xs:dateTime / RTSP clock (ISO 8601 basic or extended, optional fraction and offset) -> ms since 1970; -1 bad. */
int64_t recidx_parse_time_ms(const char *s);
/* UTC seconds -> "YYYY-MM-DDThh:mm:ssZ" */
void recidx_iso(time_t t, char out[24]);
