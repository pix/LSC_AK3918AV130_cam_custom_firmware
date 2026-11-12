#pragma once
/* Web admin page on port 80 (HTTP Digest auth, same login as RTSP/8091):
 *   GET  /                 admin page
 *   GET  /snapshot.jpg     640x360 JPEG from the sub channel
 *   GET  /api/status       JSON status
 *   GET  /api/params       LIST output (name=value lines)
 *   POST /api/set          name=..&value=..       -> SET (persisted)
 *   POST /api/credentials  current=..&user=..&pass=..
 *   POST /api/reboot
 *   POST /api/reset-settings                       -> delete saved + reboot
 * POSTs require the header "X-Requested-With: lsc" (CSRF guard: a foreign
 * page can't add custom headers to a cross-origin request without a CORS
 * preflight, which this server never approves). */
#ifndef WEB_PORT
#define WEB_PORT 80
#endif
#ifndef WEB_HTTPS_PORT
#define WEB_HTTPS_PORT 443
#endif

int web_start(void);
void web_register_params(void);   /* web.hosts, web.proxies */
#include <stdint.h>
int  web_is_proxy(uint32_t ip_net);   /* address (network order) is listed in web.proxies */
