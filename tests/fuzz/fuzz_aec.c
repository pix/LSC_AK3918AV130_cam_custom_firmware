/* Fuzz harness: aec.c echo canceller (alignment, delay line, SpeexDSP MDF +
 * preprocessor, echo ducking, auto scale) with fuzzed reference/microphone
 * blocks, block sizes, timestamps and audio.aec_* settings.
 *
 * Input: 8 setting bytes (on, delay, tail, suppress, ns, scale, nlp, gen),
 * then ops of 6 bytes: [op][n u16][dt u16][sig]:
 *   op%5: 0 reference block (aec_ref_push), 1 microphone block (aec_process),
 *         2 settings change through the web setters, 3 loopback lost, 4 stall
 *   n:   samples (the capture passes <= 1024; larger ones are also tried)
 *   dt:  timestamp step in units of 16 us (each stream monotonic, as mono_us())
 *   sig: generator: silence, full-scale square, noise, speech-like, input bytes
 * All module state is reset before each input. */
#include "aec.c"
#include <stdint.h>

static void reset_all(void)
{
    states_free();
    s_aec = 0; s_delay_ms = 0; s_tail_ms = 128; s_suppress = 40; s_ns = 10; s_scale = -1; s_nlp = 40;
    g_auto_scale = 36; g_mic_shift = 3; g_ref_shift = 3; g_reinit = 1; g_rebuild = 0;
    memset(g_ref, 0, sizeof g_ref); g_R = 0; memset(g_ra, 0, sizeof g_ra); g_ra_n = g_ra_i = 0; g_Ar = 0;
    g_ref_last_us = 0; g_ref_alive = 0; g_ref_gaps = 0;
    memset(g_mic, 0, sizeof g_mic); memset(g_out, 0, sizeof g_out); g_M = g_P = 0;
    memset(g_ma, 0, sizeof g_ma); g_ma_n = g_ma_i = 0; g_off = 0; g_off_set = g_off_pending = 0;
    g_running = 0; g_active = 0;
    g_late = g_frames = g_realign = g_concealed = 0; g_e_mic = g_e_out = g_e_ref = 0;
    g_peak_ms = -1; g_peak_ratio = 0;
    g_path_m = g_path_r = 0; g_path_n = 0; g_path_db = 0;
    g_duck_floor = 1e12; g_duck_hold = 0; g_duck_gain = 1.0f; g_duck_frames = 0;
}

static uint32_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void gen(int16_t *s, size_t n, unsigned kind, const uint8_t *d, size_t dn, size_t *dpos, uint64_t t)
{
    for (size_t i = 0; i < n; i++) {
        int v;
        switch (kind % 6) {
        case 0: v = 0; break;
        case 1: v = ((t + i) / 20) & 1 ? 32767 : -32768; break;
        case 2: v = (int)(rnd() & 0xffff) - 32768; break;
        case 3: v = (int)(((t + i) * 37 % 400) - 200) * ((int)((t + i) / 2000 % 2) ? 60 : 3); break;
        case 4: v = (int)(rnd() % 2001) - 1000; break;
        default:
            if (dn) { v = (int16_t)(d[*dpos % dn] | d[(*dpos + 1) % dn] << 8); *dpos += 2; } else v = 0;
        }
        s[i] = (int16_t)v;
    }
}

static void set_int(int (*fn)(const char *, const char **), int v)
{
    char b[16]; const char *e;
    snprintf(b, sizeof b, "%d", v);
    fn(b, &e);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 8) return 0;
    reset_all();
    g_rng = 1;
    const char *e;
    p_aec_set(d[0] & 1 ? "on" : "off", &e);
    set_int(p_delay_set, (int)d[1] * 2 - 100);
    set_int(p_tail_set, 32 + d[2] * 2);
    set_int(p_sup_set, d[3] % 61);
    set_int(p_ns_set, d[4] % 31);
    if (d[5] >= 49) p_scale_set("auto", &e); else set_int(p_scale_set, d[5]);
    set_int(p_nlp_set, d[6] % 61);
    const uint8_t *audio = d + 8;
    size_t an = n - 8, apos = 0;
    int64_t t_ref = 1000000000 + (int64_t)d[7] * 1000, t_mic = 1000000000;
    uint64_t sref = 0, smic = 0;
    static int16_t buf[20000];
    const uint8_t *p = d + 8, *end = d + n;
    int ops = 0;
    while (end - p >= 6 && ops < 200) {
        unsigned op = p[0] % 5, sig = p[5];
        size_t ns = (size_t)(p[1] | p[2] << 8);
        int64_t dt = (int64_t)(p[3] | p[4] << 8) * 16;
        p += 6; ops++;
        switch (op) {
        case 0: {
            if (ns > 16384) ns %= 16384;
            gen(buf, ns, sig, audio, an, &apos, sref);
            t_ref += dt;
            aec_ref_push(buf, ns, t_ref);
            sref += ns;
            break;
        }
        case 1: {
            if (sig & 0x80) ns %= 1025;                   /* audio.c reads periods of <= 1024 */
            else if (ns > 16384) ns %= 16384;
            gen(buf, ns, sig, audio, an, &apos, smic);
            t_mic += dt;
            aec_process(buf, ns, t_mic);
            smic += ns;
            break;
        }
        case 2:
            switch (sig % 8) {
            case 0: p_aec_set(ns & 1 ? "on" : "off", &e); break;
            case 1: set_int(p_delay_set, (int)(ns % 501) - 100); break;
            case 2: set_int(p_tail_set, (int)(ns % 481) + 32); break;
            case 3: set_int(p_sup_set, (int)(ns % 61)); break;
            case 4: set_int(p_ns_set, (int)(ns % 31)); break;
            case 5: if (ns > 48) p_scale_set("auto", &e); else set_int(p_scale_set, (int)ns); break;
            case 6: set_int(p_nlp_set, (int)(ns % 61)); break;
            case 7: set_int(p_tail_set, (int)ns - 32768); break;  /* out of range: rejected */
            }
            break;
        case 3: aec_ref_lost(); break;
        case 4: t_mic += dt * 64; t_ref += dt * 32; break;   /* scheduling stall */
        }
    }
    char json[600];
    aec_status_json(json, sizeof json);
    (void)aec_active();
    reset_all();
    return 0;
}
