#pragma once
/* Monotonic seconds/milliseconds that actually work on this camera.
 *
 * On the AK3918 (kernel 4.4) with the zig-built musl, clock_gettime(
 * CLOCK_MONOTONIC) returns 0 but leaves the timespec untouched (time64 /
 * vDSO mismatch), so callers read garbage. The raw 32-bit clock_gettime
 * syscall works; sysinfo() uptime is the last-resort fallback. */
#include <stdint.h>
uint32_t mono_s(void);
int64_t  mono_ms(void);
int64_t  mono_us(void);   /* microseconds, same clock */
