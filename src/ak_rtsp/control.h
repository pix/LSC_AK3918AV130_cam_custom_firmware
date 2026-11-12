#pragma once
#include <stddef.h>

/* TCP tuning/debug control server (port 8091).
 *
 * Text protocol (newline-delimited), one client at a time:
 *   LIST                  -> one "name=value" line per known param, then "."
 *   GET <param>           -> "<param>=<value>" or "ERR ..."
 *   SET <param> <value>   -> "OK <param>=<value>" or "ERR ..."
 * Async log lines (tee'd from stdout) are pushed on the same connection
 * prefixed "LOG " whenever they occur, interleaved with command responses —
 * a client tells them apart by the "LOG " prefix, not by strict
 * request/response framing.
 *
 * See docs/serial_console_and_camera_tuning.md for the full
 * parameter list and the PC-side WinUI3 tuning tab that talks this protocol.
 *
 * Call control_start() as the very first thing in main() (before any other
 * printf-producing init) so the stdout tee captures the full boot log, not
 * just ae/night's own logging. control_stop() on shutdown. */

int  control_start(void);
void control_stop(void);
/* fd of the SD-card copy of the log (test mode), or -1. Async-signal-safe. */
int  control_sd_log_fd(void);
/* Last fatal signal of ak_rtsp, written by main.c's handler: "time=<unix> FATAL: ..." */
#define LAST_CRASH_FILE "/etc/config/ak_rtsp_lastcrash"

/* Port 8091 now requires "AUTH <user> <password>" (same login as RTSP/web)
 * before any other command; LOG lines are only pushed after AUTH. */

/* Run one LIST/GET/SET line on behalf of an already-authenticated caller
 * (the web admin page). Output ("name=value" / "OK ..." / "ERR ...", one
 * per line) goes into out. Returns bytes written. */
int  control_exec(const char *line, char *out, size_t cap);

/* Replay /etc/config/ak_rtsp_settings (call once AE/night/ISP are up). */
void control_load_settings(void);
/* Delete saved settings (defaults apply after the next reboot). */
void control_reset_settings(void);
/* Read a CTRL_PERSIST value directly from the saved file (before
 * control_load_settings() runs). 0 = found. */
int  control_saved_get(const char *name, char *out, size_t cap);

/* ---- module settings registry ----
 * Modules register their own settings; they then work through LIST/GET/SET
 * (port 8091 and /api/set), /api/config and are persisted (CTRL_PERSIST) to
 * a separate 0600 file that "reset settings" leaves alone. Register before
 * control_load_ext_settings(). get() writes the current value; set()
 * validates and applies, returning 0 or -1 with *err. */
#define CTRL_PERSIST 1
#define CTRL_SECRET  2          /* shown as "(set)" outside the camera */
typedef struct {
    const char *name;
    int (*get)(char *out, size_t cap);
    int (*set)(const char *value, const char **err);
    unsigned flags;
} ctrl_param_t;
int  control_register(const ctrl_param_t *p);
void control_load_ext_settings(void);
/* Direct access (values may contain spaces). */
int  control_set_value(const char *name, const char *value, const char **err);
int  control_get_value(const char *name, char *out, size_t cap, int reveal_secret);
/* {"name":"value",...} for registered names starting with prefix (secrets masked). */
int  control_params_json(const char *prefix, char *out, size_t cap);
/* Re-apply picture sliders and extra image settings after an ISP reload. */
void control_reapply_picture(void);
