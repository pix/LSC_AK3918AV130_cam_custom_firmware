/* Link-time wraps for the host build (-Wl,--wrap=...): the firmware starts
 * daemons (ntpd, wpa_supplicant, udhcpc, killall), reboots, mounts the SD
 * card and spawns a shell for the web terminal. On the PC none of that may
 * happen: each call is logged and reported as done (exec: the child exits). */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>

static void note(const char *what, const char *arg)
{
    fprintf(stderr, "[host-wrap] %s(%s) not executed\n", what, arg ? arg : "");
}
int __wrap_system(const char *cmd) { note("system", cmd); return 0; }
FILE *__wrap_popen(const char *cmd, const char *mode) { (void)mode; note("popen", cmd); return fopen("/dev/null", "r"); }
int __wrap_reboot(int how) { (void)how; note("reboot", ""); return 0; }
int __wrap_mount(const char *src, const char *dst, const char *t, unsigned long f, const void *d)
{ (void)dst; (void)t; (void)f; (void)d; note("mount", src); return -1; }
int __wrap_umount(const char *dst) { note("umount", dst); return -1; }
int __wrap_execle(const char *p, const char *a, ...) { (void)a; note("execle", p); _exit(127); }
int __wrap_execl(const char *p, const char *a, ...) { (void)a; note("execl", p); _exit(127); }
int __wrap_execve(const char *p, char *const a[], char *const e[]) { (void)a; (void)e; note("execve", p); _exit(127); }
int __wrap_execvp(const char *p, char *const a[]) { (void)a; note("execvp", p); _exit(127); }
int __wrap_settimeofday(const struct timeval *tv, const void *tz) { (void)tv; (void)tz; note("settimeofday", ""); return 0; }
int __wrap_sethostname(const char *n, size_t l) { (void)l; note("sethostname", n); return 0; }

/* Thread stacks: sized for 32-bit ARM frames on the camera. x86-64 frames are
 * larger and ASan adds redzones around every local buffer, so the host build
 * scales the requested size (ASan x8, plain x2). The camera's real depth is
 * checked from the ARM disassembly (TESTING.md, crash audit). */
#include <pthread.h>
#ifndef AKR_STACK_SCALE
#define AKR_STACK_SCALE 2
#endif
int __real_pthread_attr_setstacksize(pthread_attr_t *a, size_t s);
int __wrap_pthread_attr_setstacksize(pthread_attr_t *a, size_t s) { return __real_pthread_attr_setstacksize(a, s * AKR_STACK_SCALE); }
