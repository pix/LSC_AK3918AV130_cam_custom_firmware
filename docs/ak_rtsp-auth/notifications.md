# Push notifications (ntfy, Telegram, Pushover, Discord, Gotify)

Integrations > Push notifications. On motion, sound, a tamper alert, a line crossing or loitering (see `motion.md`) the camera sends a message with the
current snapshot to your phone, and optionally a short video clip right after it. Off by default.

| Service | What you enter | Picture | Video clip |
|---|---|---|---|
| ntfy | topic URL (`https://ntfy.sh/a-long-random-topic` or your own server), optional access token (`tk_...`) or `user:password` | attachment (`PUT`, `Filename: snapshot.jpg`, `Title`, `Tags`, `Message`) | yes, second message `clip.mp4`, priority low |
| Telegram | bot token (from @BotFather), chat id (or `@channel`) | `sendPhoto` with caption | yes, `sendVideo` (silent, `supports_streaming`) |
| Pushover | application token, user key | `attachment` | **no** - Pushover only takes images, it always gets the snapshot |
| Discord | a channel webhook URL, **or** a bot token + channel id | `files[0]` with `payload_json` | yes, second message (silent flag) |
| Gotify | server URL, application token, priority 0-10 | **no attachments** - text only; with "Public picture URL" set, the message is Markdown with `![snapshot](URL)` | no |

Message: title `<Title or camera name>: motion|sound|tamper alert|line crossed|loitering|test`, text `Motion detected at 2026-11-22 18:04:05`
(sound: level in dBFS; tamper: lens covered / camera moved). The picture is the same JPEG as `/snapshot.jpg`
(640x360 sub-stream), taken when the notification is sent - normally within a fraction of a second of the event.

Discord: the webhook mode needs no bot (Channel settings > Integrations > Webhooks); mentions in the text are
disabled. The bot mode posts to `https://discord.com/api/v10/channels/<id>/messages` with `Authorization: Bot`.
Gotify: the picture URL must be reachable by the Gotify clients without the camera login (e.g. a reverse proxy
or a public snapshot path you run yourself); the camera never uploads anything to Gotify.

## Settings (`/api/config`, port 8091; saved in `ak_rtsp_ext`, so also in the settings backup)

| Name | Default | Meaning |
|---|---|---|
| `notify.enabled` | off | events notify (the test button works while off) |
| `notify.service` | ntfy | `ntfy`, `telegram`, `pushover`, `discord`, `gotify` |
| `notify.url` | | ntfy topic URL; Discord webhook URL; Gotify server; for Telegram / Pushover / Discord bot an optional API base (`https://api.telegram.org`, `https://api.pushover.net`, `https://discord.com`). **Secret** (holds the ntfy topic / webhook token): shown as `(set)`, never logged |
| `notify.token` | | ntfy access token or `user:password`; Telegram bot token; Pushover app token; Discord bot token (empty = webhook); Gotify app token. **Secret** |
| `notify.user` | | Telegram chat id, Pushover user key, Discord channel id (bot) |
| `notify.events` | motion | any of `motion,sound,tamper,line,loiter` |
| `notify.cooldown` | 60 | seconds after a notification before the same event kind notifies again (0-86400) |
| `notify.max`, `notify.window_min` | 10, 60 | at most `max` notifications per window (1-1000 per 1-1440 min); see below |
| `notify.snapshot` | on | attach the picture |
| `notify.clip`, `notify.clip_s` | off, 5 | video clip of 3-10 s |
| `notify.title` | (camera name) | title prefix, up to 99 characters (UTF-8 is fine) |
| `notify.priority` | 5 | Gotify priority 0-10 |
| `notify.image_url` | | Gotify only: a public picture URL shown as a Markdown image |

`POST /api/notify/test` (X-Requested-With: lsc) queues a test notification with the saved settings and returns
at once (202; 409 with the reason when something is missing or the queue is full). `GET /api/notify`:

```json
{"enabled":true,"service":"ntfy","server":"ntfy.sh","events":"motion,sound","state":"ok","error":"","last_ok":"...",
 "sent":12,"failed":1,"queued":0,"dropped_queue":0,"dropped_cooldown":7,"suppressed":0,"suppressed_total":3,
 "summaries":1,"window_sent":2,"window_max":10,"window_min":60,"window_left_s":1534,
 "clip":true,"clips":4,"clip_failed":0,"last_clip_bytes":912345,"last_clip_ms":5066,"clip_note":"...","log":["18:04:06 ..."]}
```
`state` is the result of the last message (`idle`, `sending`, `ok`, `error`); `error` is the last error with its
time and stays until the next error. `server` is only the host name (an ntfy topic or webhook token in the URL
is never shown). `suppressed` counts the current window, `suppressed_total` all of them.

## Limits (a busy scene cannot flood your phone or the camera)

- **Cooldown** per event kind (`notify.cooldown`): repeats inside it are dropped (`dropped_cooldown`).
- **Rate limit**: the first notification opens a window of `notify.window_min`; after `notify.max` in it, further
  events are dropped and counted (`suppressed`); when the window ends, **one** summary message
  "N notifications suppressed (limit M per W min)" is sent, and the next event opens a new window.
- **Queue**: 4 jobs waiting + 1 being sent; events beyond that are dropped (`dropped_queue`). Detectors, MQTT,
  webhook, recorder and the web page never wait for a notification.
- **Retries**: at most 3 attempts per message - after a network error before the request was sent, HTTP 5xx or
  429 (waits 3 s / 10 s, or the server's `Retry-After` / Discord's `retry_after`; asked to wait more than 30 s
  -> gives up). No retry after 4xx (bad token, unknown chat ...), an untrusted certificate, or when the request
  was sent but no answer came (the message may already be there).
- **Timeouts**: connect 10 s, TLS handshake 15 s, sending >= 16 KB/s after 30 s, answer 30 s; answers over
  16 KB are cut (only the status and a short JSON are needed).
- **Memory**: per job the JPEG (<= 200 KB, usually 30-60 KB) and, with clips, one 512 KB frame buffer plus the
  clip itself, hard-capped at **2 MB** in RAM; everything is freed after the job, at most one clip exists at a
  time. Sender thread stack 160 KB.

## Video clips

The notification thread subscribes to the live hub (the same H.264 main stream as RTSP / recordings) when the
job starts, sends the picture, then rewinds to the keyframe nearest before the event that is still in the
hub's buffer (`live_rewind`, as the SD recorder's pre-roll) and muxes `notify.clip_s` seconds into an in-RAM
MP4 (fmp4.c, fragmented MP4 with the duration in `mvhd`/`mehd`; H.264 only, no audio - the camera's PCM
audio track is not playable in the phone apps). Pre-roll before the job start only exists when a viewer or the
recorder keeps the hub filled; otherwise the clip starts at the next keyframe (the camera asks the encoder for
one). At the 2 MB cap the clip ends early (main stream at ~2 Mbit/s: ~8 s); the status says
"size cap reached".

The clip is a second, quiet message after the picture (ntfy priority 2, Telegram `disable_notification`,
Discord `SUPPRESS_NOTIFICATIONS`), so the alert itself never waits for the video. If the server refuses the clip
as too large (HTTP 413, e.g. an ntfy server with a small attachment limit) the picture has already been sent;
the camera then skips clips of 3/4 of the refused size or more (sizes vary from event to event) until the
settings change. Discord
webhooks accept 10 MB without boosts, ntfy.sh 15 MB, Telegram bots 50 MB: all above the 2 MB cap.

## HTTPS

Servers are verified with the camera's built-in public roots (`acme_roots.pem`) - checked 2026-11-22:
ntfy.sh -> ISRG Root X2, api.telegram.org -> Go Daddy Root G2 (added for Telegram), api.pushover.net ->
DigiCert Global Root G2, discord.com -> GTS Root R4. A self-hosted ntfy / Gotify with a private CA: upload that
CA as the "Extra trusted CA" (Security > Public certificate (ACME), or `POST /api/acme/ca`); it is shared with
the ACME client. `http://` URLs work too (LAN servers), but tokens then travel in clear text.

## Privacy

Anyone who knows a public ntfy topic name can subscribe to it and see your pictures: use a long random topic or
an access token. Telegram, Pushover and Discord keep the pictures on their servers. Tokens and the URL are
stored on the camera's config partition (0600 `ak_rtsp_ext`) and in settings backups.

## Tests

`tests/notify/run.sh <build> asan|valgrind|plain` (after `tests/host/build_host.sh <build> asan|valgrind`; plain =
a valgrind build run natively, which adds an RSS check of a burst with clips): the camera code
on the PC in its own network namespace against stand-in servers (`tests/notify/mock_push.py`, HTTP and HTTPS with
a test CA uploaded as the extra CA; `push.test` resolves from a private hosts file, nothing reaches the real
services or the LAN). Events are simulated through `host_server`'s FIFO (`root/cfg/sim_events`), so they take
the real path through events.c. Checked: request format of all five services (authentication, multipart fields,
JSON, headers incl. RFC 2047 titles), the JPEGs decode, clips decode with ffprobe (H.264, ~`clip_s`), 2 MB cap,
413 fallback, Discord 429 `retry_after`, 500 retries, bad credentials, server down, untrusted certificate,
slow server (no answer in 30 s) while the web server keeps answering, queue overflow, cooldown, rate limit +
summary, secrets never in the log; the server must end without ASan / UBSan / LeakSanitizer / memcheck reports.
`tests/fuzz/fuzz_notify.c`: HTTP response parser (chunked, lengths, Retry-After), JSON lookup, service answers,
URL / setting validation, header encoding.
