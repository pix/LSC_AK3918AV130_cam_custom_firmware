#pragma once
/* Live H.264 for the web page: fan-out of encoder frames to up to
 * LIVE_MAX_VIEWERS WebSocket viewers (the browser muxes to fMP4 for MSE).
 * WebSocket access needs a one-time token from an authenticated request. */
#include <stddef.h>
#include <stdint.h>
#include "conn.h"

#define LIVE_MAX_VIEWERS 2

/* Encoder side: called for every encoded frame (Annex-B). Cheap no-op when
 * nobody is watching. */
void live_push_frame(const uint8_t *data, size_t len, int keyframe, uint64_t ts_ms);
int  live_viewers(void);
/* Microphone side: one capture period of 16-bit mono PCM at 8 kHz. */
void live_push_audio(const int16_t *pcm, size_t n_samples);
void live_set_audio_mute(int on);   /* privacy mode: everyone gets silence */

/* ---- consumers (web viewers and RTSP clients) ----
 * Frames and audio are only kept while at least one subscriber exists. Each
 * consumer has its own cursor; one that falls behind skips ahead to the
 * newest keyframe instead of slowing the others down. */
#define LIVE_MAX_FRAME (512 * 1024)
#define LIVE_APER      512
#define LIVE_VIDEO 1
#define LIVE_AUDIO 2
typedef struct { uint32_t want, awant; } live_cursor_t;     /* zero-init */
typedef struct {
    size_t   len;               /* video: bytes copied to buf (Annex-B) */
    int      key;               /* video: keyframe */
    uint64_t ts;                /* video: encoder ms; audio: capture ms */
    int16_t  pcm[LIVE_APER];    /* audio: 16-bit mono 8 kHz */
    size_t   n;                 /* audio: samples */
} live_item_t;
int  live_subscribe(void);
void live_unsubscribe(void);
/* Next video frame (into buf, cap >= LIVE_MAX_FRAME) or audio period, waiting
 * up to timeout_ms. Returns LIVE_VIDEO, LIVE_AUDIO or 0 (nothing yet). */
int  live_next(live_cursor_t *c, uint8_t *buf, size_t cap, live_item_t *it, int timeout_ms);
/* Pre-roll: rewind to the earliest keyframe within back_ms of the newest frame. */
int  live_rewind(live_cursor_t *c, uint32_t back_ms);

/* One-time token (32 hex chars, valid 60 s). 0 ok, -1 failure. */
int  live_token_new(char out[33]);
/* 1 if valid (and consumes it), 0 otherwise. */
int  live_token_take(const char *tok);

/* Takes ownership of the connection after the 101 handshake was sent (plain
 * or TLS). Starts a viewer thread. Returns 0, or -1 if too many viewers
 * (caller still owns and closes it). */
int  live_ws_start(conn_t *cn);
