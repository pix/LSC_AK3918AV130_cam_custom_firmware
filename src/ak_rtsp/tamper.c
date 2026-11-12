/* Tamper detection: lens covered / camera moved or redirected.
 *
 * Once a second on the 640x360 sub-stream luma:
 *  - 32x18 block means (the burned-in overlay rows left out) and the mean
 *    absolute horizontal gradient (texture).
 *  - "covered": almost no texture and a flat picture (hand, tape, cloth, the
 *    lens pushed against a wall) for COVER_S seconds.
 *  - "moved": the normalised block pattern no longer correlates with the
 *    reference (one per day/night mode) for MOVE_S seconds. The alert stays on
 *    for MOVE_HOLD_S, then the new view becomes the reference.
 *    The reference follows slow light changes while the correlation is high.
 * Paused for 15 s after a day/night switch. Setting: tamper.enabled. */
#include "tamper.h"
#include "events.h"
#include "osd.h"
#include "night.h"
#include "vi.h"
#include "monotime.h"
#include "ctrl_params.h"
#include "sched.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TC 32
#define TR 18
#define TN (TC * TR)
#define COVER_S      10
#define MOVE_S       20
#define MOVE_HOLD_S  60
#define COVER_GRAD   2.0f     /* mean |dY| per 2 px; a normal scene is 4..20 */
#define COVER_SPREAD 6.0f     /* stddev of block means */
#define MOVE_CORR    0.45f

static int s_enabled;
CTRL_BOOL(p_en, s_enabled)

static volatile int g_on;                 /* 0 none, 1 covered, 2 moved */
static volatile float g_corr = 1, g_grad, g_spread;
static float g_ref[2][TN];
static int g_have_ref[2];

static int feat(const uint8_t *y, int w, int h, float *blk, float *grad, float *spread)
{
    int bw = w / TC, bh = h / TR;
    int r0 = -1, r1 = -1;                  /* overlay rows (the clock changes every second) */
    osd_cfg_t oc;
    osd_get(&oc);
    if (oc.enabled) {
        int th = oc.size == 2 ? 34 : 18, y0 = oc.pos[0] == 'b' ? h - 6 - th : 6;
        r0 = y0 / bh; r1 = (y0 + th - 1) / bh;
    }
    double gsum = 0; long gn = 0;
    double m = 0; int n = 0;
    for (int r = 0; r < TR; r++)
        for (int c = 0; c < TC; c++) {
            if (r >= r0 && r <= r1) { blk[r * TC + c] = -1; continue; }
            uint32_t sum = 0, cnt = 0;
            for (int yy = r * bh; yy < (r + 1) * bh; yy += 2) {
                const uint8_t *row = y + (size_t)yy * w;
                for (int x = c * bw; x < (c + 1) * bw; x += 2) {
                    sum += row[x]; cnt++;
                    if (x + 2 < w) { int d = (int)row[x + 2] - (int)row[x]; gsum += d < 0 ? -d : d; gn++; }
                }
            }
            blk[r * TC + c] = cnt ? (float)sum / (float)cnt : 0;
            m += blk[r * TC + c]; n++;
        }
    if (!n) return -1;
    m /= n;
    double v = 0;
    for (int i = 0; i < TN; i++) if (blk[i] >= 0) v += (blk[i] - m) * (blk[i] - m);
    *spread = (float)sqrt(v / n);
    *grad = gn ? (float)(gsum / (double)gn) : 0;
    /* normalise in place (zero mean, unit variance) for the correlation; overlay cells = 0 */
    float sd = *spread > 0.5f ? *spread : 0.5f;
    for (int i = 0; i < TN; i++) blk[i] = blk[i] >= 0 ? (float)((blk[i] - m) / sd) : 0;
    return n;
}

static float corr(const float *a, const float *b)
{
    double s = 0, sa = 0, sb = 0;
    for (int i = 0; i < TN; i++) { s += (double)a[i] * b[i]; sa += (double)a[i] * a[i]; sb += (double)b[i] * b[i]; }
    return sa > 0 && sb > 0 ? (float)(s / sqrt(sa * sb)) : 1.0f;
}

static void set_state(int st)
{
    if (st == g_on) return;
    static const char *why[] = { "", "lens covered", "camera moved" };
    printf("[tamper] %s\n", st ? why[st] : "cleared");
    int was = g_on;
    g_on = st;
    if (!was || !st) events_tamper(st);
}

static void *thread(void *arg)
{
    (void)arg;
    static uint8_t frame[640 * 360 * 3 / 2];
    static float blk[TN];
    int was_night = -1, cover_n = 0, move_n = 0, back_n = 0;
    int64_t pause_until = 0, moved_since = 0;
    for (;; sleep(1)) {
        int64_t now = mono_ms();
        if (!s_enabled) { set_state(0); cover_n = move_n = 0; g_have_ref[0] = g_have_ref[1] = 0; continue; }
        if (sched_privacy()) { set_state(0); cover_n = move_n = 0; pause_until = now + 15000; continue; }
        int night = night_is_night() ? 1 : 0;
        if (night != was_night) { was_night = night; pause_until = now + 15000; cover_n = move_n = 0; continue; }
        if (now < pause_until) continue;
        int w, h;
        if (vi_copy_sub_frame(frame, sizeof frame, &w, &h) < 0) continue;
        float grad, spread;
        int n = feat(frame, w, h, blk, &grad, &spread);
        if (n <= 0) continue;
        g_grad = grad; g_spread = spread;

        int covered = grad < COVER_GRAD && spread < COVER_SPREAD;
        cover_n = covered ? cover_n + 1 : 0;

        float *ref = g_ref[night];
        if (!g_have_ref[night]) {
            if (!covered) { memcpy(ref, blk, sizeof blk); g_have_ref[night] = 1; }
            g_corr = 1;
        } else {
            float c = covered ? 1 : corr(blk, ref);
            g_corr = c;
            move_n = c < MOVE_CORR ? move_n + 1 : 0;
            if (!covered && c > 0.85f) for (int i = 0; i < TN; i++) ref[i] += (blk[i] - ref[i]) * 0.02f;
        }

        if (cover_n >= COVER_S) set_state(1);
        else if (g_on == 2) {
            back_n = g_corr > 0.8f ? back_n + 1 : 0;
            if (back_n >= 3) { move_n = 0; set_state(0); }       /* put back as it was */
            else if (now - moved_since > MOVE_HOLD_S * 1000LL) { /* accept the new view */
                memcpy(ref, blk, sizeof blk);
                move_n = 0;
                set_state(0);
            }
        } else if (move_n >= MOVE_S) { moved_since = now; back_n = 0; set_state(2); }
        else if (g_on == 1 && !covered) set_state(0);
    }
    return NULL;
}

int tamper_status_json(char *out, size_t cap)
{
    static const char *st[] = { "none", "covered", "moved" };
    return snprintf(out, cap, "{\"enabled\":%s,\"tamper\":%s,\"state\":\"%s\",\"corr\":%.2f,\"texture\":%.1f,\"spread\":%.1f}",
                    s_enabled ? "true" : "false", g_on ? "true" : "false", st[g_on], g_corr, g_grad, g_spread);
}

void tamper_init(void)
{
    CTRL_REG("tamper.enabled", p_en, CTRL_PERSIST);
}

void tamper_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, thread, NULL);
    pthread_attr_destroy(&a);
}
