#pragma once
/* ONVIF replay (Profile G) over the RTSP server: rtsp://camera/replay/<token>
 * plays the SD recordings of a recording token (recidx.h: a day or one file).
 * rtsp.c keeps the RTSP state machine (login, SETUP transports); this module
 * demuxes the fMP4 files (fmp4r.c) and sends H.264 + G.711 u-law RTP itself,
 * with the ONVIF replay header extension (0xABAC: NTP time of the frame,
 * C/E/D/T flags, PLAY CSeq) when the client sent "Require: onvif-replay".
 *   PLAY Range: npt=<s>-[<e>] (seconds from the start of the recording) or
 *               clock=<UTC>-[<UTC>]; no Range = from the start / resume.
 *   Scale: 1 (default); > 1 up to 16 sends I-frames only, faster. Reverse: 456.
 *   Frames: intra -> I-frames only. Rate-Control: no -> as fast as the link allows.
 *   PAUSE / PLAY resumes; end of recording: RTCP BYE (players stop cleanly).
 * At most REPLAY_MAX_SESSIONS at once (each also takes one of the RTSP
 * server's RTSP_MAX_CLIENTS slots); per session: the current fragment's sample
 * table, one frame buffer (largest frame so far, <= 512 KB) and the playlist. */
#include <stddef.h>
#include <stdint.h>
#include "conn.h"

#define REPLAY_MAX_SESSIONS 2

typedef struct replay replay_t;

/* 1 if the request URI's path is /replay/... */
int replay_is_uri(const char *uri);
/* DESCRIBE: open a session; sdp gets the description. NULL on failure with
 * *status (404 unknown/off, 503 too many sessions, 415 unplayable) and *reason. */
replay_t *replay_open(const char *uri, char *sdp, size_t cap, int *status, const char **reason);
/* After a successful SETUP of track 0 (video) or 1 (audio): interleaved
 * channels (tcp) or the connected UDP sockets (owned by the caller). */
void replay_set_track(replay_t *r, int track, int tcp, int rtp_ch, int rtcp_ch, int rtp_fd, int rtcp_fd);
/* PLAY. Header values may be NULL. Fills the response's Range, RTP-Info and
 * Scale ("" = leave out). Returns 200 or an RTSP status with *reason. */
int replay_play(replay_t *r, const char *range, const char *scale, const char *frames, const char *rate_control,
                const char *require, unsigned cseq, const char *url,
                char *range_out, size_t rcap, char *rtpinfo, size_t icap, char *scale_out, size_t scap,
                const char **reason);
void replay_pause(replay_t *r);
/* While playing: sends what is due (waits up to ~20 ms otherwise).
 * 0 keep going, 1 end of the recording (BYE sent), -1 sending failed. */
int replay_step(replay_t *r, conn_t *cn);
void replay_close(replay_t *r);          /* NULL is fine */
int replay_sessions(void);
