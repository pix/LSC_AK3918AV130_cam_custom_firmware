#include "record.h"
#include "fmp4.h"
#include "live.h"
#include "venc.h"
#include "events.h"
#include "snapshot.h"
#include "monotime.h"
#include "ctrl_params.h"
#include "sched.h"
#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* ---- settings --------------------------------------------------------------- */
enum { M_OFF, M_MOTION, M_CONT, M_SOUND, M_EVENTS };
static int s_mode, s_pre = 3, s_post = 10, s_segment = 10, s_audio = 1, s_snaps = 1, s_keep_free = 500;
static int mode_get(char *o, size_t c)
{
    snprintf(o, c, "%s", s_mode == M_MOTION ? "motion" : s_mode == M_SOUND ? "sound" : s_mode == M_EVENTS ? "events" :
                         s_mode == M_CONT ? "continuous" : "off");
    return 0;
}
static int mode_set(const char *v, const char **e)
{
    if (!strcmp(v, "off")) s_mode = M_OFF; else if (!strcmp(v, "motion")) s_mode = M_MOTION;
    else if (!strcmp(v, "sound")) s_mode = M_SOUND;
    else if (!strcmp(v, "events")) s_mode = M_EVENTS;
    else if (!strcmp(v, "continuous")) s_mode = M_CONT;
    else { *e = "must be off, motion, sound, events or continuous"; return -1; }
    return 0;
}
CTRL_INT(p_pre, s_pre, 0, 4)
CTRL_INT(p_post, s_post, 2, 120)
CTRL_INT(p_seg, s_segment, 1, 60)
CTRL_BOOL(p_audio, s_audio)
CTRL_BOOL(p_snaps, s_snaps)
CTRL_INT(p_keep, s_keep_free, 100, 100000)
/* record.keep_days: delete days older than this (0 = keep until space runs out) */
static int s_keep_days;
CTRL_INT(p_kdays, s_keep_days, 0, 3650)
/* timelapse: one keyframe every interval seconds into a daily file played back at fps */
static int s_tl_interval, s_tl_fps = 15;
static int tl_get(char *o, size_t c) { snprintf(o, c, "%d", s_tl_interval); return 0; }
static int tl_set(const char *v, const char **e)
{
    char *end; long x = strtol(v, &end, 10);
    if (!*v || *end || (x != 0 && (x < 5 || x > 3600))) { *e = "0 (off) or 5..3600 seconds"; return -1; }
    s_tl_interval = (int)x;
    return 0;
}
CTRL_INT(p_tlfps, s_tl_fps, 1, 30)

/* ---- state -------------------------------------------------------------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   /* file listing/deletion vs writer */
static volatile int g_motion_on, g_sound_on;
static volatile int64_t g_motion_off_at, g_sound_off_at;
static char g_cur[96], g_err[96], g_tl_cur[96];
static int64_t g_bytes_written;

static int sd_present(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char l[256]; int ok = 0;
    while (fgets(l, sizeof l, f)) if (strstr(l, " " REC_ROOT " ")) { ok = 1; break; }
    fclose(f);
#ifdef REC_TEST_NOMOUNT
    ok = 1;
#endif
    return ok;
}

static void sd_space(uint64_t *total_mb, uint64_t *free_mb)
{
    struct statvfs v;
    *total_mb = *free_mb = 0;
    if (statvfs(REC_ROOT, &v) == 0) {
        *total_mb = (uint64_t)v.f_blocks * v.f_frsize / 1048576;
        *free_mb = (uint64_t)v.f_bavail * v.f_frsize / 1048576;
    }
}

static int is_day(const char *d)
{
    if (strlen(d) != 10 || d[4] != '-' || d[7] != '-') return 0;
    for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && !isdigit((unsigned char)d[i])) return 0;
    return 1;
}
static int is_name(const char *n)
{
    size_t l = strlen(n);
    if (l < 12 || l > 40 || n[0] == '.') return 0;
    for (const char *p = n; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) return 0;
    if (strstr(n, "..")) return 0;
    const char *ext = strrchr(n, '.');
    return ext && (!strcmp(ext, ".mp4") || !strcmp(ext, ".jpg"));
}

int record_path(const char *day, const char *name, char *out, size_t cap)
{
    if (!is_day(day) || !is_name(name)) return -1;
    snprintf(out, cap, "%s/%s/%s", REC_DIR, day, name);
    return 0;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Delete the oldest files until keep_free MB (and 3% of the card) are free. */
static void expire_days(void);
static void make_room(void)
{
    expire_days();
    for (int guard = 0; guard < 200; guard++) {
        uint64_t tot, fr;
        sd_space(&tot, &fr);
        uint64_t want = (uint64_t)s_keep_free;
        if (tot / 33 > want) want = tot / 33;
        if (fr >= want || !tot) return;
        DIR *d = opendir(REC_DIR);
        if (!d) return;
        char oldest_day[16] = "";
        struct dirent *e;
        while ((e = readdir(d))) if (is_day(e->d_name) && (!oldest_day[0] || strcmp(e->d_name, oldest_day) < 0)) snprintf(oldest_day, sizeof oldest_day, "%s", e->d_name);
        closedir(d);
        if (!oldest_day[0]) return;
        char dp[128];
        snprintf(dp, sizeof dp, "%s/%s", REC_DIR, oldest_day);
        DIR *dd = opendir(dp);
        char oldest[64] = "";
        if (dd) {
            while ((e = readdir(dd))) if (is_name(e->d_name) && (!oldest[0] || strcmp(e->d_name, oldest) < 0)) snprintf(oldest, sizeof oldest, "%s", e->d_name);
            closedir(dd);
        }
        char fp[200];
        if (oldest[0] && strcmp(oldest, g_cur + (strlen(g_cur) > 11 ? 11 : 0)) != 0 &&
            strcmp(oldest, g_tl_cur + (strlen(g_tl_cur) > 11 ? 11 : 0)) != 0) {
            snprintf(fp, sizeof fp, "%s/%s", dp, oldest);
            unlink(fp);
            printf("[rec] deleted %s/%s to keep space free\n", oldest_day, oldest);
        } else if (!oldest[0]) {
            rmdir(dp);                                 /* empty day */
        } else return;                                 /* only the file being written is left */
    }
}

/* Delete day folders older than keep_days (whole days: a folder dated D goes
 * once D < today - keep_days). Never without a synced clock: before NTP the
 * camera thinks it is 2000-01-01, and a clock set wrongly forward must not
 * be able to wipe the card. The files being written are kept. Call with
 * g_lock held. */
static void expire_days(void)
{
    if (!s_keep_days || !sys_time_synced()) return;
    time_t cut = time(NULL) - (time_t)s_keep_days * 86400;
    struct tm tm;
    localtime_r(&cut, &tm);
    char cutoff[16];
    strftime(cutoff, sizeof cutoff, "%Y-%m-%d", &tm);
    DIR *d = opendir(REC_DIR);
    if (!d) return;
    struct dirent *e;
    char old[32][16];
    int n = 0;
    while ((e = readdir(d)) && n < 32)
        if (is_day(e->d_name) && strcmp(e->d_name, cutoff) < 0) snprintf(old[n++], sizeof old[0], "%s", e->d_name);
    closedir(d);
    for (int i = 0; i < n; i++) {
        char dp[128];
        snprintf(dp, sizeof dp, "%s/%s", REC_DIR, old[i]);
        DIR *dd = opendir(dp);
        if (!dd) continue;
        int kept = 0, gone = 0;
        while ((e = readdir(dd))) {
            if (!is_name(e->d_name)) continue;
            if (record_is_live(old[i], e->d_name)) { kept++; continue; }
            char fp[200];
            snprintf(fp, sizeof fp, "%s/%s", dp, e->d_name);
            if (unlink(fp) == 0) gone++;
        }
        closedir(dd);
        if (!kept) rmdir(dp);
        printf("[rec] removed %d file(s) of %s (older than %d days)\n", gone, old[i], s_keep_days);
    }
}

static FILE *new_file(const char *kind, char *rel, size_t rcap)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char day[16], path[200];
    strftime(day, sizeof day, "%Y-%m-%d", &tm);
    mkdir(REC_DIR, 0755);
    snprintf(path, sizeof path, "%s/%s", REC_DIR, day);
    mkdir(path, 0755);
    snprintf(rel, rcap, "%s/%02d-%02d-%02d_%s", day, tm.tm_hour, tm.tm_min, tm.tm_sec, kind);
    snprintf(path, sizeof path, "%s/%s", REC_DIR, rel);
    return fopen(path, "wb");
}

void record_event_snapshot(const char *kind)
{
    if (!s_snaps || !sd_present()) return;
    static uint8_t jpg[200 * 1024];
    int n = snapshot_jpeg(jpg, sizeof jpg, 0);
    if (n <= 0) return;
    char rel[96], jkind[24];
    snprintf(jkind, sizeof jkind, "%s.jpg", kind);
    pthread_mutex_lock(&g_lock);
    make_room();
    FILE *f = new_file(jkind, rel, sizeof rel);
    if (f) { fwrite(jpg, 1, (size_t)n, f); fclose(f); }
    pthread_mutex_unlock(&g_lock);
}

static void on_event(const char *kind, int on)
{
    if (!strcmp(kind, "motion")) { g_motion_on = on; if (!on) g_motion_off_at = mono_ms(); }
    else if (!strcmp(kind, "sound")) { g_sound_on = on; if (!on) g_sound_off_at = mono_ms(); }
}

/* Should an event clip be running now (trigger active, or within post-roll)? */
static int event_wanted(int64_t now, const char **kind)
{
    int64_t post = (int64_t)s_post * 1000;
    int m = (s_mode == M_MOTION || s_mode == M_EVENTS) && (g_motion_on || (g_motion_off_at && now - g_motion_off_at < post));
    int snd = (s_mode == M_SOUND || s_mode == M_EVENTS) && (g_sound_on || (g_sound_off_at && now - g_sound_off_at < post));
    *kind = m && snd ? "event" : m ? "motion" : "sound";
    return m || snd;
}

/* ---- writer thread ------------------------------------------------------------ */
static void *rec_thread(void *arg)
{
    (void)arg;
    uint8_t *frame = malloc(LIVE_MAX_FRAME);
    live_item_t *it = malloc(sizeof *it);
    if (!frame || !it) return NULL;
    live_cursor_t cur = { 0, 0 };
    int subscribed = 0;
    FILE *f = NULL;
    fmp4_t *m = NULL;
    int64_t delta = 0, delta_at = 0, file_started = 0;     /* mono_ms - encoder_ts */
    int have_delta = 0, waiting_key = 0, closing = 0;
    int64_t last_expire = 0;
    for (;;) {
        if (mono_ms() - last_expire > 3600000 && sd_present()) {     /* hourly, recording or not */
            last_expire = mono_ms();
            pthread_mutex_lock(&g_lock);
            expire_days();
            pthread_mutex_unlock(&g_lock);
        }
        int want_sub = s_mode != M_OFF && sd_present();
        if (want_sub && !subscribed) { live_subscribe(); subscribed = 1; memset(&cur, 0, sizeof cur); g_err[0] = 0; }
        if (!want_sub) {
            if (m) { fmp4_close(m); m = NULL; }
            if (f) { fclose(f); f = NULL; g_cur[0] = 0; }
            closing = 0; waiting_key = 0;          /* no stale end-of-file request */
            if (subscribed) { live_unsubscribe(); subscribed = 0; }
            if (s_mode != M_OFF && !sd_present()) snprintf(g_err, sizeof g_err, "no SD card");
            sleep(1);
            continue;
        }
        int k = live_next(&cur, frame, LIVE_MAX_FRAME, it, 300);
        int64_t now = mono_ms();
        if (k == LIVE_VIDEO) {
            int64_t d = now - (int64_t)it->ts;
            /* encoder-clock -> monotonic offset: smallest receive latency seen
             * (only between files, and never from a rewound backlog frame) */
            if (!m && (!have_delta || d < delta || (now - delta_at > 30000 && d < delta + 300))) { delta = d; delta_at = now; have_delta = 1; }
        }

        /* start / stop decisions */
        const char *trig = "motion";
        int should = (s_mode == M_CONT || event_wanted(now, &trig)) && sched_record_ok();
        if (!m && !waiting_key && should) {
            /* pre-roll; with no keyframe in reach (long smart GOP), ask for one */
            if (!(s_mode != M_CONT && s_pre > 0 && live_rewind(&cur, (uint32_t)s_pre * 1000) == 0))
                venc_request_idr();
            waiting_key = 1;
            continue;
        }
        if (m && !should) closing = 1;
        if (m && s_mode == M_CONT && now - file_started > (int64_t)s_segment * 60000) closing = 1;

        if (k == LIVE_VIDEO) {
            if (it->key && (closing || (m && s_mode != M_CONT && now - file_started > 600000))) {   /* end at a keyframe; event clips max 10 min */
                /* "closing" can outlive the file: a mode or schedule change closes
                 * it in the !want_sub branch below, which did not reset the flag,
                 * and fmp4_close(NULL) is a no-op, so an unguarded fclose(f) ran on
                 * a NULL pointer: SIGSEGV inside fclose (seen on the camera). */
                if (m) { fmp4_close(m); m = NULL; }
                if (f) { fclose(f); f = NULL; printf("[rec] closed %s\n", g_cur); }
                g_cur[0] = 0; closing = 0;
                if (!should) continue;
                waiting_key = 1;                         /* next segment starts right here */
            }
            if (waiting_key && it->key) {
                pthread_mutex_lock(&g_lock);
                make_room();
                char rel[96];
                char kname[16];
                snprintf(kname, sizeof kname, "%s.mp4", s_mode == M_CONT ? "cont" : trig);
                f = new_file(kname, rel, sizeof rel);
                pthread_mutex_unlock(&g_lock);
                if (!f) { snprintf(g_err, sizeof g_err, "cannot write to the SD card (%s)", strerror(errno)); waiting_key = 0; sleep(2); continue; }
                m = fmp4_open(f, frame, it->len, s_audio);
                if (!m) { fclose(f); f = NULL; continue; }
                snprintf(g_cur, sizeof g_cur, "%s", rel);
                waiting_key = 0; file_started = now;
                g_err[0] = 0;
                printf("[rec] recording %s\n", rel);
            }
            if (m) {
                if (it->key) fmp4_flush(m);              /* fragment = one GOP */
                if (fmp4_video(m, frame, it->len, it->key, it->ts) != 0) closing = 1;
                if (it->key) { fflush(f); fsync(fileno(f)); }
                g_bytes_written += (int64_t)it->len;
            }
        } else if (k == LIVE_AUDIO && m && s_audio && have_delta) {
            int64_t v0_mono = (int64_t)fmp4_video_start_ms(m) + delta;
            int64_t off = (int64_t)it->ts - v0_mono;
            if (off >= 0) fmp4_audio(m, it->pcm, it->n, (uint64_t)off * 8);
        }
    }
    return NULL;
}

/* ---- timelapse writer ------------------------------------------------------------ */
static void *tl_thread(void *arg)
{
    (void)arg;
    uint8_t *frame = malloc(LIVE_MAX_FRAME);
    live_item_t *it = malloc(sizeof *it);
    if (!frame || !it) return NULL;
    live_cursor_t cur = { 0, 0 };
    int subscribed = 0;
    FILE *f = NULL;
    fmp4_t *m = NULL;
    char day[16] = "";
    int64_t last = 0;
    uint64_t n = 0;
    for (;;) {
        int want = s_tl_interval > 0 && sd_present() && sched_record_ok();
        if (!want) {
            if (m) { fmp4_close(m); m = NULL; }
            if (f) { fclose(f); f = NULL; printf("[rec] timelapse closed %s\n", g_tl_cur); g_tl_cur[0] = 0; }
            if (subscribed) { live_unsubscribe(); subscribed = 0; }
            last = 0;
            sleep(1);
            continue;
        }
        if (!subscribed) { live_subscribe(); subscribed = 1; memset(&cur, 0, sizeof cur); }
        int k = live_next(&cur, frame, LIVE_MAX_FRAME, it, 500);
        if (k != LIVE_VIDEO || !it->key) continue;
        int64_t now = mono_ms();
        if (last && now - last < (int64_t)s_tl_interval * 1000 - 500) continue;
        time_t t = time(NULL);
        struct tm tm;
        char today[16];
        localtime_r(&t, &tm);
        strftime(today, sizeof today, "%Y-%m-%d", &tm);
        if (m && strcmp(today, day)) {                    /* one file per day */
            fmp4_close(m); m = NULL;
            fclose(f); f = NULL;
            printf("[rec] timelapse closed %s\n", g_tl_cur);
            g_tl_cur[0] = 0;
        }
        if (!m) {
            char rel[96];
            pthread_mutex_lock(&g_lock);
            make_room();
            f = new_file("timelapse.mp4", rel, sizeof rel);
            if (f) snprintf(g_tl_cur, sizeof g_tl_cur, "%s", rel);
            pthread_mutex_unlock(&g_lock);
            if (!f) { snprintf(g_err, sizeof g_err, "cannot write to the SD card (%s)", strerror(errno)); sleep(5); continue; }
            m = fmp4_open(f, frame, it->len, 0);
            if (!m) { fclose(f); f = NULL; g_tl_cur[0] = 0; continue; }
            snprintf(day, sizeof day, "%s", today);
            n = 0;
            printf("[rec] timelapse %s (every %d s, %d fps)\n", rel, s_tl_interval, s_tl_fps);
        }
        fmp4_flush(m);                                     /* writes the previous picture */
        if (fmp4_video(m, frame, it->len, 1, n * 1000 / (uint64_t)s_tl_fps) != 0) {
            fmp4_close(m); m = NULL; fclose(f); f = NULL; g_tl_cur[0] = 0; continue;
        }
        fflush(f); fsync(fileno(f));
        n++;
        last = now;
    }
    return NULL;
}

/* ---- web helpers ------------------------------------------------------------------- */
int record_status_json(char *out, size_t cap)
{
    uint64_t tot = 0, fr = 0;
    int sd = sd_present();
    if (sd) sd_space(&tot, &fr);
    char mode[16];
    mode_get(mode, sizeof mode);
    char sc[96];
    sched_status_json(sc, sizeof sc);
    return snprintf(out, cap, "{\"sd\":%s,\"total_mb\":%llu,\"free_mb\":%llu,\"mode\":\"%s\",\"recording\":\"%s\",\"timelapse\":\"%s\",\"error\":\"%s\",\"schedule\":%s}",
                    sd ? "true" : "false", (unsigned long long)tot, (unsigned long long)fr, mode, g_cur, g_tl_cur, g_err, sc);
}

int record_list_json(const char *day, char *out, size_t cap)
{
    size_t o = 0;
    pthread_mutex_lock(&g_lock);
    if (!day || !*day) {
        char *days[400]; int n = 0;
        DIR *d = opendir(REC_DIR);
        struct dirent *e;
        while (d && (e = readdir(d)) && n < 400) if (is_day(e->d_name)) days[n++] = strdup(e->d_name);
        if (d) closedir(d);
        qsort(days, (size_t)n, sizeof(char *), cmpstr);
        o += (size_t)snprintf(out + o, cap - o, "{\"days\":[");
        for (int i = n - 1; i >= 0; i--) {                       /* newest first */
            char dp[128]; int files = 0; unsigned long long bytes = 0;
            snprintf(dp, sizeof dp, "%s/%s", REC_DIR, days[i]);
            DIR *dd = opendir(dp);
            while (dd && (e = readdir(dd))) {
                if (!is_name(e->d_name)) continue;
                char fp[200]; struct stat st;
                snprintf(fp, sizeof fp, "%s/%s", dp, e->d_name);
                if (stat(fp, &st) == 0) { files++; bytes += (unsigned long long)st.st_size; }
            }
            if (dd) closedir(dd);
            if (o < cap) o += (size_t)snprintf(out + o, cap - o, "%s{\"day\":\"%s\",\"files\":%d,\"bytes\":%llu}", i == n - 1 ? "" : ",", days[i], files, bytes);
            free(days[i]);
        }
        if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]}");
    } else {
        if (!is_day(day)) { pthread_mutex_unlock(&g_lock); return snprintf(out, cap, "{\"files\":[]}"); }
        char dp[128];
        snprintf(dp, sizeof dp, "%s/%s", REC_DIR, day);
        char *names[2000]; int n = 0;
        DIR *dd = opendir(dp);
        struct dirent *e;
        while (dd && (e = readdir(dd)) && n < 2000) if (is_name(e->d_name)) names[n++] = strdup(e->d_name);
        if (dd) closedir(dd);
        qsort(names, (size_t)n, sizeof(char *), cmpstr);
        o += (size_t)snprintf(out + o, cap - o, "{\"day\":\"%s\",\"files\":[", day);
        for (int i = n - 1; i >= 0; i--) {
            char fp[200]; struct stat st;
            snprintf(fp, sizeof fp, "%s/%s", dp, names[i]);
            long long sz = stat(fp, &st) == 0 ? (long long)st.st_size : 0;
            int live = g_cur[0] && !strcmp(g_cur + 11, names[i]);
            if (o + 120 < cap) o += (size_t)snprintf(out + o, cap - o, "%s{\"name\":\"%s\",\"size\":%lld%s}", i == n - 1 ? "" : ",", names[i], sz, live ? ",\"recording\":true" : "");
            free(names[i]);
        }
        if (o < cap) o += (size_t)snprintf(out + o, cap - o, "]}");
    }
    pthread_mutex_unlock(&g_lock);
    return o < cap ? (int)o : (int)cap - 1;
}

int record_delete(const char *day, const char *name)
{
    if (!is_day(day)) return -1;
    char dp[128], fp[200];
    snprintf(dp, sizeof dp, "%s/%s", REC_DIR, day);
    pthread_mutex_lock(&g_lock);
    int ret = 0;
    if (name && *name) {
        if (!is_name(name) || (g_cur[0] && !strcmp(g_cur + 11, name))) ret = -1;     /* not the file being written */
        else { snprintf(fp, sizeof fp, "%s/%s", dp, name); ret = unlink(fp); rmdir(dp); }   /* rmdir: only if now empty */
    } else {
        DIR *dd = opendir(dp);
        struct dirent *e;
        while (dd && (e = readdir(dd))) {
            if (!is_name(e->d_name) || (g_cur[0] && !strncmp(g_cur, day, 10) && !strcmp(g_cur + 11, e->d_name))) continue;
            snprintf(fp, sizeof fp, "%s/%s", dp, e->d_name);
            unlink(fp);
        }
        if (dd) closedir(dd);
        rmdir(dp);
    }
    pthread_mutex_unlock(&g_lock);
    return ret;
}

void record_init(void)
{
    ctrl_param_t pm = { "record.mode", mode_get, mode_set, CTRL_PERSIST };
    control_register(&pm);
    CTRL_REG("record.pre", p_pre, CTRL_PERSIST);
    CTRL_REG("record.post", p_post, CTRL_PERSIST);
    CTRL_REG("record.segment", p_seg, CTRL_PERSIST);
    CTRL_REG("record.audio", p_audio, CTRL_PERSIST);
    CTRL_REG("record.snapshots", p_snaps, CTRL_PERSIST);
    CTRL_REG("record.keep_free", p_keep, CTRL_PERSIST);
    CTRL_REG("record.keep_days", p_kdays, CTRL_PERSIST);
    ctrl_param_t tl = { "timelapse.interval", tl_get, tl_set, CTRL_PERSIST };
    control_register(&tl);
    CTRL_REG("timelapse.fps", p_tlfps, CTRL_PERSIST);
}

void record_start(void)
{
    events_set_hook(on_event);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, rec_thread, NULL);
    pthread_create(&t, &a, tl_thread, NULL);
    pthread_attr_destroy(&a);
}

/* Every recording, newest first: [{"d":day,"n":name,"s":size},...] (malloc'd). */
char *record_list_all_json(size_t *len)
{
    size_t cap = 65536, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    pthread_mutex_lock(&g_lock);
    char *days[400]; int nd = 0;
    DIR *d = opendir(REC_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) && nd < 400) if (is_day(e->d_name)) days[nd++] = strdup(e->d_name);
    if (d) closedir(d);
    qsort(days, (size_t)nd, sizeof(char *), cmpstr);
    out[o++] = '[';
    for (int i = nd - 1; i >= 0; i--) {
        char dp[128];
        snprintf(dp, sizeof dp, "%s/%s", REC_DIR, days[i]);
        char *names[3000]; int n = 0;
        DIR *dd = opendir(dp);
        while (dd && (e = readdir(dd)) && n < 3000) if (is_name(e->d_name)) names[n++] = strdup(e->d_name);
        if (dd) closedir(dd);
        qsort(names, (size_t)n, sizeof(char *), cmpstr);
        for (int j = n - 1; j >= 0; j--) {
            char fp[200]; struct stat st;
            snprintf(fp, sizeof fp, "%s/%s", dp, names[j]);
            long long sz = stat(fp, &st) == 0 ? (long long)st.st_size : 0;
            int live = g_cur[0] && !strncmp(g_cur, days[i], 10) && !strcmp(g_cur + 11, names[j]);
            if (o + 160 > cap) { char *nb = realloc(out, cap * 2); if (!nb) { free(names[j]); continue; } out = nb; cap *= 2; }
            o += (size_t)snprintf(out + o, cap - o, "%s{\"d\":\"%s\",\"n\":\"%s\",\"s\":%lld%s}", o > 1 ? "," : "", days[i], names[j], sz, live ? ",\"r\":1" : "");
            free(names[j]);
        }
        free(days[i]);
    }
    pthread_mutex_unlock(&g_lock);
    out[o++] = ']';
    out[o] = 0;
    *len = o;
    return out;
}

/* 1 if this file is being written right now */
int record_active(void) { return g_cur[0] != 0; }

int record_is_live(const char *day, const char *name)
{
    return (g_cur[0] && !strncmp(g_cur, day, 10) && !strcmp(g_cur + 11, name)) ||
           (g_tl_cur[0] && !strncmp(g_tl_cur, day, 10) && !strcmp(g_tl_cur + 11, name));
}
