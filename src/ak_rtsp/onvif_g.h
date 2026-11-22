#pragma once
/* ONVIF Profile G (recording search and replay) for the SD recordings:
 *  - Search service    /onvif/search_service     (summary, recording info,
 *    media attributes, FindRecordings / FindEvents with result paging)
 *  - Recording service /onvif/recording_service  (read-only: GetRecordings,
 *    GetRecordingConfiguration, GetTrackConfiguration, no jobs)
 *  - Replay service    /onvif/replay_service     (GetReplayUri -> rtsp://camera/replay/<token>, replay.c)
 * Recordings: one per day folder (token "YYYY-MM-DD"); a single file is
 * also accepted as a token ("YYYY-MM-DD_HH-MM-SS_kind"), see recidx.h.
 * Login as for the other ONVIF services (onvif.c checks it before calling in;
 * only GetServiceCapabilities is answered without).
 * Setting: onvif.replay (default off): services advertised and answered,
 * RTSP /replay/ served. */
#include <stddef.h>

void onvif_g_init(void);        /* register onvif.replay (called by onvif_init) */
int  onvif_g_enabled(void);
/* A request for one of these services (by path, or by an action only they
 * have). 0: not ours. 1: *out = SOAP Body content (malloc'd, caller frees;
 * NULL if out of memory), *code = HTTP status (200, or 400 with a fault).
 * base = "http://<host>", host = Host header's host part. */
int  onvif_g_handle(const char *path, const char *action, const char *body, const char *base, const char *host,
                    char **out, int *code);
/* What the device service adds while enabled ("" when off):
 * GetCapabilities' <tt:Extension>, GetServices' entries, a scope item. */
void onvif_g_caps(const char *base, char *out, size_t cap);
void onvif_g_services(const char *base, char *out, size_t cap);
const char *onvif_g_scope(void);
