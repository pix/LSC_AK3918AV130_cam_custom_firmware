#include "system.h"
#include "control.h"
#include "monotime.h"
#include "rtsp.h"
#include "speaker.h"
#include "events.h"
#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <glob.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#ifndef SYS_LED_PATH
#define SYS_LED_PATH "/sys/class/leds/state_led"
#endif

/* ---- status LED ------------------------------------------------------------ */
static int g_led_on = 1;  /* DTB default-state = "on" */

static int write_str(const char *path, const char *v)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = fputs(v, f) >= 0;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

/* led.status is the LED's resting state. led.feedback lets activity override
 * it: a fast blink while motion is active, a slow blink while a recording file
 * is being written (motion first). Blinking uses the kernel "timer" trigger,
 * so it costs nothing between changes. */
enum { LED_OFF, LED_ON, LED_FAST, LED_SLOW };
static pthread_mutex_t g_led_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_led_phys = -1;               /* state written to sysfs, -1 = unknown */
static int s_led_fb;                      /* bit 0 motion, bit 1 recording */
static const char *const k_led_fb[] = { "off", "motion", "recording", "both" };

static int led_write(int st)              /* g_led_lock held */
{
    if (st == g_led_phys) return 0;
    int rc;
    if (st == LED_FAST || st == LED_SLOW) {
        const char *d = st == LED_FAST ? "150" : "1000";
        rc = write_str(SYS_LED_PATH "/trigger", "timer");
        if (rc == 0) rc = write_str(SYS_LED_PATH "/delay_on", d);
        if (rc == 0) rc = write_str(SYS_LED_PATH "/delay_off", d);
    } else {
        write_str(SYS_LED_PATH "/trigger", "none");   /* ignore: may not exist */
        rc = write_str(SYS_LED_PATH "/brightness", st == LED_ON ? "1" : "0");
    }
    g_led_phys = rc == 0 ? st : -1;
    return rc;
}

static int led_wanted(void)               /* g_led_lock held */
{
    if ((s_led_fb & 1) && events_motion_active()) return LED_FAST;
    if ((s_led_fb & 2) && record_active()) return LED_SLOW;
    return g_led_on ? LED_ON : LED_OFF;
}

int sys_led_set(int on)
{
    pthread_mutex_lock(&g_led_lock);
    g_led_on = on ? 1 : 0;
    int want = led_wanted();
    if (want == LED_ON || want == LED_OFF) g_led_phys = -1;   /* always rewrite a plain on/off */
    int rc = led_write(want);
    pthread_mutex_unlock(&g_led_lock);
    return rc;
}

int sys_led_get(void) { return g_led_on; }

static int p_ledfb_get(char *o, size_t c) { snprintf(o, c, "%s", k_led_fb[s_led_fb & 3]); return 0; }
static int p_ledfb_set(const char *v, const char **e)
{
    for (int i = 0; i < 4; i++)
        if (!strcmp(v, k_led_fb[i])) { s_led_fb = i; return 0; }
    *e = "must be off, motion, recording or both";
    return -1;
}

static void *led_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(300000);
        pthread_mutex_lock(&g_led_lock);
        if (s_led_fb || g_led_phys == LED_FAST || g_led_phys == LED_SLOW) led_write(led_wanted());
        pthread_mutex_unlock(&g_led_lock);
    }
    return NULL;
}

void sys_led_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 32 * 1024);
    if (pthread_create(&t, &a, led_thread, NULL) != 0) printf("[led] cannot start the feedback thread\n");
    pthread_attr_destroy(&a);
}

/* ---- time zone ------------------------------------------------------------- */
/* musl has no zoneinfo on this camera: map names to POSIX TZ strings. */
static const struct { const char *name, *posix; } k_tz[] = {
    { "UTC",                 "UTC0" },
    { "Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon",       "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Europe/Paris",        "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels",     "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Amsterdam",    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",         "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Athens",       "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Helsinki",     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Moscow",       "MSK-3" },
    { "America/New_York",    "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago",     "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver",      "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "Asia/Tokyo",          "JST-9" },
    { "Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};

static pthread_mutex_t g_tlock = PTHREAD_MUTEX_INITIALIZER;
static char g_tz[32]  = TIME_DEFAULT_TZ;
static char g_ntp[64] = TIME_DEFAULT_NTP;
static int  g_ntp_dirty = 1;

int sys_time_set_tz(const char *name)
{
    for (size_t i = 0; i < sizeof(k_tz) / sizeof(k_tz[0]); i++) {
        if (!strcmp(name, k_tz[i].name)) {
            pthread_mutex_lock(&g_tlock);
            snprintf(g_tz, sizeof(g_tz), "%s", name);
            setenv("TZ", k_tz[i].posix, 1);
            tzset();
            pthread_mutex_unlock(&g_tlock);
            return 0;
        }
    }
    return -1;
}

const char *sys_time_tz(void) { return g_tz; }

int sys_tz_list_json(char *out, size_t cap)
{
    size_t n = (size_t)snprintf(out, cap, "[");
    for (size_t i = 0; i < sizeof(k_tz) / sizeof(k_tz[0]) && n < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s\"%s\"", i ? "," : "", k_tz[i].name);
    if (n < cap) n += (size_t)snprintf(out + n, cap - n, "]");
    return (int)n;
}

/* ---- NTP ------------------------------------------------------------------- */
static int valid_host(const char *h)
{
    size_t n = strlen(h);
    if (n < 1 || n > 63 || h[0] == '-' || h[0] == '.') return 0;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)h[i]) || h[i] == '.' || h[i] == '-')) return 0;
    return 1;
}

int sys_time_set_server(const char *host)
{
    if (!valid_host(host)) return -1;
    pthread_mutex_lock(&g_tlock);
    snprintf(g_ntp, sizeof(g_ntp), "%s", host);
    g_ntp_dirty = 1;
    pthread_mutex_unlock(&g_tlock);
    return 0;
}

const char *sys_time_server(void) { return g_ntp; }

int sys_time_synced(void) { return time(NULL) > 1700000000; }  /* boots at 2000-01-01 */

/* A daemon we start keeps every descriptor it inherited for its whole life.
 * ntpd did: it held /dev/video*, /dev/venc*, /dev/isp-param-0, the audio
 * devices and our listening sockets, so after ak_rtsp died every restart
 * failed with "Resource busy" / "Address in use" until a reboot. */
void sys_cloexec_all(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return;
    int self = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int fd = atoi(e->d_name);
        if (fd <= 2 || fd == self) continue;
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0 && !(fl & FD_CLOEXEC)) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
    closedir(d);
}

static int process_running(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char p[64], comm[32] = "";
        snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = 0;
            found = !strcmp(comm, name);
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static void *ntp_thread(void *arg)
{
    (void)arg;
    sleep(20);   /* let Wi-Fi + DHCP come up */
    for (;;) {
        char host[64];
        int dirty;
        pthread_mutex_lock(&g_tlock);
        snprintf(host, sizeof(host), "%s", g_ntp);
        dirty = g_ntp_dirty;
        g_ntp_dirty = 0;
        pthread_mutex_unlock(&g_tlock);

        if (dirty || !process_running("ntpd")) {
            /* host is validated ([A-Za-z0-9.-]) so it is safe in a command line */
            char cmd[160];
            sys_cloexec_all();                       /* ntpd must not keep our devices */
            if (system("killall ntpd >/dev/null 2>&1") == -1) { /* ignore */ }
            snprintf(cmd, sizeof(cmd), "ntpd -p %s >/dev/null 2>&1", host);
            int r = system(cmd);   /* busybox ntpd daemonises */
            printf("[time] ntpd (re)started with server %s (rc=%d)\n", host, r);
        }
        for (int i = 0; i < 60 && !g_ntp_dirty; i++) sleep(1);
    }
    return NULL;
}

void sys_time_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, ntp_thread, NULL) == 0) pthread_detach(t);
}

/* ---- health ---------------------------------------------------------------- */
static long read_long_file(const char *path, int *ok)
{
    FILE *f = fopen(path, "r");
    long v = 0;
    *ok = 0;
    if (f) { *ok = fscanf(f, "%ld", &v) == 1; fclose(f); }
    return v;
}

/* Temperature. On this board the chip sensor is the stock ak_tsensor driver
 * (never loaded by the stock firmware): /sys/.../chip_temperature reads
 * like "31.864 \xe2\x84\x83". Loaded once on demand (verified on hardware
 * 2026-11-11: loads cleanly, ~32 C idle). */
#ifndef TSENSOR_PATH
#define TSENSOR_PATH "/sys/devices/platform/soc/80001e0.tsensor/chip_temperature"
#endif
static int read_tsensor_c10(void)
{
    static int tried_load;
    if (access(TSENSOR_PATH, R_OK) != 0 && !tried_load) {
        tried_load = 1;
        sys_cloexec_all();
        if (access("/usr/modules/ak_tsensor.ko", R_OK) == 0 &&
            system("insmod /usr/modules/ak_tsensor.ko >/dev/null 2>&1") == -1) { /* ignore */ }
    }
    FILE *f = fopen(TSENSOR_PATH, "r");
    if (!f) return -9999;
    int whole = 0, frac = 0, neg = 0;
    char buf[32] = "";
    if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
    fclose(f);
    const char *p = buf;
    if (*p == '-') { neg = 1; p++; }
    if (!isdigit((unsigned char)*p)) return -9999;
    while (isdigit((unsigned char)*p)) whole = whole * 10 + (*p++ - '0');
    if (*p == '.' && isdigit((unsigned char)p[1])) frac = p[1] - '0';
    int v = whole * 10 + frac;
    return neg ? -v : v;
}

/* Temperature: chip sensor first, then common sysfs locations. */
static int read_temp_c10(void)
{
    int t = read_tsensor_c10();
    if (t != -9999) return t;
    static const char *pats[] = {
        "/sys/class/thermal/thermal_zone*/temp",
        "/sys/class/hwmon/hwmon*/temp*_input",
        "/sys/devices/platform/*tsensor*/temp*",
        "/sys/devices/platform/soc/*tsensor*/temp*",
        "/sys/bus/platform/drivers/*tsensor*/*/temp*",
    };
    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        glob_t g;
        if (glob(pats[i], 0, NULL, &g) != 0) continue;
        for (size_t j = 0; j < g.gl_pathc; j++) {
            int ok;
            long v = read_long_file(g.gl_pathv[j], &ok);
            if (!ok) continue;
            globfree(&g);
            if (v > 1000 || v < -1000) return (int)(v / 100);   /* millidegrees */
            return (int)(v * 10);
        }
        globfree(&g);
    }
    return -9999;
}

int sys_temp_c10(void) { return read_temp_c10(); }

int sys_health_json(char *out, size_t cap)
{
    double load = 0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%lf", &load) != 1) load = 0; fclose(f); }

    long total = 0, avail = -1, mfree = 0, buffers = 0, cached = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char k[32]; long v;
        while (fscanf(f, "%31s %ld kB", k, &v) == 2) {
            if (!strcmp(k, "MemTotal:")) total = v;
            else if (!strcmp(k, "MemAvailable:")) avail = v;
            else if (!strcmp(k, "MemFree:")) mfree = v;
            else if (!strcmp(k, "Buffers:")) buffers = v;
            else if (!strcmp(k, "Cached:")) cached = v;
        }
        fclose(f);
    }
    if (avail < 0) avail = mfree + buffers + cached;

    char tbuf[32] = "";
    time_t now = time(NULL);
    struct tm tm;
    pthread_mutex_lock(&g_tlock);
    localtime_r(&now, &tm);
    pthread_mutex_unlock(&g_tlock);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);

    int t10 = read_temp_c10();
    char temp[16];
    if (t10 == -9999) snprintf(temp, sizeof(temp), "null");
    else snprintf(temp, sizeof(temp), "%d.%d", t10 / 10, abs(t10 % 10));

    /* last fatal crash of ak_rtsp (main.c's signal handler): "time=<unix> FATAL: ..." */
    char crash[160] = "", when[32] = "";
    FILE *cf = fopen(LAST_CRASH_FILE, "r");
    if (cf) {
        char line[200] = "";
        if (fgets(line, sizeof line, cf)) {
            line[strcspn(line, "\r\n")] = 0;
            long ct = 0;
            const char *msg = line;
            if (sscanf(line, "time=%ld ", &ct) == 1 && (msg = strchr(line, ' '))) msg++;
            else msg = line;
            if (ct > 1700000000) {
                time_t tt = (time_t)ct;
                struct tm ctm;
                localtime_r(&tt, &ctm);
                strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &ctm);
            }
            size_t k = 0;                              /* JSON-safe copy */
            for (const char *p = msg; *p && k + 1 < sizeof crash; p++)
                crash[k++] = (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) ? ' ' : *p;
            crash[k] = 0;
        }
        fclose(cf);
    }

    return snprintf(out, cap,
        "{\"uptime_s\":%u,\"load\":%.2f,\"mem_total_kb\":%ld,\"mem_avail_kb\":%ld,"
        "\"temp_c\":%s,\"rtsp_client\":%d,"
        "\"time\":{\"local\":\"%s\",\"synced\":%s,\"tz\":\"%s\",\"ntp\":\"%s\",\"ntpd\":%s},"
        "\"led\":%d,\"speaker\":%s,\"last_crash\":\"%s\",\"last_crash_time\":\"%s\"}",
        mono_s(), load, total, avail, temp, rtsp_client_connected(),
        tbuf, sys_time_synced() ? "true" : "false", g_tz, g_ntp,
        process_running("ntpd") ? "true" : "false", g_led_on, speaker_ready() ? "true" : "false",
        crash, when);
}

/* ---- device name (shown in the web page, Home Assistant, webhooks, ONVIF) ---- */
#include "ctrl_params.h"
#include <ctype.h>
static char s_devname[33] = "LSC Camera";
static int devname_ok(const char *v)
{
    if (!*v || strlen(v) > 32) return 0;
    for (; *v; v++) if (!(isalnum((unsigned char)*v) || *v == ' ' || *v == '-' || *v == '_' || *v == '.')) return 0;
    return 1;
}
CTRL_STR(p_devname, s_devname, devname_ok, "1-32 letters, digits, spaces, '-', '_' or '.'")
const char *sys_device_name(void) { return s_devname; }
void sys_register_params(void)
{
    CTRL_REG("device.name", p_devname, CTRL_PERSIST);
    CTRL_REG("led.feedback", p_ledfb, CTRL_PERSIST);
}

/* device name as a certificate common name / identifier (spaces -> '-') */
const char *sys_device_name_cn(void)
{
    static char cn[33];
    size_t i = 0;
    for (const char *p = s_devname; *p && i < sizeof cn - 1; p++) cn[i++] = *p == ' ' ? '-' : *p;
    cn[i] = 0;
    return cn;
}
