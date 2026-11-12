#include "sounddet.h"
#include "events.h"
#include "speaker.h"
#include "monotime.h"
#include "ctrl_params.h"
#include <math.h>

static int s_enabled, s_threshold = -30, s_hold = 5;
CTRL_BOOL(p_en, s_enabled)
CTRL_INT(p_thr, s_threshold, -60, -5)
CTRL_INT(p_hold, s_hold, 1, 60)

static volatile int g_level = -99;
static int64_t g_speaker_last, g_loud_last;
static int g_loud_run;

int sounddet_level_db(void) { return g_level; }

void sounddet_feed(const int16_t *pcm, size_t n)
{
    if (!n) return;
    double sq = 0;
    int dc = 0;
    for (size_t i = 0; i < n; i++) dc += pcm[i];
    dc /= (int)n;                                    /* remove DC offset of the ADC */
    for (size_t i = 0; i < n; i++) { double v = pcm[i] - dc; sq += v * v; }
    double rms = sqrt(sq / (double)n);
    int db = rms < 1 ? -99 : (int)lround(20 * log10(rms / 32768.0));
    g_level = db;
    if (!s_enabled) { if (g_loud_last) { g_loud_last = 0; events_sound(0, db); } return; }

    int64_t now = mono_ms();
    if (speaker_busy()) g_speaker_last = now;
    int own = now - g_speaker_last < 1500;           /* our own speaker (chime, talk-back) */
    if (!own && db >= s_threshold) {
        if (++g_loud_run >= 2) {                     /* two consecutive periods (~64 ms) */
            g_loud_last = now;
            events_sound(1, db);
        }
    } else {
        g_loud_run = 0;
    }
    if (g_loud_last && now - g_loud_last > (int64_t)s_hold * 1000) { g_loud_last = 0; events_sound(0, db); }
}

void sounddet_init(void)
{
    CTRL_REG("sound.enabled", p_en, CTRL_PERSIST);
    CTRL_REG("sound.threshold", p_thr, CTRL_PERSIST);
    CTRL_REG("sound.hold", p_hold, CTRL_PERSIST);
}
