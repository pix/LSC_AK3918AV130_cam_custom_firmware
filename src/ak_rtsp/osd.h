#pragma once
/* On-screen overlay settings (burned into the video by osd_hw.c; the web
 * live view draws them itself when the video overlay is unavailable).
 * Stored in OSD_FILE as key=value lines. Text is printable ASCII only
 * (what the camera's bitmap font can draw), max OSD_TEXT_MAX chars. */
#include <stddef.h>

#ifndef OSD_FILE
#define OSD_FILE "/etc/config/ak_rtsp_osd"
#endif
#define OSD_TEXT_MAX 32

typedef struct {
    int  enabled;
    int  show_time;
    int  show_mode;
    char prefix[OSD_TEXT_MAX + 1];
    char suffix[OSD_TEXT_MAX + 1];
    char pos[4];                 /* "tl", "tr", "bl", "br" */
    int  size;                   /* video overlay font: 1 = normal, 2 = large */
    int  show_fps;               /* current frame rate, e.g. "5 FPS" (adaptive frame rate) */
} osd_cfg_t;

void osd_init(void);                        /* load (defaults if missing) */
void osd_get(osd_cfg_t *out);
/* Validate + persist. Returns 0, or -1 with *err set. */
int  osd_set(const osd_cfg_t *in, const char **err);
int  osd_json(char *out, size_t cap);
/* 1 if the text is acceptable for the overlay. */
int  osd_text_ok(const char *s);
