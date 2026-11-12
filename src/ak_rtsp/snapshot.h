#pragma once
/* JPEG of the current 640x360 sub-stream frame (thread-safe, own buffers).
 * Returns the JPEG length or -1. */
#include <stddef.h>
#include <stdint.h>
int snapshot_jpeg(uint8_t *out, size_t cap, int quality);   /* quality 0 = setting */
int  snapshot_quality(void);
void snapshot_init(void);           /* registers image.jpeg_quality */
