#include "monotime.h"
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>

#if defined(__arm__)
#define NR_CLOCK_GETTIME32 263   /* ARM EABI __NR_clock_gettime (32-bit timespec) */
#endif

int64_t mono_ms(void)
{
#ifdef NR_CLOCK_GETTIME32
    struct { int32_t sec, nsec; } t;
    if (syscall(NR_CLOCK_GETTIME32, CLOCK_MONOTONIC, &t) == 0)
        return (int64_t)t.sec * 1000 + t.nsec / 1000000;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    struct sysinfo si;
    if (sysinfo(&si) == 0) return (int64_t)si.uptime * 1000;
    return 0;
}

uint32_t mono_s(void)
{
    return (uint32_t)(mono_ms() / 1000);
}

int64_t mono_us(void)
{
#ifdef NR_CLOCK_GETTIME32
    struct { int32_t sec, nsec; } t;
    if (syscall(NR_CLOCK_GETTIME32, CLOCK_MONOTONIC, &t) == 0)
        return (int64_t)t.sec * 1000000 + t.nsec / 1000;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
    return mono_ms() * 1000;
}
