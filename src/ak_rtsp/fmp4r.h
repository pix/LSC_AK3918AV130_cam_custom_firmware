#pragma once
/* Reader for fragmented MP4 (the SD recordings fmp4.c writes), for ONVIF
 * replay: H.264 video ('avc1'/'avcC') and 16-bit PCM audio ('ipcm' + 'pcmC',
 * also 'sowt'/'twos'). Reads the header (moov, at most FMP4R_MAX_MOOV bytes)
 * and then one moof at a time with pread(): never the whole file, and the
 * sample data only when asked (fmp4r_read). A user can put any file on the
 * SD card: every box size, offset, count and time is checked against the
 * file and the limits below; a broken or truncated file simply ends early.
 * Not thread-safe per reader (one reader per replay session). */
#include <stdint.h>
#include <stddef.h>

#define FMP4R_MAX_MOOV    (256 * 1024)
#define FMP4R_MAX_MOOF    (256 * 1024)
#define FMP4R_MAX_SAMPLES 4096          /* per fragment, both tracks */

typedef struct {
    uint64_t off;        /* file offset of the sample data */
    uint32_t size;
    uint32_t dur_us;
    int64_t  t_us;       /* decode time since the start of the file's timeline */
    uint8_t  audio;      /* 0 video, 1 audio */
    uint8_t  key;        /* video sync sample */
} fmp4r_sample_t;

typedef struct {
    int fd;
    uint64_t fsize;
    /* from the moov */
    uint32_t vid, aid;                /* track IDs, 0 = none */
    uint32_t vscale, ascale;          /* media timescales */
    uint8_t  sps[256], pps[256];
    uint16_t sps_len, pps_len;
    uint8_t  nal_len;                 /* NAL length field size: 1, 2 or 4 */
    uint16_t width, height;           /* from the sample entry */
    uint8_t  audio_be;                /* PCM byte order */
    uint32_t arate;                   /* audio sample rate */
    int64_t  duration_us;             /* mehd / mvhd, 0 = unknown (file being written or cut short) */
    uint32_t trex_dur[2], trex_size[2], trex_flags[2];
    uint64_t first_frag, next_frag;   /* file offsets of the first / next fragment */
    uint64_t next_dts[2];             /* continues a track without tfdt */
    /* the current fragment: samples in time order */
    fmp4r_sample_t *s;
    size_t ns;
    int last_idx[2];                  /* index of each track's last sample in s, -1 none */
    uint8_t *buf;                     /* moof read buffer */
} fmp4r_t;

/* 0 ok (H.264 video track found), -1 not usable. On -1 nothing stays open. */
int  fmp4r_open(fmp4r_t *r, const char *path);
/* Same, on an open descriptor (taken over: closed by fmp4r_close or on failure). */
int  fmp4r_open_fd(fmp4r_t *r, int fd);
void fmp4r_close(fmp4r_t *r);
/* Load the next fragment's samples (r->s, r->ns). 1 loaded, 0 end (or the rest is unreadable / still being written). */
int  fmp4r_next(fmp4r_t *r);
/* Make fmp4r_next() return the fragment holding t_us (the last one starting at or before it). */
void fmp4r_seek(fmp4r_t *r, int64_t t_us);
/* 1 if a complete fragment follows the current one */
int  fmp4r_more(fmp4r_t *r);
/* End time of the last complete sample (us), scanning the fragment headers; keeps the position. */
int64_t fmp4r_scan_end(fmp4r_t *r);
/* Sample data into buf: its size, or -1 (too big for cap, or unreadable). */
int  fmp4r_read(const fmp4r_t *r, const fmp4r_sample_t *s, uint8_t *buf, size_t cap);
