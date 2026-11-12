#pragma once
#include <sys/socket.h>
#include "conn.h"

#define RTSP_MAX_CLIENTS 4   /* RTSP + RTSPS sessions at once (Wi-Fi is the limit) */

/* Serve an accepted RTSP (tls=0) or RTSPS (tls=1) connection in its own
 * thread; each session reads frames from the live hub (live.c) with its
 * own cursor. Returns 0, or -1 (socket closed) when RTSP_MAX_CLIENTS
 * sessions are already running. */
int rtsp_start_client(int fd, int tls, struct sockaddr_storage *addr, socklen_t len);

/* Move every pending encoder frame into the live hub (call every ~20 ms
 * from the main loop; keeps the encoder ring from wrapping). */
void drain_venc_ring(void);

/* Number of RTSP/RTSPS sessions (health panel). */
int rtsp_client_connected(void);

/* Register RTSP settings (audio.codec). Call before loading settings. */
void rtsp_register_params(void);
/* Encoder output over the last ~2 GOPs, kbps (0 until measured). */
int  rtsp_measured_kbps(void);
