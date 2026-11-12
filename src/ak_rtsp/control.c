#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "ae.h"
#include "night.h"
#include "isp.h"
#include "control.h"
#include "monotime.h"
#include "auth.h"
#include "ircut.h"
#include "system.h"
#include "netcfg.h"
#include "speaker.h"
#include "imgx.h"

#ifndef SETTINGS_FILE
#define SETTINGS_FILE "/etc/config/ak_rtsp_settings"
#endif
#ifndef EXT_SETTINGS_FILE
#define EXT_SETTINGS_FILE "/etc/config/ak_rtsp_ext"   /* registered params (0600: may hold secrets) */
#endif

/* ---- registered parameters (modules add their own settings) ---------------- */
#define MAX_REG 96
static ctrl_param_t g_reg[MAX_REG];
static int g_nreg;

int control_register(const ctrl_param_t *p)
{
    if (g_nreg >= MAX_REG || !p || !p->name || !p->get || !p->set) return -1;
    g_reg[g_nreg++] = *p;
    return 0;
}

static const ctrl_param_t *reg_find(const char *name)
{
    for (int i = 0; i < g_nreg; i++) if (!strcmp(g_reg[i].name, name)) return &g_reg[i];
    return NULL;
}
static void ext_save(void);

#define CONTROL_PORT 8091

static volatile int    g_control_running;
static pthread_t       g_control_tid;
static pthread_t       g_log_tid;
static int             g_server_fd = -1;

/* Single-client model — this is a debug/tuning tool, not a multi-user
 * service. A new connection replaces any existing one. */
static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_client_fd = -1;
static int             g_client_authed;   /* LOG lines only after AUTH */

/* Output sink: replies go to a socket (port 8091) or into a buffer
 * (control_exec(), used by the web admin page). */
typedef struct {
    int     fd;      /* >= 0: socket */
    char   *buf;     /* else: append here */
    size_t  cap, len;
} sink_t;

static pthread_mutex_t g_exec_lock = PTHREAD_MUTEX_INITIALIZER; /* serialises SET/GET across port 8091 and web */
static int g_loading_settings;
static void settings_save(void);

static int             g_orig_stdout = -1; /* real stdout fd (serial console) */
static int             g_log_pipe_r  = -1;
static int             g_sd_log      = -1;   /* test mode: copy of stdout on the SD card */
#define SD_LOG_FLAG "/tmp/sd/maint/ak_rtsp.log.enable"
#define SD_LOG_FILE "/tmp/sd/maint/ak_rtsp.log"

/* isp.saturation/contrast/brightness/sharpness — the isp_set_*() functions
 * (isp.c) are one-way (GET-modify-SET against the hardware, no getter for
 * "what did we last ask for"), so the logical -50..50 value the user set is
 * tracked here, same ownership pattern as ae_tuning_t/night_tuning_t but
 * without a matching isp_tuning_t struct. 0 = neutral/unchanged from stock
 * default (matches isp_set_* treating the value as a delta off the
 * hardware/cached baseline captured at first call). */
static int g_isp_saturation = 0;
static int g_isp_contrast   = 0;
static int g_isp_brightness = 0;
static int g_isp_sharpness  = 0;
static int g_img_flip       = 0;   /* sensor default orientation until set */
static int g_img_mirror     = 0;

/* -------------------------------------------------------------------------
 * stdout tee — captures every printf in the whole binary (not just ae.c/
 * night.c) without touching a single existing call site. control_start()
 * redirects STDOUT_FILENO into a pipe; this thread reads it, writes the
 * exact same bytes back to the real stdout (serial console unaffected),
 * and additionally forwards complete lines to the connected control client
 * prefixed "LOG ".
 * ------------------------------------------------------------------------- */
static void *log_pump(void *arg)
{
    (void)arg;
    char   raw[512];
    char   line[1024];
    size_t line_len = 0;

    for (;;) {
        ssize_t n = read(g_log_pipe_r, raw, sizeof(raw));
        if (n <= 0) break;  /* pipe write end closed = process exiting */

        write(g_orig_stdout, raw, n);  /* unchanged passthrough to serial console */
        if (g_sd_log >= 0) {           /* survives a reboot: written through to the SD card */
            static time_t last_sync;
            if (write(g_sd_log, raw, (size_t)n) < 0) { close(g_sd_log); g_sd_log = -1; }
            else if (time(NULL) != last_sync) { fdatasync(g_sd_log); last_sync = time(NULL); }
        }

        for (ssize_t i = 0; i < n; i++) {
            char c = raw[i];
            if (c == '\n') {
                line[line_len] = '\0';
                pthread_mutex_lock(&g_client_lock);
                if (g_client_fd >= 0 && g_client_authed)
                    dprintf(g_client_fd, "LOG %s\n", line);
                pthread_mutex_unlock(&g_client_lock);
                line_len = 0;
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Command protocol
 * ------------------------------------------------------------------------- */
/* Locked against g_client_lock — log_pump() also writes to this fd from a
 * different thread (async LOG lines); without a shared lock the two could
 * interleave mid-line on the socket. */
static void reply(sink_t *fd, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len < sizeof(buf) - 1) { buf[len] = '\n'; len++; }
    if (fd->fd >= 0) {
        pthread_mutex_lock(&g_client_lock);
        if (write(fd->fd, buf, len) < 0) { /* client gone; reader notices */ }
        pthread_mutex_unlock(&g_client_lock);
    } else if (fd->buf && fd->len + len < fd->cap) {
        memcpy(fd->buf + fd->len, buf, len);
        fd->len += len;
        fd->buf[fd->len] = '\0';
    }
}

static const char *ir_mode_name(int m)
{
    return m == 1 ? "on" : m == 2 ? "off" : "auto";
}

static const char *night_mode_name(night_override_t m)
{
    switch (m) {
    case NIGHT_MODE_FORCE_DAY:   return "day";
    case NIGHT_MODE_FORCE_NIGHT: return "night";
    default:                     return "auto";
    }
}

static int night_mode_parse(const char *s, night_override_t *out)
{
    if (!strcmp(s, "auto"))  { *out = NIGHT_MODE_AUTO;        return 0; }
    if (!strcmp(s, "day"))   { *out = NIGHT_MODE_FORCE_DAY;   return 0; }
    if (!strcmp(s, "night")) { *out = NIGHT_MODE_FORCE_NIGHT; return 0; }
    return -1;
}

static void do_list(sink_t *fd)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    reply(fd, "ae.stable_range=%d",       a.stable_range);
    reply(fd, "ae.hold_range=%d",         a.hold_range);
    reply(fd, "ae.speed=%d",              a.speed);
    reply(fd, "ae.exp_max=%d",            a.exp_max);
    reply(fd, "ae.enabled=%d",            a.enabled);
    reply(fd, "night.mode=%s",            night_mode_name(n.override));
    reply(fd, "night.state=%s",           night_is_night() ? "night" : "day");
    reply(fd, "night.trigger_hw_exp=%u",  n.trigger_hw_exp);
    reply(fd, "night.day_hw_exp=%u",      n.day_hw_exp);
    reply(fd, "night.confirm_samples=%d", n.confirm_samples);
    reply(fd, "night.lock_ms=%d",         n.lock_ms);
    reply(fd, "isp.saturation=%d",        g_isp_saturation);
    reply(fd, "isp.contrast=%d",          g_isp_contrast);
    reply(fd, "isp.brightness=%d",        g_isp_brightness);
    reply(fd, "isp.sharpness=%d",         g_isp_sharpness);
    reply(fd, "image.flip=%d",            g_img_flip);
    reply(fd, "image.mirror=%d",          g_img_mirror);
    reply(fd, "ir.mode=%s",               ir_mode_name(ircut_get_ir_mode()));
    reply(fd, "led.status=%s",            sys_led_get() ? "on" : "off");
    reply(fd, "time.ntp=%s",              sys_time_server());
    reply(fd, "time.tz=%s",               sys_time_tz());
    reply(fd, "net.ap_fallback=%s",       net_get_fallback() ? "on" : "off");
    reply(fd, "speaker.volume=%d",        speaker_get_volume());
    /* VENC QP is stock-matched ground truth but
     * not live-settable yet — no confirmed ioctl to update rc params on an
     * already-created encoder channel without a DEACTIVATE/CREATE_ENC
     * cycle. Reported for visibility only. */
    reply(fd, "venc.minqp=28 (read-only)");
    reply(fd, "venc.maxqp=43 (read-only)");
    for (int i = 0; i < g_nreg; i++) {
        char v[256] = "";
        g_reg[i].get(v, sizeof(v));
        if (g_reg[i].flags & CTRL_SECRET) reply(fd, "%s=%s", g_reg[i].name, v[0] ? "(set)" : "");
        else reply(fd, "%s=%s", g_reg[i].name, v);
    }
    reply(fd, ".");
}

static void do_get(sink_t *fd, const char *param)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    if      (!strcmp(param, "ae.stable_range"))       reply(fd, "ae.stable_range=%d", a.stable_range);
    else if (!strcmp(param, "ae.hold_range"))         reply(fd, "ae.hold_range=%d", a.hold_range);
    else if (!strcmp(param, "ae.speed"))              reply(fd, "ae.speed=%d", a.speed);
    else if (!strcmp(param, "ae.exp_max"))            reply(fd, "ae.exp_max=%d", a.exp_max);
    else if (!strcmp(param, "ae.enabled"))            reply(fd, "ae.enabled=%d", a.enabled);
    else if (!strcmp(param, "night.mode"))            reply(fd, "night.mode=%s", night_mode_name(n.override));
    else if (!strcmp(param, "night.state"))           reply(fd, "night.state=%s", night_is_night() ? "night" : "day");
    else if (!strcmp(param, "night.trigger_hw_exp"))  reply(fd, "night.trigger_hw_exp=%u", n.trigger_hw_exp);
    else if (!strcmp(param, "night.day_hw_exp"))      reply(fd, "night.day_hw_exp=%u", n.day_hw_exp);
    else if (!strcmp(param, "night.confirm_samples")) reply(fd, "night.confirm_samples=%d", n.confirm_samples);
    else if (!strcmp(param, "night.lock_ms"))         reply(fd, "night.lock_ms=%d", n.lock_ms);
    else if (!strcmp(param, "isp.saturation"))        reply(fd, "isp.saturation=%d", g_isp_saturation);
    else if (!strcmp(param, "isp.contrast"))          reply(fd, "isp.contrast=%d", g_isp_contrast);
    else if (!strcmp(param, "isp.brightness"))        reply(fd, "isp.brightness=%d", g_isp_brightness);
    else if (!strcmp(param, "isp.sharpness"))         reply(fd, "isp.sharpness=%d", g_isp_sharpness);
    else if (!strcmp(param, "image.flip"))            reply(fd, "image.flip=%d", g_img_flip);
    else if (!strcmp(param, "image.mirror"))          reply(fd, "image.mirror=%d", g_img_mirror);
    else if (!strcmp(param, "ir.mode"))               reply(fd, "ir.mode=%s", ir_mode_name(ircut_get_ir_mode()));
    else if (!strcmp(param, "led.status"))            reply(fd, "led.status=%s", sys_led_get() ? "on" : "off");
    else if (!strcmp(param, "time.ntp"))              reply(fd, "time.ntp=%s", sys_time_server());
    else if (!strcmp(param, "time.tz"))               reply(fd, "time.tz=%s", sys_time_tz());
    else if (!strcmp(param, "net.ap_fallback"))       reply(fd, "net.ap_fallback=%s", net_get_fallback() ? "on" : "off");
    else if (!strcmp(param, "speaker.volume"))        reply(fd, "speaker.volume=%d", speaker_get_volume());
    else if (!strcmp(param, "venc.minqp"))            reply(fd, "venc.minqp=28 (read-only)");
    else if (!strcmp(param, "venc.maxqp"))            reply(fd, "venc.maxqp=43 (read-only)");
    else if (reg_find(param)) {
        const ctrl_param_t *r = reg_find(param);
        char v[256] = "";
        r->get(v, sizeof(v));
        if (r->flags & CTRL_SECRET) reply(fd, "%s=%s", param, v[0] ? "(set)" : "");
        else reply(fd, "%s=%s", param, v);
    }
    else                                              reply(fd, "ERR unknown param %s", param);
}

static int do_set_inner(sink_t *fd, const char *param, const char *value);

static void do_set(sink_t *fd, const char *param, const char *value)
{
    char before[160] = "", after[160] = "";
    sink_t b = { -1, before, sizeof before, 0 }, a = { -1, after, sizeof after, 0 };
    if (!g_loading_settings) do_get(&b, param);
    if (do_set_inner(fd, param, value) == 0 && !g_loading_settings) {
        do_get(&a, param);
        if (strcmp(before, after) != 0) settings_save();   /* unchanged: no flash write */
    }
}

/* whole decimal number within [lo, hi] */
static int int_in(const char *v, long lo, long hi)
{
    char *end;
    long x = strtol(v, &end, 10);
    return *v && !*end && x >= lo && x <= hi;
}

static int do_set_inner(sink_t *fd, const char *param, const char *value)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    if (!strcmp(param, "ae.stable_range")) {
        if (!int_in(value, 1, 50)) { reply(fd, "ERR ae.stable_range must be 1..50"); return -1; }
        a.stable_range = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.stable_range=%d", a.stable_range);
    } else if (!strcmp(param, "ae.hold_range")) {
        if (!int_in(value, 1, 50)) { reply(fd, "ERR ae.hold_range must be 1..50"); return -1; }
        a.hold_range = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.hold_range=%d", a.hold_range);
    } else if (!strcmp(param, "ae.speed")) {
        if (!int_in(value, 1, 32)) { reply(fd, "ERR ae.speed must be 1..32"); return -1; }
        a.speed = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.speed=%d", a.speed);
    } else if (!strcmp(param, "ae.exp_max")) {
        if (!int_in(value, 1, 32767)) { reply(fd, "ERR ae.exp_max must be 1..32767"); return -1; }
        a.exp_max = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.exp_max=%d", a.exp_max);
    } else if (!strcmp(param, "ae.enabled")) {
        a.enabled = atoi(value) ? 1 : 0;
        ae_set_tuning(&a);
        reply(fd, "OK ae.enabled=%d", a.enabled);
    } else if (!strcmp(param, "night.mode")) {
        night_override_t m;
        if (night_mode_parse(value, &m) != 0) {
            reply(fd, "ERR invalid night.mode value %s (want auto|day|night)", value);
            return -1;
        }
        n.override = m;
        night_set_tuning(&n);
        reply(fd, "OK night.mode=%s", night_mode_name(m));
    } else if (!strcmp(param, "night.trigger_hw_exp")) {
        n.trigger_hw_exp = (uint32_t)strtoul(value, NULL, 10);
        night_set_tuning(&n);
        reply(fd, "OK night.trigger_hw_exp=%u", n.trigger_hw_exp);
    } else if (!strcmp(param, "night.day_hw_exp")) {
        n.day_hw_exp = (uint32_t)strtoul(value, NULL, 10);
        night_set_tuning(&n);
        reply(fd, "OK night.day_hw_exp=%u", n.day_hw_exp);
    } else if (!strcmp(param, "night.confirm_samples")) {
        n.confirm_samples = atoi(value);
        night_set_tuning(&n);
        reply(fd, "OK night.confirm_samples=%d", n.confirm_samples);
    } else if (!strcmp(param, "night.lock_ms")) {
        n.lock_ms = atoi(value);
        night_set_tuning(&n);
        reply(fd, "OK night.lock_ms=%d", n.lock_ms);
    } else if (!strcmp(param, "isp.saturation")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.saturation range [-50,50]"); return -1; }
        if (isp_set_saturation(v) != 0) { reply(fd, "ERR isp_set_saturation failed"); return -1; }
        g_isp_saturation = v;
        reply(fd, "OK isp.saturation=%d", v);
    } else if (!strcmp(param, "isp.contrast")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.contrast range [-50,50]"); return -1; }
        if (isp_set_contrast(v) != 0) { reply(fd, "ERR isp_set_contrast failed"); return -1; }
        g_isp_contrast = v;
        reply(fd, "OK isp.contrast=%d", v);
    } else if (!strcmp(param, "isp.brightness")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.brightness range [-50,50]"); return -1; }
        if (isp_set_brightness(v) != 0) { reply(fd, "ERR isp_set_brightness failed"); return -1; }
        g_isp_brightness = v;
        reply(fd, "OK isp.brightness=%d", v);
    } else if (!strcmp(param, "isp.sharpness")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.sharpness range [-50,50]"); return -1; }
        if (isp_set_sharpness(v) != 0) { reply(fd, "ERR isp_set_sharpness failed"); return -1; }
        g_isp_sharpness = v;
        reply(fd, "OK isp.sharpness=%d", v);
    } else if (!strcmp(param, "image.flip") || !strcmp(param, "image.mirror")) {
        if (strcmp(value, "0") && strcmp(value, "1")) { reply(fd, "ERR %s must be 0 or 1", param); return -1; }
        int v = value[0] == '1';
        int flip   = !strcmp(param, "image.flip")   ? v : g_img_flip;
        int mirror = !strcmp(param, "image.mirror") ? v : g_img_mirror;
        if (isp_set_flip_mirror(flip, mirror) != 0) { reply(fd, "ERR isp_set_flip_mirror failed"); return -1; }
        g_img_flip = flip; g_img_mirror = mirror;
        reply(fd, "OK %s=%d", param, v);
    } else if (!strcmp(param, "ir.mode")) {
        int m = !strcmp(value, "auto") ? 0 : !strcmp(value, "on") ? 1 : !strcmp(value, "off") ? 2 : -1;
        if (m < 0) { reply(fd, "ERR ir.mode must be auto|on|off"); return -1; }
        ircut_set_ir_mode(m);
        reply(fd, "OK ir.mode=%s", value);
    } else if (!strcmp(param, "led.status")) {
        if (strcmp(value, "on") && strcmp(value, "off")) { reply(fd, "ERR led.status must be on|off"); return -1; }
        if (sys_led_set(!strcmp(value, "on")) != 0) { reply(fd, "ERR cannot set status LED"); return -1; }
        reply(fd, "OK led.status=%s", value);
    } else if (!strcmp(param, "time.ntp")) {
        if (sys_time_set_server(value) != 0) { reply(fd, "ERR time.ntp must be a hostname or IPv4 address"); return -1; }
        reply(fd, "OK time.ntp=%s", value);
    } else if (!strcmp(param, "time.tz")) {
        if (sys_time_set_tz(value) != 0) { reply(fd, "ERR unknown time zone %s", value); return -1; }
        reply(fd, "OK time.tz=%s", value);
    } else if (!strcmp(param, "net.ap_fallback")) {
        if (strcmp(value, "on") && strcmp(value, "off")) { reply(fd, "ERR net.ap_fallback must be on|off"); return -1; }
        net_set_fallback(!strcmp(value, "on"));
        reply(fd, "OK net.ap_fallback=%s", value);
    } else if (!strcmp(param, "speaker.volume")) {
        int v = atoi(value);
        if (v < 0 || v > 100) { reply(fd, "ERR speaker.volume range [0,100]"); return -1; }
        speaker_set_volume(v);
        reply(fd, "OK speaker.volume=%d", v);
    } else if (reg_find(param)) {
        const ctrl_param_t *r = reg_find(param);
        const char *err = "invalid value";
        if (r->set(value, &err) != 0) { reply(fd, "ERR %s: %s", param, err); return -1; }
        if (r->flags & CTRL_SECRET) reply(fd, "OK %s=(set)", param);
        else { char v[256] = ""; r->get(v, sizeof(v)); reply(fd, "OK %s=%s", param, v); }
        if (!g_loading_settings && (r->flags & CTRL_PERSIST)) ext_save();
        return 1;                                     /* saved separately */
    } else if (!strcmp(param, "venc.minqp") || !strcmp(param, "venc.maxqp")) {
        reply(fd, "ERR %s is read-only in this version "
                  "(requires an encoder restart, not yet implemented)", param);
        return -1;
    } else {
        reply(fd, "ERR unknown param %s", param);
        return -1;
    }
    return 0;
}

/* Blocking byte-at-a-time line reader — traffic on this protocol is tiny
 * (a human moving sliders), so simplicity wins over buffered-recv efficiency. */
static int read_line(int fd, char *buf, size_t cap)
{
    size_t i = 0;
    for (;;) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;  /* EOF or error */
        if (c == '\r') continue;
        if (c == '\n') { buf[i] = '\0'; return (int)i; }
        if (i < cap - 1) buf[i++] = c;
    }
}

/* Runs one LIST/GET/SET command line. Caller must already be authorised. */
static void exec_command(sink_t *out, char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " ", &save);
    if (!cmd) return;

    pthread_mutex_lock(&g_exec_lock);
    if (!strcasecmp(cmd, "LIST")) {
        do_list(out);
    } else if (!strcasecmp(cmd, "GET")) {
        char *param = strtok_r(NULL, " ", &save);
        if (!param) reply(out, "ERR GET requires a param name");
        else        do_get(out, param);
    } else if (!strcasecmp(cmd, "SET")) {
        char *param = strtok_r(NULL, " ", &save);
        char *value = strtok_r(NULL, " ", &save);
        if (!param || !value) reply(out, "ERR SET requires param and value");
        else                  do_set(out, param, value);
    } else {
        reply(out, "ERR unknown command %s", cmd);
    }
    pthread_mutex_unlock(&g_exec_lock);
}

int control_exec(const char *line, char *out, size_t cap)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", line);
    sink_t s = { -1, out, cap, 0 };
    if (cap) out[0] = '\0';
    exec_command(&s, tmp);
    return (int)s.len;
}

/* -------------------------------------------------------------------------
 * Persistent settings: every successful SET rewrites SETTINGS_FILE (on the
 * CONFIG JFFS2 partition) with the current value of each persisted param;
 * control_load_settings() replays it at boot as SET commands.
 * ------------------------------------------------------------------------- */
static const char *const k_persisted[] = {
    "night.mode", "night.trigger_hw_exp", "night.day_hw_exp",
    "night.confirm_samples", "night.lock_ms",
    "ae.stable_range", "ae.hold_range", "ae.speed", "ae.exp_max", "ae.enabled",
    "isp.saturation", "isp.contrast", "isp.brightness", "isp.sharpness",
    "image.flip", "image.mirror",
    "ir.mode", "led.status", "time.ntp", "time.tz", "net.ap_fallback",
    "speaker.volume",
};

static void settings_save(void)
{
    char buf[1024];
    size_t len = 0;
    for (size_t i = 0; i < sizeof(k_persisted) / sizeof(k_persisted[0]); i++) {
        char one[128];
        sink_t s = { -1, one, sizeof(one), 0 };
        one[0] = 0;
        do_get(&s, k_persisted[i]);            /* "name=value\n" */
        char *eq = strchr(one, '=');
        if (!eq || !strncmp(one, "ERR", 3)) continue;
        *eq = ' ';
        size_t n = strlen(one);
        if (len + n < sizeof(buf)) { memcpy(buf + len, one, n); len += n; }
    }
    const char *tmp = SETTINGS_FILE ".tmp";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("[control] settings save"); return; }
    if (write(fd, buf, len) != (ssize_t)len || fsync(fd) != 0) {
        perror("[control] settings write");
        close(fd); unlink(tmp); return;
    }
    close(fd);
    if (rename(tmp, SETTINGS_FILE) != 0) { perror("[control] settings rename"); unlink(tmp); }
}

void control_load_settings(void)
{
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) { printf("[control] no saved settings, using defaults\n"); return; }
    char line[160];
    int applied = 0;
    g_loading_settings = 1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        int known = 0;
        for (size_t i = 0; i < sizeof(k_persisted) / sizeof(k_persisted[0]); i++)
            if (!strcmp(line, k_persisted[i])) known = 1;
        if (!known) continue;
        char cmd[200], res[160];
        snprintf(cmd, sizeof(cmd), "SET %s %s", line, sp + 1);
        control_exec(cmd, res, sizeof(res));
        res[strcspn(res, "\n")] = 0;
        printf("[control] restore: %s\n", res);
        applied++;
    }
    g_loading_settings = 0;
    fclose(f);
    printf("[control] restored %d saved settings\n", applied);
}

/* A saved CTRL_PERSIST value, read straight from the file: for the few
 * settings that are needed BEFORE control_load_settings() runs (the encoder
 * profile is chosen while starting the video pipeline). 0 = found. */
int control_saved_get(const char *name, char *out, size_t cap)
{
    FILE *f = fopen(EXT_SETTINGS_FILE, "r");
    if (!f) return -1;
    char line[320];
    size_t nl = strlen(name);
    int rc = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, name, nl) && line[nl] == '\t') {
            char *v = line + nl + 1, *e = strpbrk(v, "\r\n");
            if (e) *e = 0;
            snprintf(out, cap, "%s", v);
            rc = 0;
            break;
        }
    }
    fclose(f);
    return rc;
}

/* Registered (CTRL_PERSIST) params: "name<TAB>value" lines in EXT_SETTINGS_FILE.
 * Values are single-line printable text (setters reject control chars). */
static void ext_save(void)
{
    char *buf = malloc(16384);
    if (!buf) return;
    size_t len = 0;
    for (int i = 0; i < g_nreg; i++) {
        if (!(g_reg[i].flags & CTRL_PERSIST)) continue;
        char v[256] = "";
        g_reg[i].get(v, sizeof(v));
        int n = snprintf(buf + len, 16384 - len, "%s\t%s\n", g_reg[i].name, v);
        if (n < 0 || len + (size_t)n >= 16384) break;
        len += (size_t)n;
    }
    const char *tmp = EXT_SETTINGS_FILE ".tmp";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        int ok = write(fd, buf, len) == (ssize_t)len && fsync(fd) == 0;
        close(fd);
        if (!ok || rename(tmp, EXT_SETTINGS_FILE) != 0) { perror("[control] ext settings save"); unlink(tmp); }
    }
    free(buf);
}

void control_load_ext_settings(void)
{
    FILE *f = fopen(EXT_SETTINGS_FILE, "r");
    if (!f) return;
    char line[512];
    int n = 0;
    g_loading_settings = 1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const ctrl_param_t *r = reg_find(line);
        if (!r || !(r->flags & CTRL_PERSIST)) continue;
        const char *err = "";
        if (r->set(tab + 1, &err) == 0) n++;
        else printf("[control] restore %s failed: %s\n", line, err);
    }
    g_loading_settings = 0;
    fclose(f);
    printf("[control] restored %d module settings\n", n);
}

int control_set_value(const char *name, const char *value, const char **err)
{
    const ctrl_param_t *r = reg_find(name);
    if (!r) { *err = "unknown setting"; return -1; }
    for (const char *p = value; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f) { *err = "control characters not allowed"; return -1; }
    pthread_mutex_lock(&g_exec_lock);
    char before[512] = "", after[512] = "";
    r->get(before, sizeof before);
    int ret = r->set(value, err);
    if (ret == 0) r->get(after, sizeof after);
    /* only write flash when the value really changed (MQTT/HA may resend) */
    if (ret == 0 && (r->flags & CTRL_PERSIST) && strcmp(before, after) != 0) ext_save();
    pthread_mutex_unlock(&g_exec_lock);
    return ret;
}

int control_get_value(const char *name, char *out, size_t cap, int reveal_secret)
{
    const ctrl_param_t *r = reg_find(name);
    if (!r) return -1;
    out[0] = 0;
    pthread_mutex_lock(&g_exec_lock);
    r->get(out, cap);
    pthread_mutex_unlock(&g_exec_lock);
    if ((r->flags & CTRL_SECRET) && !reveal_secret && out[0]) snprintf(out, cap, "(set)");
    return 0;
}

int control_params_json(const char *prefix, char *out, size_t cap)
{
    size_t n = 0, pl = strlen(prefix);
    n += (size_t)snprintf(out + n, cap - n, "{");
    for (int i = 0; i < g_nreg && n < cap; i++) {
        if (strncmp(g_reg[i].name, prefix, pl)) continue;
        char v[256];
        control_get_value(g_reg[i].name, v, sizeof(v), 0);
        n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s\"%s\":\"", n > 1 ? "," : "", g_reg[i].name);
        for (const char *p = v; *p && n + 8 < cap; p++) {
            if (*p == '"' || *p == '\\') out[n++] = '\\';
            out[n++] = *p;
        }
        n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "\"");
    }
    n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "}");
    return n < cap ? (int)n : (int)cap - 1;
}

/* After an ISP calibration reload (day/night switch) the picture sliders
 * are back at calibration defaults: apply the configured values again. */
void control_reapply_picture(void)
{
    printf("[control] re-applying picture settings after ISP reload\n");
    pthread_mutex_lock(&g_exec_lock);
    if (g_isp_saturation) isp_set_saturation(g_isp_saturation);
    if (g_isp_contrast)   isp_set_contrast(g_isp_contrast);
    if (g_isp_brightness) isp_set_brightness(g_isp_brightness);
    if (g_isp_sharpness)  isp_set_sharpness(g_isp_sharpness);
    pthread_mutex_unlock(&g_exec_lock);
    imgx_apply_all();
}

void control_reset_settings(void)
{
    unlink(SETTINGS_FILE);
    sync();
    printf("[control] saved settings deleted (defaults after reboot)\n");
}

static void set_rcv_timeout(int fd, int seconds)
{
    struct timeval tv = { seconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void handle_control_client(int fd, const struct sockaddr *peer)
{
    char line[256];
    sink_t out = { fd, NULL, 0, 0 };
    int authed = 0, failures = 0;
    uint32_t t0 = mono_s();

    reply(&out, "AUTH required: send 'AUTH <user> <password>'");
    set_rcv_timeout(fd, 30);   /* unauthenticated clients can't hog the port */

    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n < 0) return;
        if (!authed && (mono_s() - t0 > 20 || failures >= 5)) return;   /* log in promptly or go */
        if (n == 0) continue;

        if (!authed) {
            if (auth_throttled(peer)) { reply(&out, "ERR too many failed logins, try later"); return; }
            char *save = NULL;
            char *cmd  = strtok_r(line, " ", &save);
            char *user = strtok_r(NULL, " ", &save);
            char *pass = save;  /* rest of line: password may contain spaces */
            if (!cmd || strcasecmp(cmd, "AUTH") || !user || !pass || !*pass) {
                failures++;                              /* any pre-auth line counts */
                reply(&out, "ERR AUTH required");
                continue;
            }
            int ok = auth_check_plain(user, pass);
            auth_record(peer, ok);
            if (ok) {
                authed = 1;
                set_rcv_timeout(fd, 0);
                pthread_mutex_lock(&g_client_lock);
                g_client_authed = 1;
                pthread_mutex_unlock(&g_client_lock);
                reply(&out, "OK authenticated");
            } else {
                sleep(1 + failures);   /* slow down guessing */
                if (++failures >= 5) { reply(&out, "ERR too many failures"); return; }
                reply(&out, "ERR bad credentials");
            }
            continue;
        }
        exec_command(&out, line);
    }
}

static void *control_accept_loop(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[control] socket"); return NULL; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    /* Loopback only: the protocol sends the admin password in clear text.
     * Use an SSH/telnet session on the camera, or the web API, instead. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(CONTROL_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[control] bind"); close(server_fd); return NULL;
    }
    if (listen(server_fd, 1) < 0) {
        perror("[control] listen"); close(server_fd); return NULL;
    }

    g_server_fd = server_fd;
    printf("[control] tuning server listening on 127.0.0.1:%d\n", CONTROL_PORT);

    while (g_control_running) {
        struct sockaddr_in client_addr;
        socklen_t alen = sizeof(client_addr);
        int fd = accept(server_fd, (struct sockaddr *)&client_addr, &alen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;  /* server_fd closed by control_stop() */
        }

        pthread_mutex_lock(&g_client_lock);
        if (g_client_fd >= 0) close(g_client_fd);  /* single-client: replace previous */
        g_client_fd = fd;
        g_client_authed = 0;
        pthread_mutex_unlock(&g_client_lock);

        printf("[control] tuning client connected\n");
        handle_control_client(fd, (const struct sockaddr *)&client_addr);

        pthread_mutex_lock(&g_client_lock);
        if (g_client_fd == fd) { close(fd); g_client_fd = -1; g_client_authed = 0; }
        pthread_mutex_unlock(&g_client_lock);
        printf("[control] tuning client disconnected\n");
    }

    return NULL;
}

int control_sd_log_fd(void) { return g_sd_log; }

int control_start(void)
{
    /* Tee stdout BEFORE any other init runs, so the full boot log (ISP,
     * VI, VENC) reaches the tuning tab's debug view, not just ae/night's
     * own logging. */
    g_orig_stdout = dup(STDOUT_FILENO);
    if (access(SD_LOG_FLAG, F_OK) == 0)
        g_sd_log = open(SD_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("[control] pipe"); return -1; }
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) { perror("[control] dup2"); return -1; }
    close(pipefd[1]);
    g_log_pipe_r = pipefd[0];
    setvbuf(stdout, NULL, _IOLBF, 0);  /* line-buffered so printf flushes promptly through the pipe */

    if (pthread_create(&g_log_tid, NULL, log_pump, NULL) != 0) {
        perror("[control] log_pump pthread_create");
        return -1;
    }

    g_control_running = 1;
    if (pthread_create(&g_control_tid, NULL, control_accept_loop, NULL) != 0) {
        perror("[control] pthread_create");
        g_control_running = 0;
        return -1;
    }
    return 0;
}

void control_stop(void)
{
    g_control_running = 0;
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
        close(g_server_fd);
        g_server_fd = -1;
    }
    pthread_mutex_lock(&g_client_lock);
    if (g_client_fd >= 0) {
        /* shutdown() before close() — the accept thread may be blocked in
         * recv() inside handle_control_client(); shutdown() forces that
         * call to return cleanly instead of racing a bare close(). */
        shutdown(g_client_fd, SHUT_RDWR);
        close(g_client_fd);
        g_client_fd = -1;
    }
    pthread_mutex_unlock(&g_client_lock);
    pthread_join(g_control_tid, NULL);
    /* log_pump isn't joined — it blocks on read() of the tee'd stdout pipe
     * and only returns when that pipe closes at process exit, which is
     * always imminent by the time control_stop() runs. Acceptable for a
     * debug tool's shutdown path. */
}
