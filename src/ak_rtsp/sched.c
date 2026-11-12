/* Weekly schedules (local time, one bit per hour, Monday 00:00 = bit 0):
 *   sched.record   hours when recording (record.mode, timelapse) may run; "" = always
 *   sched.events   hours when detections are reported (MQTT, webhook, ONVIF,
 *                  event clips and pictures); "" = always
 *   sched.privacy  hours of privacy mode: picture blacked out by the ISP (all
 *                  streams, snapshots, recordings), microphone muted, no
 *                  detections, no recording; "" = never
 * Value: 42 hex digits = 168 bits, MSB first (digit 0 bit 3 = Monday 00-01 h).
 * A thread applies the privacy state within 10 s of an hour change. */
#include "sched.h"
#include "imgx.h"
#include "live.h"
#include "ctrl_params.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HEXLEN 42
static char s_rec[HEXLEN + 1], s_ev[HEXLEN + 1], s_priv[HEXLEN + 1];
static volatile int g_priv_on = -1;

static int hex_ok(const char *v)
{
    if (!*v) return 1;
    if (strlen(v) != HEXLEN) return 0;
    for (; *v; v++) if (!isxdigit((unsigned char)*v)) return 0;
    return 1;
}
CTRL_STR(p_rec, s_rec, hex_ok, "empty or 42 hex digits (7 days x 24 hours, Monday first)")
CTRL_STR(p_ev, s_ev, hex_ok, "empty or 42 hex digits (7 days x 24 hours, Monday first)")
CTRL_STR(p_priv, s_priv, hex_ok, "empty or 42 hex digits (7 days x 24 hours, Monday first)")

static int hour_index(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return ((tm.tm_wday + 6) % 7) * 24 + tm.tm_hour;     /* Monday = 0 */
}

static int bit(const char *hex, int i)
{
    char c = hex[i / 4];
    int v = isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10);
    return (v >> (3 - i % 4)) & 1;
}

static int on_now(const char *hex, int empty_means)
{
    if (!hex[0]) return empty_means;
    return bit(hex, hour_index());
}

int sched_privacy(void)     { return g_priv_on > 0; }
int sched_record_ok(void)   { return !sched_privacy() && on_now(s_rec, 1); }
int sched_events_ok(void)   { return !sched_privacy() && on_now(s_ev, 1); }

static void apply_privacy(int on)
{
    if (on == g_priv_on) return;
    g_priv_on = on;
    printf("[sched] privacy mode %s\n", on ? "ON" : "off");
    imgx_set_privacy(on);
    live_set_audio_mute(on);
}

static void *thread(void *arg)
{
    (void)arg;
    for (;;) {
        apply_privacy(on_now(s_priv, 0));
        sleep(10);
    }
    return NULL;
}

/* privacy settings take effect at once */
static int priv_setx(const char *v, const char **e)
{
    int r = p_priv_set(v, e);
    if (!r && g_priv_on >= 0) apply_privacy(on_now(s_priv, 0));
    return r;
}

int sched_status_json(char *out, size_t cap)
{
    return snprintf(out, cap, "{\"privacy\":%s,\"record\":%s,\"events\":%s,\"hour\":%d}",
                    sched_privacy() ? "true" : "false", sched_record_ok() ? "true" : "false",
                    sched_events_ok() ? "true" : "false", hour_index());
}

void sched_init(void)
{
    CTRL_REG("sched.record", p_rec, CTRL_PERSIST);
    CTRL_REG("sched.events", p_ev, CTRL_PERSIST);
    ctrl_param_t pv = { "sched.privacy", p_priv_get, priv_setx, CTRL_PERSIST };
    control_register(&pv);
}

void sched_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 32 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, thread, NULL);
    pthread_attr_destroy(&a);
}
