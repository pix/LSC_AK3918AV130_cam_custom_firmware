#pragma once
/* Second H.264 stream 640x360 (setting video.sub, crash-guarded). */
#include <stddef.h>
#include <stdint.h>
void substream_init(void);      /* register settings */
void substream_start(void);     /* after the main encoder runs */
int  substream_running(void);
/* Next frame for a consumer: cursor 0 = start at the newest keyframe.
 * Returns the frame length, 0 on timeout. */
int  substream_next(uint32_t *cursor, uint8_t *buf, size_t cap, int *key, uint64_t *ts, int timeout_ms);
int  substream_status_json(char *out, size_t cap);
