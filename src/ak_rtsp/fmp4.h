#pragma once
/* Fragmented MP4 writer: H.264 video (track 1, 90 kHz) + optional 16-bit
 * 8 kHz mono PCM audio ('ipcm', track 2). Each fragment (moof+mdat) is
 * complete on its own, so a file cut short by power loss still plays up to
 * its last fragment. Browsers play the video (Chrome also the audio). */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

typedef struct fmp4 fmp4_t;

/* Annex-B keyframe gives SPS/PPS; returns NULL if it has none. */
fmp4_t *fmp4_open(FILE *f, const uint8_t *keyframe, size_t len, int with_audio);
/* Add one video frame (Annex-B, ts in ms, monotonic). Frames are buffered
 * until fmp4_flush (call it at each keyframe before adding that frame). */
int  fmp4_video(fmp4_t *m, const uint8_t *annexb, size_t len, int key, uint64_t ts_ms);
/* Add audio: n 16-bit samples whose first sample is at audio time t8 (8 kHz units since the file start). */
int  fmp4_audio(fmp4_t *m, const int16_t *pcm, size_t n, uint64_t t8);
/* Write the buffered fragment. */
int  fmp4_flush(fmp4_t *m);
/* Flush and free (does not close the FILE). */
void fmp4_close(fmp4_t *m);
uint64_t fmp4_video_start_ms(const fmp4_t *m);   /* ts of the first video frame */
