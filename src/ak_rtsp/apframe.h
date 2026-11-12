#pragma once
/* 802.11 management frames for the built-in OPEN setup access point.
 * Pure functions (no I/O) so they can be fuzzed on the host: every parser
 * treats its input as attacker-controlled (anyone in radio range can send
 * frames while the AP runs). */
#include <stddef.h>
#include <stdint.h>

#define AP_SSID_MAX   32
#define AP_RATES_MAX  16

/* Frame control values (little-endian first byte; type 0 = management) */
#define FC_ASSOC_REQ    0x00
#define FC_ASSOC_RESP   0x10
#define FC_REASSOC_REQ  0x20
#define FC_REASSOC_RESP 0x30
#define FC_PROBE_REQ    0x40
#define FC_PROBE_RESP   0x50
#define FC_BEACON       0x80
#define FC_DISASSOC     0xa0
#define FC_AUTH         0xb0
#define FC_DEAUTH       0xc0

typedef struct {
    uint8_t  bssid[6];
    char     ssid[AP_SSID_MAX + 1];
    uint8_t  channel;        /* 1..13 */
    uint16_t beacon_int;     /* TU */
} ap_cfg_t;

typedef struct {
    int      subtype_fc;     /* FC_* value, or -1 if not a mgmt frame we handle */
    uint8_t  da[6], sa[6], bssid[6];
    /* probe request */
    int      ssid_present;
    uint8_t  ssid_len;
    uint8_t  ssid[AP_SSID_MAX];
    /* authentication */
    uint16_t auth_alg, auth_seq;
    /* (re)association request */
    uint16_t capab, listen_int;
    uint8_t  rates[AP_RATES_MAX];
    uint8_t  n_rates;
} ap_rx_t;

/* Parse a received management frame. Returns 0 if the frame is well-formed
 * enough to act on, -1 otherwise (drop it). Never reads past len. */
int ap_parse(const uint8_t *f, size_t len, ap_rx_t *out);

/* 1 if a probe request should be answered (wildcard or our SSID). */
int ap_probe_matches(const ap_cfg_t *c, const ap_rx_t *rx);

/* Builders return the frame length, or 0 if cap is too small. */
size_t ap_build_beacon_head(const ap_cfg_t *c, uint8_t *out, size_t cap);
size_t ap_build_beacon_tail(const ap_cfg_t *c, uint8_t *out, size_t cap);
size_t ap_build_probe_resp(const ap_cfg_t *c, const uint8_t *da, uint8_t *out, size_t cap);
size_t ap_build_auth_resp(const ap_cfg_t *c, const uint8_t *da, uint16_t status, uint8_t *out, size_t cap);
size_t ap_build_assoc_resp(const ap_cfg_t *c, const uint8_t *da, int reassoc, uint16_t status,
                           uint16_t aid, uint8_t *out, size_t cap);
size_t ap_build_deauth(const ap_cfg_t *c, const uint8_t *da, uint16_t reason, uint8_t *out, size_t cap);
