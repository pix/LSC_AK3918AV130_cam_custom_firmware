# HLS stream and casting to a Chromecast

The camera can serve its main stream (1080p H.264) as HTTP Live Streaming, for clients that cannot or should not
use RTSP: Safari (macOS, iPhone, iPad) natively, other browsers through hls.js, VLC, mpv, ffmpeg, smart TVs and
Chromecast. It reuses the fragmented-MP4 muxer of the SD recordings (`fmp4.c`); segments are kept in RAM only.

Off by default. Image > Video quality > **HLS stream** (setting `hls.enabled`, applies at once). The live view
then shows **Open HLS stream URL** (creates an address and shows it as a link with a copy button) and **Cast…**.

## Addresses and access

    GET /api/hls/token                 (login + X-Requested-With)  -> {"token":"<32 hex>","url":"/hls/<token>/index.m3u8"}
    GET /hls/<token>/index.m3u8        media playlist (no login: the token is the credential)
    GET /hls/<token>/init.mp4          initialisation segment (EXT-X-MAP)
    GET /hls/<token>/seg<N>.m4s        media segments
    GET /hls/<token>/live.mp4          one endless fragmented MP4, ~1 s behind live (see "Low latency: live.mp4")
    GET /api/hls                       (login) state: running, segments, bytes, peak, tokens, mp4_streams, cast_format

- Media players cannot do HTTP Digest reliably, so the URL carries a random 128-bit token instead (like the
  live-view WebSocket token). Compared in constant time against at most 8 live tokens.
- Expiry: a new token must be used within 10 minutes; after that it stays valid while it is used at least every
  2 minutes, and never longer than 24 hours. Creating a 9th token replaces the least recently used one.
  Switching `hls.enabled` off revokes every token at once. Tokens live in RAM (a restart revokes them too).
- Anyone holding the URL can watch while it is valid: share it like a password. These URLs are the only way to
  watch without the login, and only a logged-in request (or a cast started by a logged-in user) creates one.
  A cast URL is bound to the Cast device's address (a copy used from any other machine gets 403) and revoked by
  Stop or by the next cast. Over plain HTTP (casting) the URL and the video cross the LAN unencrypted, as with
  plain RTSP.
- `Access-Control-Allow-Origin: *` on `/hls/` responses only (hls.js pages, the Chromecast receiver);
  `/api/` never sends CORS headers.
- The Host check (DNS rebinding) applies as for every page. The optional HTTP -> HTTPS redirect applies too:
  players follow it, but then they must trust the HTTPS certificate (camera CA installed, or an ACME / own
  certificate). The page builds the URL with its own scheme, so a page opened over HTTPS hands out https URLs.
- Requests are answered outside the web server's global lock: waiting for the first segments or sending a
  segment never holds up the admin page. A segment is copied out of the ring 32 KB at a time under the HLS lock
  (never the live hub's lock), and sent without any lock held.

## Stream layout

- One producer thread subscribes to the live hub (`live.c`) on the first request and unsubscribes 30 s after
  the last one (ring freed). The next request starts a new session: segment numbers continue and the media
  timeline (tfdt) continues forward, so a paused player that comes back sees a jump ahead, not back in time.
- Segments start at a keyframe: a cut is made at the first keyframe at least 1 s after the segment start. With
  the default GOP (venc.gop 30 at 15 fps) segments are 2 s; night mode (10 fps) and the idle frame rate stretch
  a 30-frame GOP to 3-6 s. Very long GOPs: with smart encoding the producer asks for a keyframe after 3 s;
  otherwise a segment is cut without a keyframe at 10 s or 1.5 MB (the playlist then drops
  EXT-X-INDEPENDENT-SEGMENTS while such a segment is listed).
- Inside a segment the muxer writes a moof+mdat fragment every 256 KB (bounds the muxer's buffer).
- Playlist: `EXT-X-VERSION:7`, `EXT-X-TARGETDURATION` = largest rounded EXTINF of the session (constant while the
  GOP duration is; it only grows when the frame rate drops, which players accept), `EXT-X-MEDIA-SEQUENCE`,
  `EXT-X-MAP:URI="init.mp4"`, the newest 4 segments. The first playlist is answered once 2 segments exist
  (or 1 after 5 s; 503 + Retry-After after 9 s without video): cold start ~4 s at the default GOP.
- The ring keeps 5 segments (one more than listed, for clients holding an older playlist), at most 4 MB of
  segments (at least 3 are kept). A client still downloading a segment when it leaves the ring gets a short
  response (it retries or skips ahead; at 2 s segments that client is > 8 s behind).

## Memory and CPU budget

| | default (1.5 Mbit/s, 2 s) | 3 Mbit/s, 2 s | hard bound |
|---|---|---|---|
| completed segments | 5 x 375 KB = 1.9 MB | 5 x 750 KB = 3.75 MB | 4 MB (4.5 MB if 3 x 1.5 MB) |
| segment being built | <= 375 KB | <= 750 KB | 1.5 MB + one fragment |
| muxer fragment buffer | <= 512 KB | <= 512 KB | 1 MB |
| hub frame buffer (pages touched) | ~100 KB | ~150 KB | 512 KB |
| per request | 32 KB copy buffer | | 16 connections max |
| **total while streaming** | **~3 MB** | **~5 MB** | **~7.5 MB** |

Nothing is allocated while nobody watches. `GET /api/hls` reports `peak_bytes` (ring + building + init).
CPU: per frame one copy out of the hub, one NAL scan + copy into the fragment and one copy into the segment
(about 3 x 375 KB/s at 3 Mbit/s); per client one 32 KB copy per send. Over HTTPS each client adds AES-GCM on
375 KB/s. Still to be measured on the camera (expected low single-digit % per HTTP client).

## Audio

Not included: the HLS stream is video only. The camera's microphone is 8 kHz 16-bit PCM and there is no AAC
encoder on it. HLS clients only take AAC / HE-AAC, MP3, AC-3 / E-AC-3 (and, in fMP4, FLAC / Opus / ALAC on recent
Apple players); raw PCM (`ipcm`, `sowt`, `lpcm`) and G.711 are not in that list: hls.js and the Chromecast
receiver refuse such a track (no SourceBuffer for it) and Safari's HLS does not play it. Sound stays available
in the live view (WebSocket), over RTSP and in recordings. A software AAC-LC encoder at 8 kHz mono would make it
possible (a few % CPU), not done here.

## Low-latency HLS

Not implemented. LL-HLS needs blocking playlist reloads (`_HLS_msn` / `_HLS_part`: the server holds each request
open until the next part exists). Every waiting player would then occupy one of the web server's 16 connection
threads (6 per address) for up to a segment duration, the server closes every connection after one response
(no keep-alive / HTTP/2), and Apple expects HTTP/2 for it. Latency today: hls.js and Safari start 3 segments
from the live edge, about 6-8 s at 2 s segments. For low latency use the live view (WebSocket), RTSP, or
live.mp4 below.

## Low latency: live.mp4

`/hls/<token>/live.mp4` (same tokens) is one endless HTTP response holding a fragmented MP4: `ftyp` + `moov`,
then one `moof` + `mdat` per frame. For players that take a plain media URL (the Cast receiver, `<video src>`,
VLC, mpv): they play frames as they arrive instead of holding three segments (the delay is then the player's own start-up buffer; for casting see the catch-up below).

- Starts at the next keyframe produced after the request (not one already kept for other viewers): a player
  never catches up with live, so every frame of pre-roll would stay as delay. The first frame comes within a
  GOP (2 s by default).
- A frame is sent when the next one arrives, with its real duration (the adaptive frame rate changes it): media
  time keeps pace with the wall clock. A client that falls behind is skipped ahead to the newest keyframe (the
  frame before the skip is shown longer), so delay does not build up. Video only, like the HLS stream.
- No `Content-Length`, `Connection: close`, `Cache-Control: no-store`. At most 2 at once (503 after that):
  each holds one of the web server's connection threads. Send timeout 10 s: a reader that stops is dropped.
- Ends when the token expires or is revoked, when `hls.enabled` is switched off, or at shutdown. Does not
  start the HLS segment producer.

## Players

- Safari / iOS: open the URL, or `<video src="...index.m3u8">`.
- Other browsers: hls.js (`new Hls().loadSource(url)`), CORS is allowed.
- VLC, mpv, ffplay: open the URL. `ffmpeg -i URL -c copy out.mp4` records it. ffmpeg prints "Found duplicated
  MOOV Atom" once per playlist reload for any live fMP4 HLS (its own HLS muxer's output too): harmless.
- ffmpeg over HTTPS with the camera CA: its HLS demuxer does not pass `-tls_verify` / `-ca_file` on to segment
  requests; install the camera CA in the system trust store (or use HTTP / an ACME certificate).
- Chromecast from a sender app: content type `application/x-mpegurl`, stream type LIVE,
  `hlsSegmentFormat` / `hlsVideoSegmentFormat` = `fmp4`.

## Casting from the camera (no browser support needed)

**Cast…** under the live view lists the Cast devices found with mDNS (`_googlecast._tcp`, 3 s) and has a field
for an IP address. **Play** makes the camera itself connect to the device and start the stream; **Stop** ends it.

    POST /api/hls/cast            host=<IPv4>[&port=8009]   (login + X-Requested-With)
    POST /api/hls/cast/stop       host=<IPv4>[&port=8009]
    POST /api/hls/cast/discover
    GET  /api/hls/cast            {"state":"idle|discovering|connecting|launching|loading|playing|stopped|error",
                                   "message":..., "device":..., "devices":[{"name","model","host","port"}], "busy":...}

- `cast.c` speaks Cast v2 over TLS to port 8009 (the device's self-signed certificate is not verified, like every
  sender): CONNECT, LAUNCH of the Default Media Receiver (CC1AD845), CONNECT to its transport, LOAD of
  `http://<camera address on that network>/hls/<new token>/live.mp4` (`video/mp4`, LIVE), answers PINGs, reports
  PLAYING / LOAD_FAILED / LAUNCH_ERROR, then closes its connection without CLOSE (the receiver keeps playing, as
  after any sender app exits). Stop asks for the receiver status and stops only the Default Media Receiver session.
- Catch-up (live.mp4): a receiver buffers a few seconds before it starts playing and then plays at normal speed,
  so that buffer stays as delay (about 5 s on an Android TV). For up to 30 s after PLAYING the camera polls the
  receiver's position (GET_STATUS) and compares it with the media time live.mp4 has sent. Above 1.5 s behind it
  sends SEEK to 1 s behind the newest frame; if the receiver refuses it (INVALID_REQUEST) or the delay stays, it
  sets the playback rate to 1.3 until 1 s behind, then back to 1.0 (also when the receiver reports BUFFERING).
  The status line shows the result: `5.2 s behind -> 1.0 s: jumped ahead`. Stop or a new cast cancels the
  catch-up at once (rate back to 1.0 first).
- Format (`hls.cast_format`, the selector next to Play): `mp4` (default) = live.mp4, caught up to about 1 s behind live where the receiver allows it;
  `hls` = the playlist (`application/x-mpegurl`, fMP4 segments), 6-8 s behind. If the device answers the MP4 LOAD
  with LOAD_FAILED or a playback error, the camera loads HLS in the same session by itself (status: "refused the
  low latency stream, trying HLS"); the status line says which format plays.
- HLS start-up: Play starts the HLS producer before connecting to the device, so the first segments are built
  while the receiver app launches (a few seconds on a TV) instead of after its first playlist request (~4-5 s
  saved). The LOAD carries no `currentTime`, so a LIVE stream starts at the live edge, not at the oldest listed
  segment. Delay behind live is then the receiver's own buffer (about 3 segments, 6-8 s at the default GOP; a
  shorter `venc.gop` gives shorter segments and less delay, at some bitrate cost for every stream).
- The token handed to the device works only from that device's address (port 8009 devices; a group's members
  fetch the stream themselves, so group casts get an unbound token) and is revoked by Stop or the next cast.
- Runs in one background thread (one job at a time; a second request gets 503), never on the web thread.
- The device fetches the stream over plain HTTP: with the HTTP -> HTTPS redirect on, the cast is refused with that
  reason (a Chromecast does not trust the camera's own CA).
- The token handed to the device follows the usual expiry (idle 2 min, 24 h at most): a cast stops after 24 h.
- Cast groups (port 32xxx) appear in the list; whether a group plays video depends on its members.

## Tests (PC, no camera)

`tests/hls/run.sh <host build> <video.h264> <work dir> [valgrind]` (build: `tests/host/build_host.sh <dir>
asan|valgrind`): unit test with a fake clock (`test_hls_unit.c`: path parser, token expiry / LRU / revocation,
ring bounds, playlist), then `hls_test.sh` inside `tests/host/run_sandboxed.sh` (token flow, invalid paths,
Host check, cold start while the page stays responsive, 40 s emulated live client checking playlist rules and
every fragment's timeline, ffmpeg 35 s recording, 3 clients + ffmpeg + RTSP + WebSocket viewer together, slow
client cut short, HTTPS with the camera CA, redirect, memory bound, idle stop + restart, revocation, casting
to a fake Chromecast `fake_cast.py` with mDNS). Fuzzer: `tests/fuzz/fuzz_hls.c` (paths, ring, Cast messages, mDNS).
