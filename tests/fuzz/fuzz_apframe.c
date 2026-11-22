/* Fuzz harness: apframe.c 802.11 management frame parsing (setup access
 * point; anyone in radio range can send these) plus the response builders
 * the AP would run on the parsed frame, with a fuzzed output capacity.
 * Input: [cap u8][ssid len u8][frame...]: the frame is an exact-size heap
 * copy so ASan catches reads past len. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "apframe.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 2) return 0;
    size_t cap = d[0] * 2u;
    ap_cfg_t c;
    memset(&c, 0, sizeof c);
    memcpy(c.bssid, "\x02\x11\x22\x33\x44\x55", 6);
    size_t sl = d[1] % (AP_SSID_MAX + 1);
    memset(c.ssid, 'S', sl);
    if (sl >= 15) memcpy(c.ssid, "LSC-Camera-CA01", 15);
    c.ssid[sl] = 0;
    c.channel = 6; c.beacon_int = 100;
    size_t len = n - 2;
    uint8_t *f = malloc(len ? len : 1);
    memcpy(f, d + 2, len);
    ap_rx_t rx;
    uint8_t *o = malloc(cap ? cap : 1);
    int r = ap_parse(f, len, &rx);
    if (r == 0) {
        if (rx.ssid_len > AP_SSID_MAX || rx.n_rates > AP_RATES_MAX) __builtin_trap();
        size_t k;
        if (ap_probe_matches(&c, &rx)) { k = ap_build_probe_resp(&c, rx.sa, o, cap); if (k > cap) __builtin_trap(); }
        k = ap_build_auth_resp(&c, rx.sa, rx.auth_seq, o, cap); if (k > cap) __builtin_trap();
        k = ap_build_assoc_resp(&c, rx.sa, rx.subtype_fc == FC_REASSOC_REQ, 0, rx.listen_int, o, cap); if (k > cap) __builtin_trap();
        k = ap_build_deauth(&c, rx.sa, rx.capab, o, cap); if (k > cap) __builtin_trap();
    } else if (rx.subtype_fc != -1) __builtin_trap();
    size_t k = ap_build_beacon_head(&c, o, cap); if (k > cap) __builtin_trap();
    k = ap_build_beacon_tail(&c, o, cap); if (k > cap) __builtin_trap();
    free(o);
    free(f);
    return 0;
}
