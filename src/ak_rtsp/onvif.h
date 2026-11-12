#pragma once
/* ONVIF Profile S (subset) for NVRs and Home Assistant:
 *  - WS-Discovery on UDP 3702 (Hello at start, answers Probe)
 *  - Device service  /onvif/device_service  (info, capabilities, services,
 *    scopes, network interfaces, time, hostname, reboot)
 *  - Media service   /onvif/media_service   (profile, sources, encoder
 *    configurations, stream URI, snapshot URI)
 *  - Events service  /onvif/events_service  (PullPoint: motion and sound)
 * Login: WS-Security UsernameToken (digest or text) or HTTP Digest; time and
 * capability queries are allowed without login as the spec requires.
 * Setting: onvif.enabled (default on). */
#include <stddef.h>
#include <stdint.h>
#include "conn.h"

void onvif_init(void);          /* register settings */
void onvif_start(void);         /* discovery thread */
/* Handles POST /onvif/... (body = SOAP). host = request Host header,
 * authz = HTTP Authorization header. Returns 1 if the connection was handed
 * to another thread (PullMessages long poll), 0 otherwise. */
struct sockaddr;
/* peer: for the login failure backoff; is_tls: PasswordText is only accepted
 * over TLS; *penalty set to 1 on a failed login (the caller delays). */
int  onvif_handle(conn_t *cn, const char *path, const char *body, const char *host, const char *authz,
                  const struct sockaddr *peer, int is_tls, int *penalty);
/* Motion / sound state changes for PullPoint subscribers (events.c). */
void onvif_event(const char *kind, int on);
/* Hourly system state for HA sensors: temperature (0.1 C, -9999 unknown), SD card
 * present, free/total space (KB); fires StorageFailure and LastReboot on change. */
void onvif_sys_state(int temp_c10, int sd, uint64_t free_kb, uint64_t total_kb);
/* Recording started (1) or stopped (0); RecordingConfig/JobState. */
void onvif_recording(int on);
