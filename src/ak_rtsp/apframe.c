#include "apframe.h"
#include <string.h>

/* 802.11b/g rates in 500 kb/s units; 0x80 = basic rate. */
static const uint8_t k_rates[]     = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
static const uint8_t k_ext_rates[] = { 0x30, 0x48, 0x60, 0x6c };
#define CAPAB 0x0421   /* ESS | short preamble | short slot time; no privacy (open) */

typedef struct { uint8_t *p; size_t len, cap; int bad; } wb_t;

static void put(wb_t *w, const void *d, size_t n)
{
    if (w->bad || n > w->cap - w->len) { w->bad = 1; return; }
    memcpy(w->p + w->len, d, n);
    w->len += n;
}
static void put8(wb_t *w, uint8_t v) { put(w, &v, 1); }
static void put16(wb_t *w, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(w, b, 2); }
static void zeros(wb_t *w, size_t n) { static const uint8_t z[8]; while (n) { size_t k = n > 8 ? 8 : n; put(w, z, k); n -= k; } }
static void ie(wb_t *w, uint8_t id, const void *d, size_t n) { put8(w, id); put8(w, (uint8_t)n); put(w, d, n); }

static void hdr(wb_t *w, uint8_t fc, const uint8_t *da, const uint8_t *bssid)
{
    put8(w, fc); put8(w, 0);          /* frame control */
    put16(w, 0);                      /* duration (filled by driver) */
    put(w, da, 6); put(w, bssid, 6); put(w, bssid, 6);
    put16(w, 0);                      /* sequence (assigned by mac80211) */
}

static void fixed_and_ies(wb_t *w, const ap_cfg_t *c, int with_tail_ies)
{
    zeros(w, 8);                      /* timestamp (driver fills for beacons) */
    put16(w, c->beacon_int);
    put16(w, CAPAB);
    ie(w, 0, c->ssid, strlen(c->ssid));
    ie(w, 1, k_rates, sizeof(k_rates));
    ie(w, 3, &c->channel, 1);
    if (with_tail_ies) {
        uint8_t erp = 0;
        ie(w, 42, &erp, 1);
        ie(w, 50, k_ext_rates, sizeof(k_ext_rates));
    }
}

static size_t done(wb_t *w) { return w->bad ? 0 : w->len; }

size_t ap_build_beacon_head(const ap_cfg_t *c, uint8_t *out, size_t cap)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    wb_t w = { out, 0, cap, 0 };
    hdr(&w, FC_BEACON, bcast, c->bssid);
    fixed_and_ies(&w, c, 0);          /* mac80211 inserts the TIM between head and tail */
    return done(&w);
}

size_t ap_build_beacon_tail(const ap_cfg_t *c, uint8_t *out, size_t cap)
{
    (void)c;
    wb_t w = { out, 0, cap, 0 };
    uint8_t erp = 0;
    ie(&w, 42, &erp, 1);
    ie(&w, 50, k_ext_rates, sizeof(k_ext_rates));
    return done(&w);
}

size_t ap_build_probe_resp(const ap_cfg_t *c, const uint8_t *da, uint8_t *out, size_t cap)
{
    wb_t w = { out, 0, cap, 0 };
    hdr(&w, FC_PROBE_RESP, da, c->bssid);
    fixed_and_ies(&w, c, 1);
    return done(&w);
}

size_t ap_build_auth_resp(const ap_cfg_t *c, const uint8_t *da, uint16_t status, uint8_t *out, size_t cap)
{
    wb_t w = { out, 0, cap, 0 };
    hdr(&w, FC_AUTH, da, c->bssid);
    put16(&w, 0);        /* open system */
    put16(&w, 2);        /* transaction sequence 2 */
    put16(&w, status);
    return done(&w);
}

size_t ap_build_assoc_resp(const ap_cfg_t *c, const uint8_t *da, int reassoc, uint16_t status,
                           uint16_t aid, uint8_t *out, size_t cap)
{
    wb_t w = { out, 0, cap, 0 };
    hdr(&w, reassoc ? FC_REASSOC_RESP : FC_ASSOC_RESP, da, c->bssid);
    put16(&w, CAPAB);
    put16(&w, status);
    put16(&w, (uint16_t)(aid | 0xc000));
    ie(&w, 1, k_rates, sizeof(k_rates));
    ie(&w, 50, k_ext_rates, sizeof(k_ext_rates));
    return done(&w);
}

size_t ap_build_deauth(const ap_cfg_t *c, const uint8_t *da, uint16_t reason, uint8_t *out, size_t cap)
{
    wb_t w = { out, 0, cap, 0 };
    hdr(&w, FC_DEAUTH, da, c->bssid);
    put16(&w, reason);
    return done(&w);
}

/* ---- parsing (untrusted input) ---------------------------------------------- */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Walk information elements in [p, p+n). Collects SSID and rates. */
static int parse_ies(const uint8_t *p, size_t n, ap_rx_t *o)
{
    size_t i = 0;
    while (i + 2 <= n) {
        uint8_t id = p[i], l = p[i + 1];
        if ((size_t)l > n - i - 2) return -1;            /* truncated element */
        const uint8_t *v = p + i + 2;
        if (id == 0) {
            if (l > AP_SSID_MAX) return -1;
            o->ssid_present = 1;
            o->ssid_len = l;
            memcpy(o->ssid, v, l);
        } else if (id == 1 || id == 50) {
            for (uint8_t k = 0; k < l && o->n_rates < AP_RATES_MAX; k++)
                o->rates[o->n_rates++] = v[k];
        }
        i += 2 + (size_t)l;
    }
    return 0;   /* trailing single byte, if any, is ignored */
}

int ap_parse(const uint8_t *f, size_t len, ap_rx_t *o)
{
    memset(o, 0, sizeof(*o));
    o->subtype_fc = -1;
    if (!f || len < 24) return -1;
    if ((f[0] & 0x0c) != 0) return -1;                   /* not a management frame */
    if (f[1] & 0x40) return -1;                          /* protected: we run open */
    uint8_t fc = f[0] & 0xf0;
    memcpy(o->da, f + 4, 6);
    memcpy(o->sa, f + 10, 6);
    memcpy(o->bssid, f + 16, 6);
    const uint8_t *b = f + 24;
    size_t n = len - 24;

    switch (fc) {
    case FC_PROBE_REQ:
        if (parse_ies(b, n, o) != 0) return -1;
        break;
    case FC_AUTH:
        if (n < 6) return -1;
        o->auth_alg = le16(b);
        o->auth_seq = le16(b + 2);
        break;
    case FC_ASSOC_REQ:
        if (n < 4) return -1;
        o->capab = le16(b);
        o->listen_int = le16(b + 2);
        if (parse_ies(b + 4, n - 4, o) != 0) return -1;
        break;
    case FC_REASSOC_REQ:
        if (n < 10) return -1;
        o->capab = le16(b);
        o->listen_int = le16(b + 2);
        if (parse_ies(b + 10, n - 10, o) != 0) return -1;   /* skip current AP addr */
        break;
    case FC_DISASSOC:
    case FC_DEAUTH:
        if (n < 2) return -1;
        break;
    default:
        return -1;
    }
    o->subtype_fc = fc;
    return 0;
}

int ap_probe_matches(const ap_cfg_t *c, const ap_rx_t *rx)
{
    if (rx->subtype_fc != FC_PROBE_REQ) return 0;
    if (!rx->ssid_present || rx->ssid_len == 0) return 1;    /* wildcard */
    size_t l = strlen(c->ssid);
    return rx->ssid_len == l && memcmp(rx->ssid, c->ssid, l) == 0;
}
