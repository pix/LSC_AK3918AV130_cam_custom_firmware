/* Host simulation of the camera echo canceller (firmware aec.c + SpeexDSP).
 *
 * Simulates the two PCM streams on one codec clock: the loopback delivers
 * the speaker signal in 512-sample periods, the capture delivers the
 * microphone (echo through a room response + near-end talker + noise) in
 * 256-sample periods. Each read is timestamped with random scheduling
 * delay, sometimes a long stall, exactly as aec_ref_push()/aec_process()
 * see them on the camera. Reports echo reduction while only the far end
 * talks and how much of the near-end talker survives double talk.
 *
 * build: tests/aec/run.sh   usage: test_aec [delay_samples] [echo_gain] */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aec.h"

void aec_host_config(int on, int delay_ms, int tail_ms);
int64_t mono_us(void) { return 0; }

#define FS 8000
#define SECS 24
#define LEN (FS * SECS)
#define KR 1234         /* loopback index -> time */
#define KM (-777)       /* capture index -> time */

static uint32_t rng = 12345;
static double STALLP = 0.03;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }
static double grand(void) { double u = urand() + 1e-12, v = urand(); return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }

/* speech-like: pink-ish noise, syllable envelope (4 Hz) and pauses */
static void speechlike(double *x, int n, double level, uint32_t seed)
{
    uint32_t save = rng; rng = seed;
    /* white noise through a 200..3400 Hz band (speech band, no DC) with a
     * resonance that wanders like a formant */
    double w1 = 0, y1 = 0, y2 = 0, lp = 0, ph = urand() * 6.28;
    for (int i = 0; i < n; i++) {
        double w = grand();
        double hp = w - w1; w1 = w;                                   /* zero at DC */
        double f = 500 + 400 * sin(ph * 0.7 + 2 * M_PI * 0.9 * i / FS);
        double r = 0.97, c = 2 * r * cos(2 * M_PI * f / FS);
        double y = hp + c * y1 - r * r * y2; y2 = y1; y1 = y;           /* formant */
        lp = 0.5 * lp + 0.5 * (0.15 * y + hp);                         /* roll off the top */
        double env = 0.5 + 0.5 * sin(ph + 2 * M_PI * 4.0 * i / FS);
        if (((i / (FS * 3 / 2)) % 4) == 3) env *= 0.05;                /* pauses */
        x[i] = level * env * env * lp;
    }
    rng = save;
}

typedef struct { double t; int kind; int64_t idx; } ev_t;   /* kind 0 = ref, 1 = mic */
static int evcmp(const void *a, const void *b)
{
    double d = ((const ev_t *)a)->t - ((const ev_t *)b)->t;
    return d < 0 ? -1 : d > 0;
}

static int16_t clip(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }

int main(int argc, char **argv)
{
    int D = argc > 1 ? atoi(argv[1]) : 24;              /* DAC + acoustic delay (samples) */
    double G = argc > 2 ? atof(argv[2]) : 0.8;          /* echo path gain */
    int delay_ms = argc > 3 ? atoi(argv[3]) : 0;
    static double far[LEN], near[LEN], h[600];
    if (getenv("STALL")) STALLP = atof(getenv("STALL"));
    double FL = getenv("FAR") ? atof(getenv("FAR")) : 9000;
    speechlike(far, LEN, FL, 1);
    speechlike(near, LEN, FL * 0.28, 7);
    for (int i = 0; i < LEN; i++) if (i < 14 * FS || i >= 18 * FS) near[i] = 0;   /* double talk 14..18 s */
    for (int i = 0; i < 600; i++) h[i] = G * grand() * exp(-i / 80.0) * 0.35;
    h[0] += G * 0.6;

    /* time-domain signals (time index tau = sample periods) */
    static double echo[LEN];
    for (int t = 0; t < LEN; t++) {
        double e = 0;
        for (int i = 0; i < 600; i++) { int s = t - D - i; if (s >= 0) e += h[i] * far[s]; }
        echo[t] = e;
    }
    /* streams: loopback k = tau - KR, capture j = tau - KM */
    int nref = LEN - KR - 1024, nmic = LEN + KM - 1024;
    static ev_t ev[LEN / 128];
    int ne = 0;
    /* reads are sequential: a stalled read delays the ones after it, which
     * then return at once with the buffered periods */
    double last = 0;
    for (int k = 0; k + 512 <= nref; k += 512) {
        double t = (k + 512 + KR) / 8.0 + urand() * 5.0 + (urand() < STALLP ? 150.0 : 0);   /* ms */
        if (t < last) t = last + 0.05;
        ev[ne++] = (ev_t){ last = t, 0, k };
    }
    last = 0;
    for (int j = 0; j + 256 <= nmic; j += 256) {
        double t = (j + 256 + KM) / 8.0 + urand() * 3.0 + (urand() < 0.02 ? 40.0 : 0);
        if (t < last) t = last + 0.05;
        ev[ne++] = (ev_t){ last = t, 1, j };
    }
    qsort(ev, ne, sizeof ev[0], evcmp);

    aec_host_config(1, delay_ms, 128);
    static int16_t out[LEN], micraw[LEN];
    int64_t outn = 0;
    for (int e = 0; e < ne; e++) {
        int16_t b[512];
        int64_t t_us = (int64_t)(ev[e].t * 1000.0) + 1000000;
        if (ev[e].kind == 0) {
            for (int i = 0; i < 512; i++) b[i] = clip(far[ev[e].idx + i + KR]);
            aec_ref_push(b, 512, t_us);
        } else {
            for (int i = 0; i < 256; i++) {
                int t = (int)ev[e].idx + i + KM; if (t < 0) t = 0;
                b[i] = clip(echo[t] + near[t] + 30 * grand());
                micraw[ev[e].idx + i] = b[i];
            }
            aec_process(b, 256, t_us);
            memcpy(out + outn, b, sizeof(int16_t) * 256); outn += 256;
        }
    }
    /* out[j] is capture sample j - AEC_HOLD - P (P: processing delay, found
     * by matching the near-end talker) */
    int P = 0; double bestc = -1;
    for (int p = 0; p < 400; p++) {
        double c = 0, a = 0, b = 0;
        for (int j = 14 * FS + 2000; j < 18 * FS - 2000; j++) {
            int t = j - AEC_HOLD - p + KM;
            c += out[j] * near[t]; a += out[j] * (double)out[j]; b += near[t] * near[t];
        }
        c /= sqrt(a * b + 1);
        if (c > bestc) { bestc = c; P = p; }
    }
    double em = 0, eo = 0, dn = 0, dno = 0, de = 0, dg = 0;
    for (int s = 2; s < SECS - 1; s++) {
        double a = 0, b = 0;
        for (int j = s * FS; j < (s + 1) * FS; j++) {
            double m = micraw[j - AEC_HOLD - P], o = out[j];
            a += m * m; b += o * o;
        }
        printf("%2d s: in %5.1f dB out %5.1f dB\n", s, 10 * log10(a / FS + 1) - 90.3, 10 * log10(b / FS + 1) - 90.3);
    }
    for (int j = 10 * FS; j < 14 * FS; j++) {           /* far end only, converged */
        int t = j - AEC_HOLD - P + KM;
        double o = out[j];
        em += echo[t] * echo[t]; eo += o * o;
    }
    for (int j = 14 * FS + 2000; j < 18 * FS - 2000; j++) {   /* double talk */
        int t = j - AEC_HOLD - P + KM; if (t < 0) continue;
        double o = out[j];
        dn += near[t] * near[t];
        dg += o * near[t];
        dno += (o - near[t]) * (o - near[t]);
        de += echo[t] * echo[t];
    }
    printf("processing delay %d samples, near-end correlation %.2f, near-end gain %.2f\n", P, bestc, dg / dn);
    if (getenv("AEC_SIGS")) {                 /* out, near, echo, mic: s16 x4, time aligned */
        FILE *f = fopen(getenv("AEC_SIGS"), "wb");
        for (int j = AEC_HOLD + P + 1000; f && j < outn; j++) {
            int t = j - AEC_HOLD - P + KM;
            int16_t r[4] = { out[j], clip(near[t]), clip(echo[t]), micraw[j - AEC_HOLD - P] };
            fwrite(r, sizeof r, 1, f);
        }
        if (f) fclose(f);
    }
    char js[512]; aec_status_json(js, sizeof js);
    printf("%s\n", js);
    printf("D=%d G=%.2f: far-end-only echo reduction %.1f dB; double talk: echo/near %.1f dB in, "
           "error/near %.1f dB out\n", D, G, 10 * log10(em / (eo + 1)), 10 * log10(de / dn), 10 * log10(dno / dn));
    return 0;
}
