#pragma once
/* HLS output of the main stream (video only, fragmented MP4 segments) for
 * Safari, hls.js, Chromecast and media players. Off unless hls.enabled.
 *   GET /api/hls/token (Digest)  -> {"token":..., "url":"/hls/<token>/index.m3u8"}
 *   GET /hls/<token>/index.m3u8 | init.mp4 | seg<N>.m4s   (no Digest: the token)
 * One producer (live hub subscriber) runs while requests keep coming and
 * stops HLS_IDLE_S after the last one; segments live in a small RAM ring.
 * See docs/hls.md. */
#include <stddef.h>
#include <stdint.h>
#include "conn.h"

void hls_register_params(void);           /* hls.enabled (off by default) */
int  hls_enabled(void);

/* New stream token (32 hex chars). 0 ok, -1 (HLS off, no randomness). */
int  hls_token_new(char out[33]);
/* Same, usable only from the IPv4 address ip (network order; 0: any client).
 * cast.c binds the token it hands to a Cast device to that device's address,
 * so a URL copied off the LAN is useless elsewhere. */
int  hls_token_new_for(char out[33], uint32_t ip);
/* Invalidate a token now (cast stopped / replaced). */
void hls_token_revoke(const char *tok);
/* Media time (s) already sent by the newest live.mp4 stream using tok; compared
 * with the player's position it gives how far behind live the player is.
 * 0 ok, -1 no such stream. */
int  hls_mp4_sent(const char *tok, double *secs);

/* Start the producer now (cast.c: segments are ready by the time the receiver
 * asks; it stops HLS_IDLE_S later if nobody does). 0 ok, -1 (HLS off). */
int  hls_warm(void);

/* hls.cast_format: 1 = cast the low-latency live.mp4 (default), 0 = HLS. */
int  hls_cast_mp4(void);

/* "/hls/<32 hex>/<file>[?query]" -> HLS_PLAYLIST / HLS_INIT / HLS_SEGMENT /
 * HLS_LIVEMP4 (token copied to tok, segment number to *seq), HLS_BAD otherwise.
 * live.mp4: one endless fragmented MP4 (a fragment per frame, starting at the
 * next keyframe) for players that take a plain URL: ~1 s behind live instead of
 * the ~3 segments an HLS player keeps. At most HLS_MP4_MAX at once. */
enum { HLS_BAD = 0, HLS_PLAYLIST, HLS_INIT, HLS_SEGMENT, HLS_LIVEMP4 };
#define HLS_MP4_MAX 2
int  hls_parse_path(const char *path, char tok[33], uint32_t *seq);

/* Answer one GET /hls/... request. Runs in the connection's thread WITHOUT
 * the web server's lock (it may wait for the first segments and sends
 * segments in bounded pieces). Does not close cn. */
void hls_serve(conn_t *cn, const char *path);

/* {"enabled":..,"running":..,"segments":..,"bytes":..,...} for /api/hls. */
int  hls_status_json(char *out, size_t cap);

/* Stop the producer, wait for it, free everything (process exit, tests). */
void hls_stop(void);
