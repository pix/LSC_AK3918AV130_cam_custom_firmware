#include "speaker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include "monotime.h"

/* Custom sound baked in at build time from sounds/custom.wav (see
 * gen_sound.py; generated file is git-ignored). Empty when absent. */
extern const int16_t speaker_custom_pcm[];
extern const size_t  speaker_custom_len;

#define QCAP (SPEAKER_RATE * 20)      /* 20 s queue */
#define STREAM_PREBUF (SPEAKER_RATE * 25 / 100)   /* 250 ms cushion: absorbs network jitter */
#define STREAM_TARGET (SPEAKER_RATE * 30 / 100)   /* latency after a trim */
#define STREAM_MAX    (SPEAKER_RATE * 60 / 100)   /* trim when more is queued */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t  g_q[QCAP];
static size_t   g_head, g_len;        /* ring: read at g_head, g_len samples queued */
static int      g_volume = 80;
static int      g_amp_on, g_idle_periods, g_ready;
/* talk-back jitter buffer (speaker_queue_stream) */
static int      g_hold;               /* collecting before (re)starting playback */
static int64_t  g_stream_last_ms;

/* ---- amplifier ------------------------------------------------------------- */
/* The speaker amplifier belongs to the ak_pcm driver (DT gpio, driven via
 * gpiod in its speaker ioctl) - we switch it through audio.c's ioctl hook,
 * the same call stock anyka_ipc makes (osal_ao_enable_speaker). */
static int (*g_amp_fn)(int on);

void speaker_set_amp_fn(int (*fn)(int on)) { g_amp_fn = fn; }

static void amp(int on)
{
    if (on == g_amp_on || !g_amp_fn) return;
    if (g_amp_fn(on) == 0) g_amp_on = on;
}

int speaker_init(void)
{
    /* Release a sysfs export of the amp GPIO left by older test builds so it
     * cannot interfere with the driver's own control of the pin. */
    char p[64];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d", SPEAKER_AMP_GPIO);
    FILE *f = fopen("/sys/class/gpio/unexport", "w");
    if (f) {
        FILE *g = fopen(p, "r");    /* only if exported */
        if (g) { fclose(g); fprintf(f, "%d", SPEAKER_AMP_GPIO); }
        fclose(f);
    }
    g_amp_on = 0;
    return 0;
}

/* ---- queue ----------------------------------------------------------------- */
size_t speaker_queue(const int16_t *pcm, size_t n)
{
    pthread_mutex_lock(&g_lock);
    size_t room = QCAP - g_len, k = n < room ? n : room;
    for (size_t i = 0; i < k; i++) g_q[(g_head + g_len + i) % QCAP] = pcm[i];
    g_len += k;
    pthread_mutex_unlock(&g_lock);
    return k;
}

size_t speaker_queue_stream(const int16_t *pcm, size_t n)
{
    pthread_mutex_lock(&g_lock);
    /* Hold (build the cushion) only at the start of a talk-back or after a
     * real pause. Re-holding on every momentary queue-empty punished normal
     * jitter with a 250 ms silence each time (measured: 38-95 dropouts per
     * 6 s); mid-stream underruns now just play silence for the gap. */
    if (mono_ms() - g_stream_last_ms > 300) g_hold = 1;
    if (g_len + n > STREAM_MAX) {               /* too much latency: drop the OLDEST */
        size_t drop = g_len + n - STREAM_TARGET;
        if (drop > g_len) drop = g_len;
        g_head = (g_head + drop) % QCAP;
        g_len -= drop;
    }
    size_t room = QCAP - g_len, k = n < room ? n : room;
    for (size_t i = 0; i < k; i++) g_q[(g_head + g_len + i) % QCAP] = pcm[i];
    g_len += k;
    g_stream_last_ms = mono_ms();
    pthread_mutex_unlock(&g_lock);
    return k;
}

void speaker_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_len = 0;
    g_hold = 0;
    pthread_mutex_unlock(&g_lock);
}

int speaker_busy(void) { return g_len > 0; }

/* Thread-safe "the speaker is playing something right now" (audio.c uses it
 * to mute the microphone while the speaker is busy). The queue can empty
 * between talk-back packets, and the amp runs on after the last sample, so
 * a streamed talk-back keeps the microphone muted for 1 s after its last
 * writer — otherwise the gap leaked mic audio back into the speaker
 * (feedback / Larsen during hold-to-talk). */
int speaker_playing(void)
{
    pthread_mutex_lock(&g_lock);
    int64_t since = mono_ms() - g_stream_last_ms;
    int b = g_len > 0 || since < 1000;          /* any talk-back stream keeps the mic muted */
    pthread_mutex_unlock(&g_lock);
    return b;
}
size_t speaker_queued(void) { return g_len; }

size_t speaker_fill(int16_t *out, size_t n)
{
    pthread_mutex_lock(&g_lock);
    if (g_hold) {     /* talk-back: play once STREAM_PREBUF is buffered or the sender paused */
        if (g_len >= STREAM_PREBUF || mono_ms() - g_stream_last_ms > 120) g_hold = 0;
    }
    size_t k = g_hold ? 0 : (n < g_len ? n : g_len);
    int vol = g_volume;
    for (size_t i = 0; i < k; i++) {
        int32_t s = (int32_t)g_q[(g_head + i) % QCAP] * vol / 100;
        out[i] = (int16_t)s;
    }
    g_head = (g_head + k) % QCAP;
    g_len -= k;
    pthread_mutex_unlock(&g_lock);
    if (k < n) memset(out + k, 0, (n - k) * sizeof(int16_t));

    if (k) { amp(1); g_idle_periods = 0; }
    else if (g_amp_on && ++g_idle_periods > 16) amp(0);   /* ~0.5 s of silence */
    return k;
}

void speaker_set_volume(int p) { g_volume = p < 0 ? 0 : p > 100 ? 100 : p; }
int  speaker_get_volume(void)  { return g_volume; }
int  speaker_ready(void)       { return g_ready; }
void speaker_set_ready(int ok) { g_ready = ok; }

/* ---- built-in sounds --------------------------------------------------------- */
static void synth_note(int16_t *o, size_t n, double hz, double amp_lin)
{
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / SPEAKER_RATE;
        double env = exp(-t * 6.0) * (i < 80 ? i / 80.0 : 1.0);         /* soft attack, bell decay */
        double v = sin(2 * M_PI * hz * t) + 0.3 * sin(2 * M_PI * 2 * hz * t);
        o[i] = (int16_t)(v * env * amp_lin * 20000);
    }
}

int speaker_play_builtin(const char *name)
{
    static int16_t buf[SPEAKER_RATE * 2];
    if (!strcmp(name, "custom")) {
        if (!speaker_custom_len) return -1;
        speaker_queue(speaker_custom_pcm, speaker_custom_len);
        return 0;
    }
    if (!strcmp(name, "tone")) {
        for (int i = 0; i < SPEAKER_RATE; i++)
            buf[i] = (int16_t)(sin(2 * M_PI * 440.0 * i / SPEAKER_RATE) * 12000);
        speaker_queue(buf, SPEAKER_RATE);
        return 0;
    }
    if (!strcmp(name, "chime")) {   /* rising "ta-da": C5 E5 G5, then C6 held */
        static const double notes[] = { 523.25, 659.25, 783.99 };
        size_t pos = 0;
        memset(buf, 0, sizeof(buf));
        for (int k = 0; k < 3; k++) { synth_note(buf + pos, 1100, notes[k], 0.8); pos += 1100; }
        synth_note(buf + pos, SPEAKER_RATE * 2 - pos, 1046.5, 1.0);
        speaker_queue(buf, SPEAKER_RATE * 2);
        return 0;
    }
    return -1;
}

/* ---- WAV decoding (untrusted input) ------------------------------------------- */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int speaker_play_wav(const uint8_t *d, size_t len, const char **err)
{
    *err = "not a WAV file";
    if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return -1;
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    size_t dlen = 0, i = 12;
    while (i + 8 <= len) {
        uint32_t clen = le32(d + i + 4);
        if (clen > len - i - 8) clen = (uint32_t)(len - i - 8);   /* tolerate truncated tail */
        if (!memcmp(d + i, "fmt ", 4) && clen >= 16) {
            fmt = le16(d + i + 8); ch = le16(d + i + 10); rate = le32(d + i + 12); bits = le16(d + i + 22);
        } else if (!memcmp(d + i, "data", 4)) {
            data = d + i + 8; dlen = clen;
        }
        i += 8 + (size_t)clen + (clen & 1);
    }
    if (fmt != 1 || !data) { *err = "WAV must be uncompressed PCM"; return -1; }
    if (ch < 1 || ch > 2 || (bits != 8 && bits != 16)) { *err = "WAV must be 8/16-bit mono or stereo"; return -1; }
    if (rate < 4000 || rate > 48000) { *err = "WAV sample rate must be 4-48 kHz"; return -1; }
    size_t frame = (size_t)ch * (bits / 8), nin = dlen / frame;
    if (!nin) { *err = "WAV has no samples"; return -1; }
    /* resample (linear) to 8 kHz mono, in chunks */
    int16_t out[512];
    size_t no = 0;
    uint64_t total_out = (uint64_t)nin * SPEAKER_RATE / rate;
    if (total_out > QCAP) total_out = QCAP;   /* at most 20 s */
    for (uint64_t k = 0; k < total_out; k++) {
        uint64_t pos16 = k * rate * 65536 / SPEAKER_RATE;   /* 16.16 fixed point input index */
        size_t a = (size_t)(pos16 >> 16), b = a + 1 < nin ? a + 1 : a;
        int32_t fr = (int32_t)(pos16 & 0xffff), sa = 0, sb = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t *pa = data + a * frame + (size_t)c * (bits / 8), *pb = data + b * frame + (size_t)c * (bits / 8);
            sa += bits == 16 ? (int16_t)le16(pa) : ((int)pa[0] - 128) * 256;
            sb += bits == 16 ? (int16_t)le16(pb) : ((int)pb[0] - 128) * 256;
        }
        sa /= ch; sb /= ch;
        out[no++] = (int16_t)(sa + (int32_t)(((int64_t)(sb - sa) * fr) >> 16));
        if (no == sizeof(out) / sizeof(out[0])) { speaker_queue(out, no); no = 0; }
    }
    if (no) speaker_queue(out, no);
    return 0;
}
