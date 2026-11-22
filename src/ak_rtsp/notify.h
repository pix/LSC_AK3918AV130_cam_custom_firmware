#pragma once
/* Push notifications on events (motion, sound, tamper) with a snapshot and
 * an optional short video clip, to one of: ntfy, Telegram, Pushover,
 * Discord (webhook or bot) or Gotify. See docs/notifications.md.
 *
 * Everything runs in one sender thread with a small bounded queue: the
 * events thread and the web server only queue a job (never block).
 * Per-event cooldown, N notifications per window (then one "N suppressed"
 * summary), bounded retries, timeouts on connect / send / response.
 * HTTPS is verified with the built-in public roots (acme_roots.pem) plus
 * the optional extra CA (POST /api/acme/ca). Settings: notify.* (control
 * registry, off by default; tokens are secrets and never logged). */
#include <stddef.h>
#include <time.h>

void notify_init(void);     /* register settings (before control_load_ext_settings) */
void notify_start(void);    /* sender thread */

/* An event started (events.c worker). kind: "motion", "sound", "tamper";
 * level: sound dBFS / tamper reason. Never blocks. */
void notify_event(const char *kind, int level, time_t t);
/* Queue a test notification (POST /api/notify/test). 0 queued, -1 with err. */
int  notify_test(char *err, size_t cap);
/* GET /api/notify */
int  notify_status_json(char *out, size_t cap);

/* ---- internals, exposed for the tests / fuzzer ---- */
enum { NOTIFY_NTFY, NOTIFY_TELEGRAM, NOTIFY_PUSHOVER, NOTIFY_DISCORD, NOTIFY_GOTIFY, NOTIFY_NSERVICES };
typedef struct {
    int status;              /* HTTP status */
    int retry_after_ms;      /* Retry-After header (0 = none) */
    const char *body;        /* NUL-terminated (inside the parsed buffer) */
    size_t body_len;
} notify_resp_t;
/* Parse a (possibly truncated) HTTP response held in buf[0..len); buf must
 * have room for buf[len] = 0. De-chunks in place. 0 ok, -1 not HTTP. */
int notify_parse_response(char *buf, size_t len, notify_resp_t *r);
/* Top-level member key of a JSON object: string (unescaped), number /
 * literal (as text) or, for an array, its first string. 1 found, 0 not. */
int notify_json_get(const char *js, size_t len, const char *key, char *out, size_t cap);
/* Interpret a service's answer: 0 delivered; else err is filled and the
 * result is -1 refused (bad token, ...), -2 worth retrying (429 / 5xx;
 * *retry_ms = Retry-After or Discord's retry_after, 0 = default delay) or
 * -3 too large (413). */
int notify_result(int service, const notify_resp_t *r, int *retry_ms, char *err, size_t cap);
