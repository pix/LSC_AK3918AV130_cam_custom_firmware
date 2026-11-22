#pragma once
/* Cast the HLS stream (hls.c) to a Chromecast / Google TV without a
 * browser: the camera itself speaks the Cast v2 protocol to the device
 * (TLS, port 8009), starts the Default Media Receiver and loads
 * http://<camera address>/hls/<new token>/index.m3u8. Devices are found with
 * mDNS (_googlecast._tcp). Everything runs in one background thread; the web
 * API only starts it and reads the state. Needs hls.enabled. See docs/hls.md.
 *
 *   POST /api/hls/cast           host=<IPv4>[&port=8009]   cast to that device
 *   POST /api/hls/cast/stop      host=<IPv4>[&port=8009]   stop our receiver app there
 *   POST /api/hls/cast/discover                            3 s mDNS search
 *   GET  /api/hls/cast           {"state":..,"message":..,"device":..,"devices":[..]}
 */
#include <stddef.h>

/* sub: the path after "/api/hls/cast" ("", "/stop", "/discover"). Writes a
 * JSON answer into out; returns the HTTP status code. */
int cast_api(int is_post, const char *sub, const char *host, int port, char *out, size_t cap);

/* ---- protocol helpers (exposed for the tests / fuzzer) ---- */
/* Encode a CastMessage (protobuf) with its 4-byte length prefix. Returns the
 * frame length, 0 if it does not fit. */
size_t cast_encode(unsigned char *out, size_t cap, const char *src, const char *dst, const char *ns, const char *json);
/* Decode a CastMessage body (without the length prefix). Strings are copied
 * NUL-terminated (truncated to their buffers); payload points into msg.
 * 0 ok, -1 malformed. */
typedef struct { char src[64], dst[64], ns[96]; const char *payload; size_t plen; } cast_msg_t;
int cast_decode(const unsigned char *msg, size_t len, cast_msg_t *m);
/* JSON (NUL-terminated): string member key of the object at obj; 0 ok. */
int cast_json_str(const char *obj, const char *key, char *out, size_t cap);
/* Receiver status: transportId / sessionId of the running app appid. 0 found. */
int cast_find_app(const char *json, const char *appid, char *transport, size_t tcap, char *session, size_t scap);
/* Media status: playerState of the first entry ("" if none). */
void cast_player_state(const char *json, char *out, size_t cap);
/* Media status, first entry: playerState, currentTime (s), mediaSessionId
 * (-1 if absent). 0 ok, -1 no entry or no valid currentTime. */
int cast_media_status(const char *json, char *state, size_t cap, double *cur, long *msid);
/* mDNS answer -> device fields (name from TXT fn=, model md=, port from SRV,
 * address from an A record or src_ip). 0 if it describes a Cast device. */
int cast_parse_mdns(const unsigned char *p, size_t n, const char *src_ip,
                    char *name, size_t ncap, char *model, size_t mcap, char *ip, size_t icap, int *port);
