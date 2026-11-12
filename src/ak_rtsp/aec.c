/* Acoustic echo cancellation (audio.aec) — see aec.h. */
#include "aec.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#ifndef AEC_HOST_TEST
#include "control.h"
#include "ctrl_params.h"
#include "monotime.h"
#else
int64_t mono_us(void);
#endif

#define FS        8000
#define FRAME     128                 /* SpeexDSP block: 16 ms */
#define REF_RING  16384               /* reference history: 2 s */
#define MIC_RING  4096                /* raw / processed microphone history */
#define WIN       64                  /* timing estimates kept per stream */
#define OFF_JUMP  24                  /* samples: a real re-alignment, not jitter */
#define OFF_CONFIRM 10                /* consecutive frames before re-aligning */
#define AEC_WAIT  2400                /* a block waits at most 300 ms for its reference */

/* ---- settings ---- */
static int s_aec;                      /* audio.aec */
static int s_delay_ms;                 /* audio.aec_delay: extra reference lag */
static int s_tail_ms = 128;            /* audio.aec_tail: echo tail length */
static int s_suppress = 40;            /* audio.aec_suppress: residual echo, dB */
static int s_ns = 10;                  /* audio.aec_ns: noise suppression, dB */
static int s_scale = -1;               /* audio.aec_scale: echo path gain to undo, dB; -1 = auto */
static int s_nlp = 40;                 /* audio.aec_nlp: echo ducking depth, dB (0 = off) */
static int g_auto_scale = 36;          /* auto: measured on this camera (32..37 dB at mic gain 0..3) */
static int g_mic_shift = 3, g_ref_shift = 3;   /* from s_scale, applied at (re)build */
static volatile int g_reinit = 1;      /* capture thread rebuilds the Speex states */
static volatile int g_rebuild;         /* same, from the auto scale */

/* ---- reference (written by the loopback thread) ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int16_t g_ref[REF_RING];
static int64_t g_R;                    /* reference samples written so far */
static int64_t g_ra[WIN];              /* R_end - t: larger = read with less delay */
static int     g_ra_n, g_ra_i;
static int64_t g_Ar;                   /* max of g_ra */
static int64_t g_ref_last_us;
static int     g_ref_alive;
static unsigned g_ref_gaps;
static volatile int g_ref_attached;    /* loopback reader started (boot only) */

/* ---- microphone side (capture thread only) ---- */
static int16_t g_mic[MIC_RING];
static int16_t g_out[MIC_RING];
static int64_t g_M;                    /* microphone samples received (+AEC_HOLD) */
static int64_t g_P;                    /* next sample to process */
static int64_t g_ma[WIN];
static int     g_ma_n, g_ma_i;
static int64_t g_off;                  /* reference index - microphone index */
static int     g_off_set, g_off_pending;
static SpeexEchoState       *g_echo;
static SpeexPreprocessState *g_pp;
static int     g_running;              /* states built, delay line primed */
static int     g_active;               /* last frame ran on a live reference */

/* ---- statistics (written by the capture thread, read anywhere) ---- */
static unsigned g_late, g_frames, g_realign, g_concealed;
static double   g_e_mic, g_e_out, g_e_ref;   /* smoothed energies while the reference plays */
static int      g_peak_ms = -1;              /* echo path peak inside the filter */
static int      g_peak_ratio;

/* ---- dump ---- */
static int16_t *g_dump;
static size_t   g_dump_cap, g_dump_pos;      /* in frames of 3 samples */
static volatile int g_dump_on;
static int16_t *g_raw_ref, *g_raw_mic;       /* aec_dump_raw(), under g_mu */
static size_t g_raw_ref_n, g_raw_mic_n, g_raw_ref_cap, g_raw_mic_cap;
static int g_raw_on;

static void raw_take(int16_t *dst, size_t *pos, size_t cap, const int16_t *s, size_t n)
{
    for (size_t i = 0; i < n && *pos < cap; i++) dst[(*pos)++] = s[i];
}

static int64_t tpos(int64_t t_us) { return t_us / 125; }     /* microseconds -> 8 kHz samples */

static int64_t win_push(int64_t *w, int *n, int *i, int64_t v)
{
    w[*i] = v;
    *i = (*i + 1) % WIN;
    if (*n < WIN) (*n)++;
    int64_t m = w[0];
    for (int k = 1; k < *n; k++) if (w[k] > m) m = w[k];
    return m;
}

void aec_ref_push(const int16_t *s, size_t n, int64_t t_us)
{
    if (!n) return;
    int64_t tp = tpos(t_us);
    pthread_mutex_lock(&g_mu);
    if (g_raw_on) raw_take(g_raw_ref, &g_raw_ref_n, g_raw_ref_cap, s, n);
    for (size_t k = 0; k < n; k++) g_ref[(g_R + (int64_t)k) & (REF_RING - 1)] = s[k];
    g_R += (int64_t)n;
    g_Ar = win_push(g_ra, &g_ra_n, &g_ra_i, g_R - tp);
    /* A late read is not lost data: the driver buffers it and the next
     * reads catch up. Samples really lost (overrun) leave every later read
     * short by the same amount: if even the best of the last 8 reads is
     * >50 ms behind, insert that much silence so the timeline (and the
     * microphone alignment) stays continuous. */
    if (g_ra_n >= WIN / 2) {
        int64_t recent = g_ra[(g_ra_i + WIN - 1) % WIN];
        for (int k = 2; k <= 8; k++) {
            int64_t v = g_ra[(g_ra_i + WIN - k) % WIN];
            if (v > recent) recent = v;
        }
        int64_t missing = g_Ar - recent;
        if (missing > 400) {
            int64_t fill = missing > REF_RING / 2 ? REF_RING / 2 : missing;
            int16_t keep[8192];
            size_t kn = n > 8192 ? 8192 : n;
            for (size_t k = 0; k < kn; k++) keep[k] = g_ref[(g_R - (int64_t)kn + (int64_t)k) & (REF_RING - 1)];
            g_R -= (int64_t)kn;
            for (int64_t k = 0; k < fill; k++) g_ref[(g_R + k) & (REF_RING - 1)] = 0;
            g_R += fill;
            for (size_t k = 0; k < kn; k++) g_ref[(g_R + (int64_t)k) & (REF_RING - 1)] = keep[k];
            g_R += (int64_t)kn;
            for (int k = 1; k <= 8; k++) g_ra[(g_ra_i + WIN - k) % WIN] += fill;
            g_ref_gaps++;
        }
    }
    g_ref_last_us = t_us;
    g_ref_alive = 1;
    pthread_mutex_unlock(&g_mu);
}

void aec_ref_attach(void) { g_ref_attached = 1; }

void aec_ref_lost(void)
{
    pthread_mutex_lock(&g_mu);
    g_ref_alive = 0;
    pthread_mutex_unlock(&g_mu);
}

static void states_free(void)
{
    if (g_pp) speex_preprocess_state_destroy(g_pp);
    if (g_echo) speex_echo_state_destroy(g_echo);
    g_pp = NULL; g_echo = NULL;
}

static int states_build(void)
{
    states_free();
    int sc = s_scale >= 0 ? s_scale : g_auto_scale;
    int steps = (sc + 3) / 6;                    /* 6 dB per shift */
    g_mic_shift = steps - 3 > 1 ? steps - 3 : 1; /* >= 1: headroom */
    g_ref_shift = steps - g_mic_shift;
    if (g_ref_shift < 0) g_ref_shift = 0;
    if (g_ref_shift > 3) g_ref_shift = 3;
    int tail = s_tail_ms * FS / 1000;
    tail = (tail + FRAME - 1) / FRAME * FRAME;
    g_echo = speex_echo_state_init(FRAME, tail);
    g_pp = speex_preprocess_state_init(FRAME, FS);
    if (!g_echo || !g_pp) { states_free(); return -1; }
    int rate = FS, on = 1, off = 0;
    speex_echo_ctl(g_echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    speex_preprocess_ctl(g_pp, SPEEX_PREPROCESS_SET_ECHO_STATE, g_echo);
    /* the echo suppressor runs inside the denoiser's gain stage, so the
     * denoiser must be on; its own strength is audio.aec_ns */
    speex_preprocess_ctl(g_pp, SPEEX_PREPROCESS_SET_DENOISE, &on);
    speex_preprocess_ctl(g_pp, SPEEX_PREPROCESS_SET_DEREVERB, &off);
    int ns = -s_ns, es = -s_suppress, ea = -(s_suppress * 3 / 8);
    speex_preprocess_ctl(g_pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &ns);
    speex_preprocess_ctl(g_pp, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &es);
    speex_preprocess_ctl(g_pp, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &ea);
    return 0;
}

static void delay_line_reset(void)
{
    memset(g_mic, 0, sizeof g_mic);
    memset(g_out, 0, sizeof g_out);
    g_M = AEC_HOLD;                        /* AEC_HOLD samples of silence are "already in" */
    g_P = 0;
    g_ma_n = g_ma_i = 0;
    g_off_set = 0; g_off_pending = 0;
}

/* Where the filter's energy sits: tells whether the delay setting leaves
 * the echo path inside the tail (and how much margin there is). */
static void measure_peak(void)
{
    spx_int32_t len = 0;
    speex_echo_ctl(g_echo, SPEEX_ECHO_GET_IMPULSE_RESPONSE_SIZE, &len);
    if (len <= 0 || len > 8192) return;
    spx_int32_t *h = malloc((size_t)len * sizeof *h);
    if (!h) return;
    speex_echo_ctl(g_echo, SPEEX_ECHO_GET_IMPULSE_RESPONSE, h);
    int64_t best = 0, sum = 0; int at = 0;
    for (int i = 0; i < len; i++) {
        int64_t a = h[i] < 0 ? -(int64_t)h[i] : h[i];
        sum += a;
        if (a > best) { best = a; at = i; }
    }
    free(h);
    g_peak_ms = at * 1000 / FS;
    g_peak_ratio = sum ? (int)(best * len / sum) : 0;    /* peak / mean: >10 = a clear path */
}

/* audio.aec_scale auto: follow the measured echo path (it moves ~1.8 dB
 * per mic gain step). Measured over >= 5 s of frames where the speaker
 * clearly plays (reference rms > 100) and the mic is not clipping, and
 * changed only for a mismatch over 9 dB: a change rebuilds the filter.
 * Rounded up: over-scaling costs little, under-scaling breaks the filter. */
static double g_path_m, g_path_r;      /* energy sums over those frames */
static int    g_path_n;
static double g_path_db;               /* last completed measurement */

static void path_frame(const int16_t *in, const int16_t *ref)
{
    int64_t em = 0, er = 0; int clip = 0;
    for (int i = 0; i < FRAME; i++) {
        em += (int32_t)in[i] * in[i];
        er += (int32_t)ref[i] * ref[i];
        if (in[i] >= 32700 || in[i] <= -32700) clip = 1;
    }
    /* only well below clipping: near full scale the mic preamp compresses
     * and the path would read low (seen: 14 dB instead of 30 at volume 85) */
    if (clip || er < (int64_t)100 * 100 * FRAME || em > (int64_t)3277 * 3277 * FRAME) return;
    g_path_m += (double)em; g_path_r += (double)er;
    if (++g_path_n < 312) return;                     /* 5 s of active frames */
    g_path_db = 10 * log10(g_path_m / g_path_r);
    g_path_m = g_path_r = 0; g_path_n = 0;
    if (s_scale >= 0 || (g_path_db - g_auto_scale <= 9 && g_auto_scale - g_path_db <= 9)) return;
    int want = ((int)ceil(g_path_db) + 5) / 6 * 6;
    if (want < 6) want = 6;
    if (want > 48) want = 48;
    if (want == g_auto_scale) return;
    printf("[aec] echo path %.1f dB: scale %d -> %d dB\n", g_path_db, g_auto_scale, want);
    g_auto_scale = want;
    g_rebuild = 1;
}

/* Echo ducking (audio.aec_nlp, dB of attenuation, 0 = off). At high volume
 * the camera's mic is saturated by its own speaker (-7 dBFS at volume 85)
 * and the tiny speaker distorts: no canceller removes that, and the
 * residual keeps a phone-camera loop going. So while the played audio
 * carries voice the outgoing mic is ducked, and it reopens ~200 ms after
 * (the echo tail), so the camera listens again as soon as the phone user
 * stops talking. Keyed on voice activity in the reference, 10 dB over its
 * own noise floor: a phone with an open mic streams room noise all the
 * time, which must not keep the camera deaf. Exact timing: the reference is
 * aligned with the mic, the echo arrives ~24 ms after it. */
static double g_duck_floor = 1e12;     /* reference noise floor (energy per frame) */
static int    g_duck_hold;             /* frames left before reopening */
static float  g_duck_gain = 1.0f;
static unsigned g_duck_frames;         /* frames ducked (status) */

#define DUCK_HOLD 12                   /* 192 ms: echo delay + 128 ms tail + margin */

static void nlp(const int16_t *in, const int16_t *ref, int16_t *out)
{
    int64_t er = 0;
    int clip = 0;
    for (int i = 0; i < FRAME; i++) {
        er += (int32_t)ref[i] * ref[i];
        if (in[i] >= 32000 || in[i] <= -32000) clip = 1;
    }
    double r = (double)er + 4.0 * FRAME;
    if (r < g_duck_floor) g_duck_floor = 0.5 * g_duck_floor + 0.5 * r;   /* follow down fast */
    else g_duck_floor *= 1.005;                                          /* ~1.3 dB/s up */
    if (g_duck_floor > 100.0 * 100.0 * FRAME) g_duck_floor = 100.0 * 100.0 * FRAME;   /* a howl must not become "floor" */
    int voice = (r > 10.0 * g_duck_floor && r > 30.0 * 30.0 * FRAME)   /* >10 dB over floor, > -60 dBFS */
             || r > 150.0 * 150.0 * FRAME;                              /* or simply loud: > -47 dBFS */
    /* a mic clipping during playback is saturated by the echo */
    if (voice || (clip && r > 30.0 * 30.0 * FRAME)) g_duck_hold = DUCK_HOLD;
    float target = 1.0f;
    if (s_nlp > 0 && g_duck_hold > 0) {
        g_duck_hold--;
        target = powf(10.0f, -s_nlp / 20.0f);
        g_duck_frames++;
    }
    float g0 = g_duck_gain;
    /* close within one frame (the echo is ~24 ms behind the reference),
     * reopen over ~3 frames: no clicks */
    float g1 = target < g0 ? target : g0 + (target - g0) * 0.4f;
    if (g1 > 0.98f) g1 = 1.0f;
    for (int i = 0; i < FRAME; i++) {
        float g = g0 + (g1 - g0) * (float)i / FRAME;
        out[i] = (int16_t)(out[i] * g);
    }
    g_duck_gain = g1;
}

static void energy(const int16_t *s, double *acc)
{
    int64_t e = 0;                         /* integer: no soft-float per sample */
    for (int i = 0; i < FRAME; i++) e += (int32_t)s[i] * s[i];
    *acc = 0.95 * *acc + 0.05 * (double)e;
}

int aec_process(int16_t *mic, size_t n, int64_t t_us)
{
    if (!s_aec) {
        if (g_running) { g_running = 0; g_active = 0; states_free(); }
        return 0;
    }
    if (n > MIC_RING / 4) return 0;
    if (!g_running || g_reinit || g_rebuild) {
        /* settings changes rebuild only the SpeexDSP states: the delay line
         * and the stream alignment stay (re-estimating them from a short
         * window would shift the reference by a few samples) */
        if (states_build() < 0) { fprintf(stderr, "[aec] out of memory\n"); s_aec = 0; return 0; }
        if (!g_running) {
            delay_line_reset();
            printf("[aec] speexdsp canceller started (tail %d ms, hold %d ms)\n", s_tail_ms, AEC_HOLD * 1000 / FS);
        } else printf("[aec] canceller rebuilt (scale %d dB)\n", 6 * (g_mic_shift + g_ref_shift));
        g_reinit = g_rebuild = 0;
        g_running = 1;
    }

    for (size_t i = 0; i < n; i++) g_mic[(g_M + (int64_t)i) & (MIC_RING - 1)] = mic[i];
    g_M += (int64_t)n;
    int64_t Am = win_push(g_ma, &g_ma_n, &g_ma_i, g_M - tpos(t_us));

    pthread_mutex_lock(&g_mu);
    if (g_raw_on) raw_take(g_raw_mic, &g_raw_mic_n, g_raw_mic_cap, mic, n);
    int live = g_ref_alive && g_ra_n >= 8 && t_us - g_ref_last_us < 500000;
    int64_t Ar = g_Ar;
    pthread_mutex_unlock(&g_mu);
    live = live && g_ma_n >= 8;

    if (live) {
        int64_t off = Ar - Am;
        if (!g_off_set) { g_off = off; g_off_set = 1; g_off_pending = 0; }
        else if (off - g_off > OFF_JUMP || g_off - off > OFF_JUMP) {
            if (++g_off_pending >= OFF_CONFIRM) {
                printf("[aec] re-aligned: %lld -> %lld samples\n", (long long)g_off, (long long)off);
                g_off = off; g_off_pending = 0; g_realign++;
            }
        } else g_off_pending = 0;
    }

    /* Emit [T - n, T) now. Blocks are processed as soon as their reference
     * has arrived (possibly ahead of T). A late loopback read (scheduling
     * stall) makes a block wait, up to AEC_WAIT behind T: the samples
     * emitted meanwhile are silence, and the block is still processed once
     * its reference comes, so SpeexDSP never sees a hole in the reference
     * history (a hole would corrupt the next tail's worth of prediction). */
    int64_t T = g_M - AEC_HOLD;
    int64_t lag = (int64_t)s_delay_ms * FS / 1000;
    int ran = 0;
    while (g_P + FRAME <= g_M) {
        int16_t in[FRAME], ref[FRAME], out[FRAME];
        if (live) {
            int64_t r0 = g_P + g_off - lag;
            int missing = 0;
            pthread_mutex_lock(&g_mu);
            int64_t R = g_R;
            if (r0 + FRAME > R && T - g_P < AEC_WAIT) { pthread_mutex_unlock(&g_mu); break; }
            for (int i = 0; i < FRAME; i++) {
                int64_t r = r0 + i;
                if (r < R && r >= R - REF_RING + 1024) ref[i] = g_ref[r & (REF_RING - 1)];
                else { ref[i] = 0; missing++; }
            }
            pthread_mutex_unlock(&g_mu);
            if (missing) g_late++;
            /* Scale so the filter sees an echo path near unity gain: the
             * fixed-point MDF cannot model a path much above 1 (simulated:
             * 41 dB at gain 1, <1 dB at gain 8), and on this camera the
             * echo is 32..37 dB above the loopback (tiny speaker, high amp
             * gain; see audio.aec_scale). The mic also needs headroom (the
             * filter overflows above ~4000 rms). */
            int ms = g_mic_shift, rs = g_ref_shift;
            int16_t x[FRAME], rx[FRAME];
            for (int i = 0; i < FRAME; i++) {
                in[i] = g_mic[(g_P + i) & (MIC_RING - 1)];
                x[i] = (int16_t)(in[i] >> ms);
                int v = ref[i] * (1 << rs);
                rx[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            speex_echo_cancellation(g_echo, x, rx, out);
#ifdef AEC_HOST_TEST
            if (!getenv("AEC_NOPP"))
#endif
            speex_preprocess_run(g_pp, out);
            for (int i = 0; i < FRAME; i++) {
                int v = out[i] * (1 << ms);
                out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            nlp(in, ref, out);
            energy(in, &g_e_mic); energy(out, &g_e_out); energy(ref, &g_e_ref);
            path_frame(in, ref);
            if (++g_frames % 125 == 0) measure_peak();     /* every 2 s */
            ran = 1;
        } else {
            if (g_P >= T) break;                          /* pass-through: no look-ahead needed */
            for (int i = 0; i < FRAME; i++) in[i] = g_mic[(g_P + i) & (MIC_RING - 1)];
            memcpy(out, in, sizeof out);
            memset(ref, 0, sizeof ref);
        }
        if (g_dump_on) {
            for (int i = 0; i < FRAME && g_dump_pos < g_dump_cap; i++, g_dump_pos++) {
                g_dump[g_dump_pos * 3] = in[i];
                g_dump[g_dump_pos * 3 + 1] = ref[i];
                g_dump[g_dump_pos * 3 + 2] = out[i];
            }
            if (g_dump_pos >= g_dump_cap) g_dump_on = 0;
        }
        for (int i = 0; i < FRAME; i++) g_out[(g_P + i) & (MIC_RING - 1)] = out[i];
        g_P += FRAME;
    }
    int hole = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t k = T - (int64_t)n + (int64_t)i;
        if (k < g_P) mic[i] = g_out[k & (MIC_RING - 1)];
        else { mic[i] = 0; hole = 1; }                    /* still waiting for its reference */
    }
    if (hole) g_concealed++;
    if (ran) g_active = 1;
    else if (!live) g_active = 0;
    return 1;
}

int aec_active(void) { return s_aec && g_active; }
int aec_enabled(void) { return s_aec; }

void *aec_dump(int secs, size_t *len)
{
    if (secs < 1) secs = 1;
    if (secs > 20) secs = 20;
    if (!s_aec || g_dump_on) return NULL;
    size_t cap = (size_t)secs * FS;
    int16_t *b = calloc(cap * 3, sizeof(int16_t));
    if (!b) return NULL;
    g_dump = b; g_dump_cap = cap; g_dump_pos = 0;
    __asm__ volatile("" ::: "memory");          /* single core: a compiler barrier is enough */
    g_dump_on = 1;
    for (int i = 0; i < secs * 20 + 40 && g_dump_on; i++) usleep(50000);
    g_dump_on = 0;
    __asm__ volatile("" ::: "memory");          /* single core: a compiler barrier is enough */
    *len = g_dump_pos * 3 * sizeof(int16_t);
    g_dump = NULL;
    return b;
}

/* Debug: the loopback exactly as read, and the raw microphone, over the
 * same `secs`: u32 ref_count, u32 mic_count, then the s16 samples of each. */
void *aec_dump_raw(int secs, size_t *len)
{
    if (secs < 1) secs = 1;
    if (secs > 10) secs = 10;
    if (g_raw_on) return NULL;
    size_t rc = (size_t)secs * FS * 4, mc = (size_t)secs * FS;
    uint8_t *b = calloc(8 + (rc + mc) * sizeof(int16_t), 1);
    if (!b) return NULL;
    pthread_mutex_lock(&g_mu);
    g_raw_ref = (int16_t *)(b + 8); g_raw_mic = g_raw_ref + rc;
    g_raw_ref_cap = rc; g_raw_mic_cap = mc; g_raw_ref_n = g_raw_mic_n = 0;
    g_raw_on = 1;
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < secs * 20 + 20 && g_raw_mic_n < mc; i++) usleep(50000);
    pthread_mutex_lock(&g_mu);
    g_raw_on = 0;
    uint32_t nr = (uint32_t)g_raw_ref_n, nm = (uint32_t)g_raw_mic_n;
    pthread_mutex_unlock(&g_mu);
    memcpy(b, &nr, 4); memcpy(b + 4, &nm, 4);
    memmove(b + 8 + nr * sizeof(int16_t), g_raw_mic, nm * sizeof(int16_t));
    *len = 8 + (nr + nm) * sizeof(int16_t);
    return b;
}

int aec_status_json(char *out, size_t cap)
{
    double erle = (g_e_out > 1 && g_e_ref > 1e4 * FRAME) ? 10.0 * log10(g_e_mic / g_e_out) : 0;
    pthread_mutex_lock(&g_mu);
    int alive = g_ref_alive; unsigned gaps = g_ref_gaps;
    pthread_mutex_unlock(&g_mu);
    return snprintf(out, cap,
        "{\"on\":%d,\"active\":%d,\"ref\":%d,\"offset\":%lld,\"late\":%u,\"concealed\":%u,\"gaps\":%u,"
        "\"realign\":%u,\"frames\":%u,\"erle_db\":%.1f,\"ref_db\":%.1f,\"mic_db\":%.1f,"
        "\"out_db\":%.1f,\"peak_ms\":%d,\"peak_ratio\":%d,\"path_db\":%.1f,\"scale_db\":%d,\"ducked_frames\":%u,\"ref_floor_db\":%.1f}",
        s_aec, g_active, alive, (long long)g_off, g_late, g_concealed, gaps, g_realign, g_frames, erle,
        10 * log10(g_e_ref / FRAME + 1) - 90.3, 10 * log10(g_e_mic / FRAME + 1) - 90.3,
        10 * log10(g_e_out / FRAME + 1) - 90.3, g_peak_ms, g_peak_ratio,
        g_path_db, 6 * (g_mic_shift + g_ref_shift), g_duck_frames, 10 * log10(g_duck_floor / FRAME + 1) - 90.3);
}

/* ---- settings ---- */
#ifndef AEC_HOST_TEST
static int parse_int(const char *v, int lo, int hi, int *out)
{
    char *end; long x = strtol(v, &end, 10);
    if (!*v || *end || x < lo || x > hi) return -1;
    *out = (int)x;
    return 0;
}

static int p_aec_get(char *o, size_t c) { snprintf(o, c, "%s", s_aec ? "on" : "off"); return 0; }
static int p_aec_set(const char *v, const char **e)
{
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "starting")) s_aec = 1;
    else if (!strcmp(v, "off") || !strcmp(v, "0")) s_aec = 0;
    else { *e = "must be on or off"; return -1; }
    return 0;
}
static int p_state_get(char *o, size_t c)
{
    int alive;
    pthread_mutex_lock(&g_mu); alive = g_ref_alive; pthread_mutex_unlock(&g_mu);
    /* the loopback is only opened at boot when audio.aec is saved on */
    snprintf(o, c, "%s", !s_aec ? "off" : !g_ref_attached ? "reboot needed" :
             g_active ? "active" : alive ? "ready" : "no reference");
    return 0;
}
static int p_ro_set(const char *v, const char **e) { (void)v; *e = "read-only"; return -1; }
static int p_delay_get(char *o, size_t c) { snprintf(o, c, "%d", s_delay_ms); return 0; }
static int p_delay_set(const char *v, const char **e)
{
    if (parse_int(v, -100, 400, &s_delay_ms)) { *e = "must be -100..400 ms"; return -1; }
    return 0;
}
static int p_tail_get(char *o, size_t c) { snprintf(o, c, "%d", s_tail_ms); return 0; }
static int p_tail_set(const char *v, const char **e)
{
    if (parse_int(v, 32, 512, &s_tail_ms)) { *e = "must be 32..512 ms"; return -1; }
    g_reinit = 1;
    return 0;
}
static int p_sup_get(char *o, size_t c) { snprintf(o, c, "%d", s_suppress); return 0; }
static int p_sup_set(const char *v, const char **e)
{
    if (parse_int(v, 0, 60, &s_suppress)) { *e = "must be 0..60 dB"; return -1; }
    g_reinit = 1;
    return 0;
}
static int p_ns_get(char *o, size_t c) { snprintf(o, c, "%d", s_ns); return 0; }
static int p_ns_set(const char *v, const char **e)
{
    if (parse_int(v, 0, 30, &s_ns)) { *e = "must be 0..30 dB"; return -1; }
    g_reinit = 1;
    return 0;
}
static int p_scale_get(char *o, size_t c)
{
    if (s_scale < 0) snprintf(o, c, "auto"); else snprintf(o, c, "%d", s_scale);
    return 0;
}
static int p_scale_set(const char *v, const char **e)
{
    if (!strcmp(v, "auto")) s_scale = -1;
    else if (parse_int(v, 0, 48, &s_scale)) { *e = "must be auto or 0..48 dB"; return -1; }
    g_reinit = 1;
    return 0;
}
static int p_nlp_get(char *o, size_t c) { snprintf(o, c, "%d", s_nlp); return 0; }
static int p_nlp_set(const char *v, const char **e)
{
    if (parse_int(v, 0, 60, &s_nlp)) { *e = "must be 0..60 dB (0 = off)"; return -1; }
    return 0;
}
#define p_aecstate_get p_state_get
#define p_aecstate_set p_ro_set

void aec_register_params(void)
{
    CTRL_REG("audio.aec", p_aec, CTRL_PERSIST);
    CTRL_REG("audio.aec_state", p_aecstate, 0);
    CTRL_REG("audio.aec_delay", p_delay, CTRL_PERSIST);
    CTRL_REG("audio.aec_tail", p_tail, CTRL_PERSIST);
    CTRL_REG("audio.aec_suppress", p_sup, CTRL_PERSIST);
    CTRL_REG("audio.aec_ns", p_ns, CTRL_PERSIST);
    CTRL_REG("audio.aec_scale", p_scale, CTRL_PERSIST);
    CTRL_REG("audio.aec_nlp", p_nlp, CTRL_PERSIST);
}

int aec_saved_on(void)
{
    char v[8] = "";
    return control_saved_get("audio.aec", v, sizeof v) == 0 && !strcmp(v, "on");
}
#else
void aec_host_config(int on, int delay_ms, int tail_ms)
{
    s_aec = on; s_delay_ms = delay_ms; s_tail_ms = tail_ms; g_reinit = 1;
    if (getenv("AEC_SCALE")) s_scale = atoi(getenv("AEC_SCALE"));
}
#endif
