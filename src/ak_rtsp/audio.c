#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <pthread.h>
#include <math.h>
#include "monotime.h"

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "audio.h"
#include "live.h"
#include "sounddet.h"
#include "speaker.h"
#include "ctrl_params.h"
#include "aec.h"

static int g_audio_fd = -1;

/* Opened alongside capture. A prior debugging session
 * found the likely reason SET_MODE(0) crashed the kernel on this fd in
 * every earlier session: a fresh, path-tagged ground-truth capture (isp_hook.c
 * extended with open()/read()/write() hooks) shows real anyka_ipc opens this
 * exact device O_WRONLY (flags=0x1), not O_RDWR like this code used to —
 * and calls the identical SET_MODE(0) successfully there. Switched the open()
 * call below to O_WRONLY and re-added SET_MODE(0) to test that theory
 * directly. If it still crashes, that disproves the open-mode theory (still
 * useful — narrows the search further) rather than being a regression to
 * avoid. */
static int g_audio_playback_fd = -1;
static int g_audio_loopback_fd = -1;

static volatile int g_playback_running = 0;
static volatile int g_playback_thread_started = 0;
static pthread_t    g_playback_tid;

static volatile int g_audio_running = 0;
static pthread_t    g_audio_tid;

static void audio_close_aux(void); /* closes g_audio_playback_fd/g_audio_loopback_fd */

/* audio.mic_gain: analog microphone gain 0..7, or -1 = leave the driver's value. */
static int s_mic_gain = -1;
static int g_mic_gain_boot = -1;          /* driver's value when capture opened */

static int mic_gain_apply(void)
{
    int32_t v = s_mic_gain >= 0 ? s_mic_gain : g_mic_gain_boot;
    if (g_audio_fd < 0 || v < 0) return 0;
    if (ioctl(g_audio_fd, AKPCM_IOC_AI_SET_GAIN, &v) != 0) {
        printf("[audio] set mic gain %d: %s\n", (int)v, strerror(errno));
        return -1;
    }
    return 0;
}

static int p_micgain_get(char *o, size_t c) { snprintf(o, c, "%d", s_mic_gain); return 0; }
static int p_micgain_set(const char *v, const char **e)
{
    char *end;
    long x = strtol(v, &end, 10);
    if (!*v || *end || x < -1 || x > 7) { *e = "must be -1 (driver default) or 0..7"; return -1; }
    int old = s_mic_gain;
    s_mic_gain = (int)x;
    if (mic_gain_apply() != 0) { s_mic_gain = old; *e = "the audio driver refused the gain"; return -1; }
    return 0;
}

/* ---- software microphone processing ------------------------------------
 * Runs on every captured period before it reaches anything else (RTSP,
 * ONVIF, live view, sound detection, recordings): mute, noise gate, AGC.
 * Pure userspace; the capture configuration itself is never touched. */
static int s_mute;                       /* audio.mute: write silence */
/* audio.mute_on_play: off | on (always mute) | auto (mute only while the mic
 * actually picks the playback up, so a sound nobody hears keeps the live
 * audio running in the browser). auto: mute on a frame at or above -40 dBFS
 * while the speaker plays, release 300 ms after the last such frame. */
#define MOP_OFF 0
#define MOP_ALWAYS 1
#define MOP_AUTO 2
static int s_mute_on_play = MOP_AUTO;
static int64_t g_echo_hold_ms;
static int64_t g_play_seen_ms = -100000;   /* last time the speaker was playing */
#define MOP_AUTO_DB  -40
static int s_agc;                        /* audio.agc: automatic gain */
static int s_gate_db = -90;              /* audio.gate_db: below this, silence (-90 = off) */
static float g_gate_env = 1.0f;          /* gate / AGC state (capture thread only) */
static float g_agc_gain = 1.0f;

static int p_mute_get(char *o, size_t c) { snprintf(o, c, "%s", s_mute ? "on" : "off"); return 0; }
static int p_mute_set(const char *v, const char **e)
{
    if (!strcmp(v, "on") || !strcmp(v, "1")) s_mute = 1;
    else if (!strcmp(v, "off") || !strcmp(v, "0")) s_mute = 0;
    else { *e = "must be off, on or auto"; return -1; }
    return 0;
}
static int p_mop_get(char *o, size_t c)
{ snprintf(o, c, "%s", s_mute_on_play == MOP_ALWAYS ? "on" : s_mute_on_play == MOP_AUTO ? "auto" : "off"); return 0; }
static int p_mop_set(const char *v, const char **e)
{
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "always")) s_mute_on_play = MOP_ALWAYS;
    else if (!strcmp(v, "auto")) s_mute_on_play = MOP_AUTO;
    else if (!strcmp(v, "off") || !strcmp(v, "0")) s_mute_on_play = MOP_OFF;
    else { *e = "must be off, on or auto"; return -1; }
    return 0;
}
static int p_agc_get(char *o, size_t c) { snprintf(o, c, "%s", s_agc ? "on" : "off"); return 0; }
static int p_agc_set(const char *v, const char **e)
{
    if (!strcmp(v, "on") || !strcmp(v, "1")) s_agc = 1;
    else if (!strcmp(v, "off") || !strcmp(v, "0")) s_agc = 0;
    else { *e = "must be off, on or auto"; return -1; }
    return 0;
}
static int p_gate_get(char *o, size_t c) { snprintf(o, c, "%d", s_gate_db); return 0; }
static int p_gate_set(const char *v, const char **e)
{
    char *end; long x = strtol(v, &end, 10);
    if (!*v || *end || x < -90 || x > -20) { *e = "must be -90 (off) to -20 dB"; return -1; }
    s_gate_db = (int)x;
    return 0;
}

/* RMS of a period, and its level in dBFS relative to full scale 32768 */
static int frame_db(const int16_t *s, size_t n)
{
    if (!n) return -120;
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)s[i] * s[i];
    double rms = sqrt(acc / (double)n);
    if (rms < 1) return -120;
    return (int)(20.0 * log10(rms / 32768.0));
}

static int  s_howl;                        /* audio.howl (defined with the suppressor below) */
static void howl_detect(const int16_t *s, size_t n);
static void howl_apply(int16_t *s, size_t n);

static void mic_process(int16_t *s, size_t n)
{
    if (!n) return;
    if (s_mute) { memset(s, 0, n * sizeof(int16_t)); return; }
    /* echo cancellation already ran (capture loop, before this). While it
     * works on a live reference the mic stays open during playback (full
     * duplex): only "always" still mutes. */
    int db = frame_db(s, n);
    int playing = speaker_playing();
    if (playing) g_play_seen_ms = mono_ms();
    int mop = s_mute_on_play;
    if (mop == MOP_AUTO && aec_active()) mop = MOP_OFF;
    if (mop != MOP_OFF && playing) {
        /* always: silence the whole playback. auto: only when this frame
         * actually hears it (a sound nobody can hear keeps the live audio
         * running in the browser), held 300 ms after the last loud frame. */
        if (mop == MOP_ALWAYS || db >= MOP_AUTO_DB) {
            g_echo_hold_ms = mono_ms();
            memset(s, 0, n * sizeof(int16_t));
            return;
        }
    }
    if (mop == MOP_AUTO && mono_ms() - g_echo_hold_ms < 300) {
        memset(s, 0, n * sizeof(int16_t));
        return;
    }
    /* noise gate: open above the threshold; ~50 ms attack, ~400 ms release */
    float target = (s_gate_db > -90 && db < s_gate_db) ? 0.0f : 1.0f;
    /* ~50 ms to open, ~400 ms to close (32 ms periods) */
    float step = target > g_gate_env ? 0.6f : 0.08f;
    if (target > g_gate_env)      g_gate_env = (g_gate_env + step < target) ? g_gate_env + step : target;
    else if (target < g_gate_env) g_gate_env = (g_gate_env - step > target) ? g_gate_env - step : target;
    /* AGC: bring speech to about -20 dBFS (rms 3277); gain limited to 0.25x..8x */
    float g = g_gate_env;
    if (s_agc && db > -60) {
        float rms = 32768.0f * powf(10.0f, db / 20.0f);
        float want = 3277.0f / rms;
        if (want < 0.25f) want = 0.25f;
        if (want > 8.0f) want = 8.0f;
        /* ~0.4 s to turn the gain down, ~1 s to bring it back up (32 ms periods) */
        float k = want < g_agc_gain ? 0.08f : 0.03f;
        g_agc_gain += (want - g_agc_gain) * k;
        g *= g_agc_gain;
    }
    if (g != 1.0f)
        for (size_t i = 0; i < n; i++) {
            int v = (int)(s[i] * g);
            s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    /* howling last: the notch must not be re-amplified by the AGC. Only
     * around playback (a howl needs the speaker): a steady tone in the room
     * (smoke alarm, kettle) must reach recordings and sound detection. */
    if (s_howl && mono_ms() - g_play_seen_ms < 3000) {
        howl_detect(s, n);
        howl_apply(s, n);
    }
}

/* ---- howling (Larsen) suppression (audio.howl) -------------------------
 * A live microphone next to a live speaker howls at one or two frequencies
 * where the loop gain exceeds 1. Detect a narrow band that stands well
 * above its neighbours, then cut exactly that frequency with an adaptive
 * notch (RBJ biquad). Up to 4 notches, released ~1 s after the peak goes.
 * Runs after echo cancellation, before the gate: works with the mic live
 * during playback, which is what a conversation needs. */
#define HOWL_BINS 48
#define HOWL_NOTCHES 4
static int s_howl = 1;
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; double f; int64_t last; int on; } notch_t;
static notch_t g_notch[HOWL_NOTCHES];
static double g_howl_ref;               /* magnitude of the current peak (for the state) */
static int g_howl_count;

static double goertzel(const int16_t *s, size_t n, double f)
{
    double w = 2.0 * 3.141592653589793 * f / (double)AUDIO_SAMPLE_RATE;
    double c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) { double s0 = s[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (double)n;
}

static void howl_detect(const int16_t *s, size_t n)
{
    double mag[HOWL_BINS], fmax = 0; int imax = -1;
    for (int i = 0; i < HOWL_BINS; i++) {
        double f = 200.0 * pow(17.5, (double)i / (HOWL_BINS - 1));   /* 200 Hz .. 3500 Hz */
        mag[i] = goertzel(s, n, f);
        if (mag[i] > fmax) { fmax = mag[i]; imax = i; }
    }
    g_howl_ref = fmax;
    if (imax < 0) return;
    /* neighbours: median of 3 bins either side (skip the peak) */
    double nb[6]; int k = 0;
    for (int d = -3; d <= 3; d++) if (d && imax + d >= 0 && imax + d < HOWL_BINS) nb[k++] = mag[imax + d];
    if (k == 0) return;
    for (int i = 1; i < k; i++) { double v = nb[i]; int j = i - 1; while (j >= 0 && nb[j] > v) { nb[j + 1] = nb[j]; j--; } nb[j + 1] = v; }
    double med = nb[k / 2];
    double ratio = fmax / (med + 1.0);
    if (fmax < 300.0) return;
    /* refine the peak frequency between bins (log-parabola) so the notch
     * lands on the tone, not next to it */
    double f = 200.0 * pow(17.5, (double)imax / (HOWL_BINS - 1));
    if (imax > 0 && imax < HOWL_BINS - 1) {
        double a = mag[imax - 1], b = fmax, c = mag[imax + 1], den = a - 2 * b + c;
        if (fabs(den) > 1e-9) {
            double d = 0.5 * (a - c) / den;
            if (d > -1 && d < 1) f = 200.0 * pow(17.5, (imax + d) / (HOWL_BINS - 1));
        }
    }
    /* fine search around the bin: 0.5 Hz steps over +-3 % so the subtraction
     * cancels the tone almost completely rather than leaving its skirts */
    {
        double best = goertzel(s, n, f), bestf = f;
        for (double d = -0.03; d <= 0.03; d += 0.0006) {
            double ff = f * (1.0 + d), m = goertzel(s, n, ff);
            if (m > best) { best = m; bestf = ff; }
        }
        f = bestf;
    }
    int slot = -1, free_slot = -1;
    for (int i = 0; i < HOWL_NOTCHES; i++) {
        if (g_notch[i].on && fabs(g_notch[i].f - f) / f < 0.03) { slot = i; break; }
        if (!g_notch[i].on && free_slot < 0) free_slot = i;
    }
    if (slot >= 0) {                                                  /* existing notch: keep it alive while the tone is there */
        if (ratio > 2.5) { notch_t *h = &g_notch[slot]; h->last = mono_ms(); h->f = f; }
        return;
    }
    if (ratio < 8.0) return;                                          /* not a narrow howl yet */
    slot = free_slot;
    if (slot < 0) {                                                   /* all busy: replace the oldest */
        slot = 0;
        for (int i = 1; i < HOWL_NOTCHES; i++) if (g_notch[i].last < g_notch[slot].last) slot = i;
    }
    notch_t *q = &g_notch[slot];
    if (!q->on) {
        printf("[howl] suppressing %.0f Hz\n", f);
        g_howl_count++;
    }
    q->f = f; q->on = 1; q->last = mono_ms();
}

/* A Larsen tone is a steady sinusoid: fit sine + cosine over the frame and
 * subtract it. Deeper and simpler than an IIR notch (which was only giving
 * ~5 dB here), and it cannot ring or go unstable. */
static void howl_apply(int16_t *s, size_t n)
{
    int64_t now = mono_ms();
    for (int q = 0; q < HOWL_NOTCHES; q++) {
        notch_t *h = &g_notch[q];
        if (!h->on) continue;
        if (now - h->last > 4000) { h->on = 0; printf("[howl] released %.0f Hz\n", h->f); continue; }
        double w = 2.0 * 3.141592653589793 * h->f / (double)AUDIO_SAMPLE_RATE;
        double cr = cos(w), sr = sin(w), c = 1.0, sn = 0.0;
        double ac = 0.0, as = 0.0;
        for (size_t i = 0; i < n; i++) {
            ac += s[i] * c; as += s[i] * sn;
            double nc = c * cr - sn * sr; sn = sn * cr + c * sr; c = nc;   /* rotate */
        }
        double a = 2.0 * ac / (double)n, b = 2.0 * as / (double)n;
        c = 1.0; sn = 0.0;
        for (size_t i = 0; i < n; i++) {
            double v = s[i] - (a * c + b * sn);
            s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            double nc = c * cr - sn * sr; sn = sn * cr + c * sr; c = nc;
        }
    }
}

/* ---- echo cancellation reference (audio.aec, see aec.c) ----------------
 * The speaker signal is read back through /dev/pcmC0D0l (the PCM driver's
 * loopback tap, which stock anyka_ipc uses as its AEC reference). Polled
 * non-blocking every 2 ms so each period is timestamped close to when it
 * became available; aec.c aligns it with the capture stream from those
 * timestamps. */
static volatile int g_aec_running;
static pthread_t g_aec_tid;
static volatile int g_aec_thread_started;

static void *aec_ref_loop(void *arg)
{
    (void)arg;
    int16_t buf[1024];
    int fails = 0;
    unsigned win_pos = 0;                 /* position inside the 256-sample window */
    while (g_aec_running) {
        ssize_t n = read(g_audio_loopback_fd, buf, sizeof buf);
        int64_t t = mono_us();
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) { usleep(2000); continue; }
            if (++fails > 20) {
                fprintf(stderr, "[aec] loopback read failed (%s): echo cancellation unavailable\n", strerror(errno));
                aec_ref_lost();
                break;
            }
            usleep(20000);
            continue;
        }
        fails = 0;
        if (n == 0) { usleep(2000); continue; }
        /* ak_pcm's copy_to_loopback runs on every playback interrupt (every
         * 128 samples) and copies a 256-sample window starting at the DMA
         * position, so the tap carries overlapping windows at twice the
         * rate (sysfs: loopback last_read = 2x playback last_write). The
         * first 128 samples of each window are the real stream (verified
         * bit-exact on a 440 Hz tone); the second half repeats the next
         * window, and past the DMA ring end it is junk. */
        size_t s = (size_t)n / sizeof(int16_t), k = 0;
        for (size_t i = 0; i < s; i++) {
            if (win_pos < 128) buf[k++] = buf[i];
            win_pos = (win_pos + 1) & 255;
        }
        aec_ref_push(buf, k, t);
    }
    return NULL;
}

/* what the capture thread is doing right now, read-only */
static int p_astate_get(char *o, size_t c)
{
    snprintf(o, c, "mute=%s mute_on_play=%s speaker=%s agc=%s gate=%d",
             s_mute ? "on" : "off", s_mute_on_play == MOP_ALWAYS ? "on" : s_mute_on_play == MOP_AUTO ? "auto" : "off",
             speaker_playing() ? "playing" : "idle", s_agc ? "on" : "off", s_gate_db);
    return 0;
}
static int p_astate_set(const char *v, const char **e) { (void)v; *e = "read-only"; return -1; }

static void aec_start(void)
{
    /* Only touch the loopback when echo cancellation is enabled (saved
     * setting: settings are replayed later in the boot). stock's
     * osal_loopback_open is just open("/dev/pcmC0D0l", O_RDWR); its
     * set_param (the same GET_PARS commit as capture) is what activates it.
     * Loopback period: 1024 bytes (512 samples), from the path-tagged stock
     * capture. */
    if (!aec_saved_on()) {
        printf("[aec] off: the loopback is left untouched\n");
        return;
    }
    if (g_audio_loopback_fd < 0) {
        printf("[aec] no loopback device: echo cancellation unavailable\n");
        return;
    }
    struct akpcm_pars lp;
    memset(&lp, 0, sizeof lp);
    lp.format = 0; lp.rate = AUDIO_SAMPLE_RATE; lp.channels = AUDIO_CHANNELS; lp.bits = AUDIO_BITS;
    lp.period_size = 1024; lp.period_count = 16;
    int rc = ioctl(g_audio_loopback_fd, AKPCM_IOC_GET_PARS, &lp);
    printf("[aec] loopback params commit: %s\n", rc == 0 ? "ok" : strerror(errno));
    fcntl(g_audio_loopback_fd, F_SETFL, fcntl(g_audio_loopback_fd, F_GETFL) | O_NONBLOCK);  /* so stop can join */
    g_aec_running = 1;
    if (pthread_create(&g_aec_tid, NULL, aec_ref_loop, NULL) == 0) { g_aec_thread_started = 1; aec_ref_attach(); }
    else { g_aec_running = 0; printf("[aec] cannot start the reference thread\n"); }
}

static int p_howl_get(char *o, size_t c) { snprintf(o, c, "%s", s_howl ? "on" : "off"); return 0; }
static int p_howl_set(const char *v, const char **e)
{
    if (!strcmp(v, "on") || !strcmp(v, "1")) s_howl = 1;
    else if (!strcmp(v, "off") || !strcmp(v, "0")) s_howl = 0;
    else { *e = "must be on or off"; return -1; }
    return 0;
}

void audio_register_params(void)
{
    CTRL_REG("audio.howl", p_howl, CTRL_PERSIST);
    CTRL_REG("audio.mic_gain", p_micgain, CTRL_PERSIST);
    CTRL_REG("audio.mute", p_mute, CTRL_PERSIST);
    CTRL_REG("audio.mute_on_play", p_mop, CTRL_PERSIST);
    CTRL_REG("audio.agc", p_agc, CTRL_PERSIST);
    CTRL_REG("audio.gate_db", p_gate, CTRL_PERSIST);
    aec_register_params();               /* audio.aec, aec_* (aec.c) */
    CTRL_REG("audio.state", p_astate, 0);
}

#define AUDIO_RING_SLOTS 8

struct audio_slot {
    int16_t samples[AUDIO_PERIOD_SAMPLES];
    size_t  n_samples;
};

static struct audio_slot g_ring[AUDIO_RING_SLOTS];
static volatile uint32_t g_ring_write = 0; /* total periods produced */
static volatile uint32_t g_ring_read  = 0; /* total periods consumed */

/* Reports exactly which step failed (added after a real hardware run came
 * back with a bare "audio_init: config ioctl: Not a tty" (ENOTTY) and no
 * way to tell which of the 9 calls it was — turned out to be GET_CAPS
 * specifically). */
#define AUDIO_IOC(step_name, expr) \
    do { \
        if ((expr) < 0) { \
            fprintf(stderr, "audio_init: step '%s' failed: %s (errno=%d)\n", \
                    step_name, strerror(errno), errno); \
            goto fail; \
        } \
    } while (0)

/* Same as AUDIO_IOC but non-fatal — for calls confirmed to be pure
 * informational reads in the real capture (their result was never fed into
 * any later SET call, only logged). GET_CAPS (nr=0x81) failed with ENOTTY
 * on a real hardware test of a capture-only client — best-supported theory
 * is it needs some cross-stream precondition only met when anyka_ipc's
 * playback+capture+loopback streams are all open together, which a
 * capture-only client like this one has no reason to replicate. Applied to
 * the other pure-GET informational
 * calls too on the same reasoning, since they were never load-bearing. */
#define AUDIO_IOC_WARN(step_name, expr) \
    do { \
        if ((expr) < 0) { \
            fprintf(stderr, "audio_init: step '%s' failed (non-fatal): %s (errno=%d)\n", \
                    step_name, strerror(errno), errno); \
        } \
    } while (0)

/* Hard-capped retry count for both the capture and playback streaming loops,
 * added 2026-07-04 after a real hardware test: with read() returning EPERM
 * immediately (not blocking), the previous unbounded "retry every 10ms
 * forever" loop hammered the driver at ~100Hz with no backoff, and the
 * camera hard-rebooted partway through that run. A capture/playback loop must never retry
 * a failing syscall indefinitely at that rate — give up after a handful of
 * consecutive failures and stop cleanly instead. */
#define AUDIO_MAX_CONSECUTIVE_FAILS 5

/* Configures the playback fd matching the real, path-tagged ground-truth
 * order: SET_STATE(0) -> SET_MODE(0) ->
 * SET_RATE(8000) -> SET_PERIODS(4) -> SET_STATE(4). SET_MODE(0) is skipped
 * below — confirmed to still crash the kernel even with the
 * O_WRONLY open fix, same signature every time. Root cause is
 * unresolved (unlike capture_ioctl's dispatch, playback_ioctl's real
 * dispatch hasn't been decompiled/read yet — that would be the next Ghidra
 * step if playback support is ever needed). Skipped for now since playback
 * isn't required for the actual goal (capture) and this crash was
 * repeatedly blocking us from observing capture's own read() result. All
 * steps here are non-fatal (AUDIO_IOC_WARN) — this is a best-effort peer
 * stream for capture's benefit, not something we depend on ourselves. */
static int audio_speaker_amp(int on)
{
    int32_t v = on ? 1 : 0;
    if (g_audio_playback_fd < 0) return -1;
    int r = ioctl(g_audio_playback_fd, AKPCM_IOC_SPK_ENABLE, &v);
    if (r < 0) fprintf(stderr, "[audio] speaker %s failed: %s\n", on ? "on" : "off", strerror(errno));
    return r;
}

static void audio_configure_playback(void)
{
    if (g_audio_playback_fd < 0) return;

    int32_t v;

    /* Commit the stream format first. This "GET_PARS"-named ioctl is really a
     * set (copy_from_user, see ak_ioctls.h); in ak_pcm.ko's playback_ioctl it
     * is the only path that vmallocs the playback buffer (+0x58) and sets the
     * ready bit (+0x304 |= 2) that playback_write() checks before anything
     * else (disassembly: playback_write @0x3050 / playback_ioctl @0x1e74,
     * @0x2108). Upstream never called it for playback, hence write() EPERM. */
    struct akpcm_pars pp;
    memset(&pp, 0, sizeof(pp));
    pp.format = 0; pp.rate = AUDIO_SAMPLE_RATE; pp.channels = AUDIO_CHANNELS;
    pp.bits = AUDIO_BITS; pp.period_size = AUDIO_PERIOD_BYTES; pp.period_count = 16;
    int committed = ioctl(g_audio_playback_fd, AKPCM_IOC_GET_PARS, &pp) == 0;
    if (!committed)
        fprintf(stderr, "audio_init: playback commit params failed: %s\n", strerror(errno));

    /* SET_MODE(0) is the actual "start": ak_pcm.ko's handler (@0x214c)
     * zeroes the playback buffer, sets the running bits and starts DMA
     * (snd_card_dma_xfer). Upstream's kernel crash on it was __memzero() on
     * the never-allocated buffer, i.e. it was called without the commit
     * above. Crash guard: a marker on the persistent CONFIG partition is
     * written before the call and removed after; if the camera rebooted in
     * between, the next start skips SET_MODE (no reboot loop possible). */
    const char *guard = "/etc/config/ak_rtsp_spk_guard";
    if (committed && access(guard, F_OK) != 0) {
        int gfd = open(guard, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (gfd >= 0) { fsync(gfd); close(gfd); sync(); }
        AUDIO_IOC_WARN("playback SET_MODE(0) start", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_MODE, 0));
        unlink(guard);
        sync();
    } else if (committed) {
        fprintf(stderr, "audio_init: SET_MODE skipped - previous attempt did not complete (crash guard %s)\n", guard);
    }

    v = 0;
    AUDIO_IOC_WARN("playback SET_STATE(0)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_STATE, &v));

    v = AUDIO_SAMPLE_RATE;
    AUDIO_IOC_WARN("playback SET_RATE", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_RATE, &v));

    v = 4;
    AUDIO_IOC_WARN("playback SET_PERIODS(4)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_PERIODS, &v));

    v = 4;
    AUDIO_IOC_WARN("playback SET_STATE(4)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_STATE, &v));

    /* Route the DAC to the speaker output, exactly as stock does before
     * enabling the speaker (get_source; if 0 -> set_source(4)). */
    int32_t src = -1;
    if (ioctl(g_audio_playback_fd, AKPCM_IOC_AO_GET_SOURCE, &src) == 0)
        printf("[audio] playback source was %d\n", (int)src);
    if (src != AKPCM_AO_SOURCE_DAC) {
        src = AKPCM_AO_SOURCE_DAC;
        AUDIO_IOC_WARN("playback set_source(4)", ioctl(g_audio_playback_fd, AKPCM_IOC_AO_SET_SOURCE, &src));
    }
    int32_t gain = -1;
    if (ioctl(g_audio_playback_fd, AKPCM_IOC_AO_GET_GAIN, &gain) == 0)
        printf("[audio] playback gain is %d\n", (int)gain);

    speaker_set_amp_fn(audio_speaker_amp);
    audio_speaker_amp(0);   /* off until something plays */

    printf("[ak_rtsp] audio: %s configured+started (%dHz/%dch/%dbit)\n",
           DEV_PCM_PLAYBACK, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);
}

/* Keeps the playback fd actively streaming for the duration of capture —
 * the real test of a theory:
 * whether capture's read() EPERM needs a sibling stream genuinely running,
 * not merely open/configured. Writes silence; audible output is a non-issue
 * since the speaker path is out of scope for this project. Same
 * retry-cap/backoff discipline as the capture loop. */
static void *audio_playback_loop(void *arg)
{
    (void)arg;

    int16_t silence[AUDIO_PERIOD_SAMPLES];

    int consecutive_fails = 0;
    while (g_playback_running) {
        speaker_fill(silence, AUDIO_PERIOD_SAMPLES);   /* queued sound, else silence */
        ssize_t n = write(g_audio_playback_fd, silence, sizeof(silence));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[audio] playback write");
            if (++consecutive_fails >= AUDIO_MAX_CONSECUTIVE_FAILS) {
                fprintf(stderr,
                        "[audio] playback write failed %d times in a row — "
                        "stopping playback stream thread\n", consecutive_fails);
                g_playback_running = 0;
                break;
            }
            usleep(200000);
            continue;
        }
        consecutive_fails = 0;
        if (!speaker_ready()) { speaker_set_ready(1); printf("[audio] speaker playback working\n"); }
    }
    speaker_set_ready(0);
    return NULL;
}

/* Config sequence replayed byte-for-byte from a real, path-tagged anyka_ipc
 * capture session — superseding an earlier
 * sequence, which had the wrong SET_STATE/SET_PERIODS values and was
 * missing RESET_BUF entirely, due to a since-corrected fd mislabeling bug in
 * the original capture method). Several ioctl semantics are still
 * unconfirmed (see ak_ioctls.h comments) — this deliberately replays the
 * exact same calls/values in the exact same order rather than trying to
 * "clean up" a sequence we don't fully understand yet.
 *
 * Split from playback config: the real
 * capture shows playback's SET_MODE(0) succeeds while capture is ALREADY
 * ACTIVELY READING (interleaved reads visible in the log between playback's
 * config ioctls), not merely after capture's config ioctls returned success.
 * An earlier "configure capture fully, then playback" reorder crashed on
 * SET_MODE again — this function now only opens+configures the capture fd;
 * the caller (audio_start()) starts the capture read thread before calling
 * audio_init_playback(), so playback configures while capture is genuinely
 * mid-stream, matching the real timing as closely as this code can. */
static int audio_init_capture(void)
{
    g_audio_fd = open(DEV_PCM_CAPTURE, O_RDONLY);
    if (g_audio_fd < 0) {
        perror("audio_init: open " DEV_PCM_CAPTURE);
        return -1;
    }

    int32_t v;
    struct akpcm_pars pars;

    /* Real order (ground truth): SET_STATE(2) -> GET_PERIODS -> GET_PARS
     * -> RESET_BUF(-1) -> SET_MODE(0) -> SET_RATE(8000) -> SET_STATE(2) again
     * -> SET_PERIODS(2). Note SET_STATE's value here is 2, not 0/4 as an
     * earlier attempt incorrectly assumed — replay the exact value, its
     * meaning is still unconfirmed. */
    v = 2;
    AUDIO_IOC("SET_STATE(2)", ioctl(g_audio_fd, AKPCM_IOC_SET_STATE, &v));

    AUDIO_IOC_WARN("GET_PERIODS#1", ioctl(g_audio_fd, AKPCM_IOC_GET_PERIODS, &v));

    /* GET_PARS is misnamed — a Ghidra relocation-fix session
     * decompiled ak_pcm.ko's real capture_ioctl dispatch and found this exact
     * ioctl (req=0x401c50e0) does a copy_FROM_user of this 28-byte struct into the
     * kernel, validates rate!=0, period_size!=0 && period_size%64==0, and
     * period_count in [1,0x50], computes the hardware rate divisor, and ONLY ON
     * SUCCESS sets the ready bit at +0x304 that capture_read() requires (bit 0x2).
     * Every previous session called this with a zeroed struct (treating
     * it as a pure informational GET) — that fails validation every time
     * (rate=0), which is exactly the observed EINVAL, and is why the ready bit
     * was never set regardless of what SET_MODE/SET_RATE/SET_STATE/SET_PERIODS
     * did. Fill it with the real captured values instead. */
    memset(&pars, 0, sizeof(pars));
    pars.format       = 0;
    pars.rate         = AUDIO_SAMPLE_RATE;
    pars.channels     = AUDIO_CHANNELS;
    pars.bits         = AUDIO_BITS;
    pars.period_size  = AUDIO_PERIOD_BYTES;
    pars.period_count = 16;
    AUDIO_IOC("GET_PARS (commit params)", ioctl(g_audio_fd, AKPCM_IOC_GET_PARS, &pars));

    /* dir=NONE — value goes directly in the ioctl request, not via pointer.
     * Real capture calls this with arg=-1, not NULL/0 (see ak_ioctls.h) —
     * previously entirely missing from this sequence, a leading candidate
     * for (part of) the actual EPERM fix. */
    AUDIO_IOC("RESET_BUF(-1)", ioctl(g_audio_fd, AKPCM_IOC_RESET_BUF, (void *)(long)-1));

    /* dir=NONE — value goes directly in the ioctl request, not via pointer */
    AUDIO_IOC("SET_MODE(0)", ioctl(g_audio_fd, AKPCM_IOC_SET_MODE, 0));

    v = AUDIO_SAMPLE_RATE;
    AUDIO_IOC("SET_RATE(8000)", ioctl(g_audio_fd, AKPCM_IOC_SET_RATE, &v));

    v = 2;
    AUDIO_IOC("SET_STATE(2)#2", ioctl(g_audio_fd, AKPCM_IOC_SET_STATE, &v));

    v = 2;
    AUDIO_IOC("SET_PERIODS(2)", ioctl(g_audio_fd, AKPCM_IOC_SET_PERIODS, &v));
    /* (that request is the capture GET_GAIN, see ak_ioctls.h) */
    printf("[audio] mic gain at open: %d (0..7)\n", (int)v);
    if (g_mic_gain_boot < 0 && v >= 0 && v <= 7) g_mic_gain_boot = v;
    mic_gain_apply();

    printf("[ak_rtsp] audio: %s opened, capture configured (%dHz/%dch/%dbit, "
           "period=%d samples)\n",
           DEV_PCM_CAPTURE, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS,
           AUDIO_PERIOD_SAMPLES);
    return 0;

fail:
    close(g_audio_fd);
    g_audio_fd = -1;
    return -1;
}

/* Opens+configures playback/loopback — called AFTER the capture read thread
 * is already running (see audio_start() and the comment above
 * audio_init_capture()). Best-effort: not fatal if these fail, capture is
 * already working by this point regardless. O_WRONLY for playback matches
 * real anyka_ipc's own open() flags — was O_RDWR in every earlier
 * session. */
static void audio_init_playback(void)
{
    /* Test mode: leave the speaker device free for the aoprobe tool. */
    if (access("/tmp/sd/maint/no_playback", F_OK) == 0) {
        printf("[audio] no_playback flag: not opening %s\n", DEV_PCM_PLAYBACK);
        g_audio_playback_fd = -1;
    } else
    g_audio_playback_fd = open(DEV_PCM_PLAYBACK, O_WRONLY);
    if (g_audio_playback_fd < 0)
        fprintf(stderr, "audio_init: open %s failed (non-fatal): %s\n",
                DEV_PCM_PLAYBACK, strerror(errno));
    else
        audio_configure_playback();
    g_audio_loopback_fd = open(DEV_PCM_LOOPBACK, O_RDWR);
    if (g_audio_loopback_fd < 0)
        fprintf(stderr, "audio_init: open %s failed (non-fatal): %s\n",
                DEV_PCM_LOOPBACK, strerror(errno));
    printf("[ak_rtsp] audio: playback_fd=%d (configured+started) loopback_fd=%d "
           "(held open, unconfigured)\n",
           g_audio_playback_fd, g_audio_loopback_fd);
}

static void audio_close_aux(void)
{
    if (g_audio_playback_fd >= 0) { close(g_audio_playback_fd); g_audio_playback_fd = -1; }
    if (g_audio_loopback_fd >= 0) { close(g_audio_loopback_fd); g_audio_loopback_fd = -1; }
}

static void audio_close(void)
{
    if (g_audio_fd >= 0) {
        close(g_audio_fd);
        g_audio_fd = -1;
    }
    audio_close_aux();
}

static void *audio_capture_loop(void *arg)
{
    (void)arg;

    int consecutive_fails = 0;
    while (g_audio_running) {
        struct audio_slot *slot = &g_ring[g_ring_write % AUDIO_RING_SLOTS];
        ssize_t n = read(g_audio_fd, slot->samples, sizeof(slot->samples));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[audio] read");
            if (++consecutive_fails >= AUDIO_MAX_CONSECUTIVE_FAILS) {
                fprintf(stderr,
                        "[audio] read failed %d times in a row — stopping capture "
                        "thread (not retrying further)\n",
                        consecutive_fails);
                g_audio_running = 0; /* so audio_get_frame() reports "not running" immediately */
                break;
            }
            usleep(200000); /* 200ms backoff, not 10ms — this is a slow retry, not a poll */
            continue;
        }
        consecutive_fails = 0;
        if (n == 0) {
            usleep(10000);
            continue;
        }
        slot->n_samples = (size_t)n / sizeof(int16_t);
        aec_process(slot->samples, slot->n_samples, mono_us());   /* echo cancellation (audio.aec) */
        mic_process(slot->samples, slot->n_samples);   /* mute / noise gate / AGC */
        g_ring_write++;
        live_push_audio(slot->samples, slot->n_samples);   /* web live view copy */
        sounddet_feed(slot->samples, slot->n_samples);     /* loud-sound events */
    }
    return NULL;
}

/* Tracks whether pthread_create succeeded, independent of g_audio_running
 * (which the capture thread itself may clear on a fatal read() streak — see
 * audio_capture_loop). Needed so audio_stop() always joins a thread that was
 * actually started, even if it already exited on its own. */
static volatile int g_audio_thread_started = 0;

int audio_start(void)
{
    if (audio_init_capture() < 0) return -1;

    g_ring_write = 0;
    g_ring_read  = 0;
    g_audio_running = 1;

    if (pthread_create(&g_audio_tid, NULL, audio_capture_loop, NULL) != 0) {
        perror("[audio] pthread_create");
        g_audio_running = 0;
        audio_close();
        return -1;
    }
    g_audio_thread_started = 1;

    /* Deliberately started only now — see the comment above
     * audio_init_capture(): real anyka_ipc's playback SET_MODE succeeds
     * while capture is already mid-read, not merely after capture's config
     * ioctls returned success. */
    audio_init_playback();

    aec_start();                               /* needs the loopback fd opened above */

    if (g_audio_playback_fd >= 0) {
        g_playback_running = 1;
        if (pthread_create(&g_playback_tid, NULL, audio_playback_loop, NULL) != 0) {
            perror("[audio] playback pthread_create");
            g_playback_running = 0;
        } else {
            g_playback_thread_started = 1;
        }
    }

    return 0;
}

void audio_stop(void)
{
    g_playback_running = 0;
    if (g_playback_thread_started) {
        pthread_join(g_playback_tid, NULL);
        g_playback_thread_started = 0;
    }

    g_audio_running = 0;
    if (g_audio_thread_started) {
        pthread_join(g_audio_tid, NULL);
        g_audio_thread_started = 0;
    }
    g_aec_running = 0;
    if (g_aec_thread_started) {
        pthread_join(g_aec_tid, NULL);
        g_aec_thread_started = 0;
    }
    audio_close();
}

int audio_get_frame(int16_t *buf, size_t max_samples, size_t *out_samples)
{
    if (!g_audio_running) return -1;
    if (g_ring_read >= g_ring_write) return 0;

    /* Producer lapped us — drop the oldest slots we can no longer trust. */
    if (g_ring_write - g_ring_read > AUDIO_RING_SLOTS)
        g_ring_read = g_ring_write - AUDIO_RING_SLOTS;

    struct audio_slot *slot = &g_ring[g_ring_read % AUDIO_RING_SLOTS];
    size_t n = slot->n_samples;
    if (n > max_samples) n = max_samples;
    memcpy(buf, slot->samples, n * sizeof(int16_t));
    *out_samples = n;
    g_ring_read++;
    return 1;
}
