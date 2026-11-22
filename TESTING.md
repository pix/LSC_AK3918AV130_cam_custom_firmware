# Testing, flashing safely, and what we learned about the hardware

Target: LSC Smart Connect indoor camera (Anyka AK3918AV130, GC20C3 sensor), decloud SD card in place.
Rule used throughout: **nothing is flashed before the exact binary has run from the SD card and passed the
checks below.**

## 1. Host tests (no camera)
`bash tests/run_host_tests.sh` builds each suite with ASan + UBSan and runs it (~1-2 min):
SHA-1, AP-mode frames, MD5 + JPEG encoder, Wi-Fi QR parser (quirc), settings backup/restore (round trip and
hostile archives: `../`, symlinks, bad checksums, partial archives…), Wi-Fi manager simulation (wrong password
rolls back, AP fallback), and `tools/maint/flash_image.sh` against a fake MTD (update, rollback, refusals,
write failure → restore). Other harnesses: `tests/host_main.c` (web/auth/settings server on the host),
`tests/osd/test_osd_render.c` (overlay renderer → PGM files), `tests/aec/run.sh` (echo canceller on simulated
streams: two clocks, late reads, echo path gain, double talk).

### The whole firmware on the PC: sanitizers, valgrind, fuzzing
Everything that does not touch the hardware also runs on the PC: `tests/host/build_host.sh <dir> asan|valgrind|fuzz`
builds the web server, RTSP, ONVIF, TLS/ACME, MQTT, events, notifications, recording, HLS/cast, motion logic,
backup… with the hardware (sensor, encoder, ISP, audio, IR, LEDs) stubbed by `tests/host/hw_stubs.c`: frames
come from a test H.264 file, audio is a synthetic camera-like stream. Paths (`/etc/config`, the SD card) are
redirected into the build directory, and `system()` / `popen()` / `reboot()` / `mount()` are wrapped so nothing
touches the PC.
`tests/host/run_sandboxed.sh <dir> <video.h264> <log> [valgrind] -- <command>` starts that server in
unprivileged namespaces (own network: the camera is 10.99.0.1, own process tree, tmpfs SD card), runs the test
command against it, stops it with SIGTERM and appends the ASan/LSan/valgrind verdict to the log.

| Suite | What it covers |
|---|---|
| `tests/host/sweep.sh <fixtures> <venv>` | every endpoint and setting (valid and invalid), uploads, backup/restore, RTSP/RTSPS TCP+UDP, WebSocket live view + talk-back, ONVIF + Home Assistant checks, MQTT, recordings |
| `tests/host/netfuzz.py`, `rtsp_edge.py` | malformed HTTP/RTSP for minutes (server must survive); RTSP pipelining, split requests, garbage then a valid request |
| `tests/tls/acme/run.sh` (`VALGRIND=1`) | own-certificate checks (wrong key, chain order, expired, CA, key types and sizes…), certificate selection by SNI/IP, ACME against Pebble and mock DNS APIs |
| `tests/motion/run.sh` | `md_logic.c` against the old decision, synthetic scenes for every option, line crossing / loitering replay |
| `tests/notify/run.sh <dir> asan\|valgrind` | ntfy / Telegram / Pushover / Discord / Gotify against local stand-ins: limits, pictures, clips, failures |
| `tests/hls/run.sh <dir> <video> <work> [valgrind]` | playlists/segments, tokens, concurrent clients, HTTPS, casting to a fake Chromecast (start-up time, live edge) |
| `tests/onvif/profile_g.sh <dir> <video> <venv> [valgrind]` | Profile G search/replay with onvif-zeep-async, RTSP replay with ffmpeg |
| `tests/fuzz/*.c` + `build_fuzzer.sh` | libFuzzer harnesses for every parser: HTTP requests and handlers, Digest, RTSP, ONVIF (S and G), MQTT, PEM, backup archives, fMP4 muxer/reader, HLS paths, notifications, motion settings, QR, WAV, OSD… |

Inputs: a test clip (`ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=15 -t 12 -c:v libx264 -g 30 -bf 0
-bsf:v h264_metadata=aud=insert -f h264 video.h264`), certificates from `tests/tls/acme/gen_fixtures.sh <dir>`,
and a Python venv with `onvif-zeep-async` and `websockets`.
Expected on the PC: `POST /api/npu/test` answers 500 (no NPU in the stubs). Bugs found this way (heap overflow
in the certificate upload, stale host names, RTSP pipelining, several integer overflows in parsers, a settings
save that could drop values) are fixed; third-party SpeexDSP and quirc still have UBSan arithmetic reports.

## 2. Trying a build on the camera without flashing
1. Append `tools/dev/sd_hook_append.sh` to the decloud SD card's `hack.sh` once.
2. `CAMERA_IP=… tools/dev/deploy_test.sh src/ak_rtsp/ak_rtsp v1` pushes the binary to `/tmp/sd/maint/`,
   selects it with `USE_TEST_BINARY`, reboots, waits for the web page. Strip it first (`llvm-strip`).
3. Back to the flashed build: delete `/tmp/sd/maint/USE_TEST_BINARY` (or pull the SD card) and reboot.

`tools/dev/camsh.py` runs a command on the SD card's root telnet (port 24). Logs: `/tmp/sd/maint/ak_rtsp.log`,
kernel log `/tmp/sd/maint/dmesg.log`, last crash in System > Health.

Per-build checks: 401 without login / 200 with it; snapshot; RTSP main + `/sub`, TCP and UDP, full frame rate
(15 fps day, 10 fps night) for 20 s; RTSPS on 322; settings survive a reboot; uptime keeps rising for 5 min;
`tests/onvif/onviftest.py` and `ha_compat_test.py` (what Home Assistant's ONVIF integration does) pass; the
web page loads in Firefox and Chromium without failed requests.
Feature scripts live under `tests/` (recording, events/MQTT over TLS, TLS/RTSPS/talk-back, ISP adjustments);
they take the camera address as first argument or `CAMERA_IP`.

## 3. Flashing and rollback
- Only the APP partition (`mtd7`) is ever written, only by `tools/maint/flash_image.sh`, and only between images
  listed in `tools/maint/KNOWN_IMAGES` (md5). It checks the flash layout, backs up the current APP to
  `mtd7_before_<md5>.bin` on the SD card, writes, reads back, and restores the backup if anything differs.
  Final line: `FLASH_OK | FLASH_ALREADY | FLASH_RESTORED_PREVIOUS | FLASH_FAILED`.
- Image = the decloud build's APP tree with only `bin/ak_rtsp` replaced (`diff -rq` before every flash),
  `mksquashfs -comp gzip -b 131072 -all-root`, round-trip `unsquashfs` compare, `tools/flash_scripts/pad_build.sh`.
- Rollback: `sh /tmp/sd/maint/flash_image.sh mtd7_before_<md5>.bin`. A bad flashed build is still recoverable
  through the SD card (port 24 + `USE_TEST_BINARY`). Power loss during the ~6 s write needs UART + U-Boot and
  your stock dump (README, step 0).
- Never: kill `/usr/bin/watchdog` (hardware watchdog resets the board), leave jobs attached to the telnet
  session (use `setsid`), `drop_caches` while the APP is rewritten.

## 4. Hardware facts that cost time (keep these in mind when changing code)
- **Clocks:** `CLOCK_MONOTONIC`/`BOOTTIME` are broken with musl on this kernel; `monotime.c` uses the raw syscall.
- **ISP device** `/dev/isp-param-0` opens once: everything that talks to the ISP (OSD, AE, MD, adjustments) runs
  inside ak_rtsp. Read-only dumps: `GET /api/isp/ae`, `GET /api/isp/get?m=nr1|nr2|uvnr|3dnr|expo`.
- **Crash guards:** risky driver steps (3D-NR reference frames, overlay, second encoder channel) leave a marker in
  `/etc/config` while they start; a start that never completes disables the feature next boot instead of
  looping (cleared once the feature has run, or when it is re-selected).
- **Descriptor leaks:** daemons started with `system()` (ntpd, udhcpc) inherited `/dev/video*`, `/dev/venc*`
  and the listening sockets, so after any crash restarts failed until a reboot. `sys_cloexec_all()` first.
- **Core dumps are empty** on this kernel; the fatal-signal handler logs signal, fault address, PC and LR
  (`llvm-addr2line` on the unstripped build) to the log and to `/etc/config/ak_rtsp_lastcrash`.
- **Speaker:** the public `ak_ioctls.h` names have read/write swapped; source must be set to 4 (DAC) and
  start-playing needs a buffer or the kernel crashes. Mic gain: capture ioctl 0x40045030 (0..7).
- **Hardware motion statistics** only update while temporal NR has reference frames (mmap of the ISP device
  before STREAMON).
- **Picture adjustments** follow stock `anyka_ipc` (`isp_set_effect`): brightness moves the AE target (+15-entry
  table, stable range scaled); sharpness rescales block 0 and the 16 per-gain blocks of the sharp blob; WDR blends
  the tone curves with a straight line. Under artificial light, darkening stops at one anti-flicker step
  (as stock); anti-flicker off gives the full range. Measure with `tests/isp/adjust_measure.py`.
- **Noise reduction** (no stock control): NR2/UVNR weight tables are Gaussians `1023*exp(-256 i²/σ)` per gain
  block; `image.nr` scales σ (1/8×..8×) and 0 restores the calibration byte for byte.
- **Day/night:** the stock soft photosensor (`sdn.c`, EV + non-IR ratio) is ported; night runs at 10 fps.
- **Encoder:** the sub stream needs `rc_flag 0` and matching MMA/MVV flags in QUERY_DMA (a mismatch crashed the
  main channel); RTCP sender reports wait 3 s so ffmpeg's timestamps do not go backwards over UDP. Smart
  encoding (`video.smart`) must use the same mode in QUERY_DMA and CREATE_ENC (LTR needs two more reference
  buffers); keyframe on demand is ioctl 0x400456f3 (stock `ak_venc_request_idr`).
- **Frame rate** can change while streaming (`video.idle_fps`): same ISP calls as night mode, VTS = 34260/fps.
  RTP, the SD recorder and the web view all use per-frame timestamps, so files play at the right speed.
  Bitrate falls almost linearly (busy scene: 15 fps 3.0 Mbit/s, 5 fps 0.94).
- **H.265** is not available: the encoder IP is Allegro's (Xilinx VCU profile words), but `ak_venc_adapter.ko`
  reads an IP signature at 0x2140000C and only the AVC-only one gives a working encoder
  (`docs/ak_rtsp-auth/re/h265.md`).
- **Audio loopback** (`/dev/pcmC0D0l`, the echo canceller's speaker reference) carries 256-sample windows copied
  every 128 samples: keep the first half of each. The speaker reaches the mic 32-37 dB above the loopback
  level, so the fixed-point SpeexDSP filter needs pre-scaling; at high volume the mic saturates, hence voice
  ducking while talk-back plays. A phone near the camera closes a loop no camera-side canceller sees
  (camera speaker → phone mic): the talk-back page sends close-talk sound only (no browser AGC, gate).
- **TLS on a fresh install** (found installing a second, stock camera with the Decloud Wizard): with musl,
  mbedTLS read `/dev/random`, which blocks on this 4.4 kernel (54 bits of entropy: HTTPS stuck at
  "Starting"), so a `getrandom()` entropy source is used. mbedTLS rejects certificates with an unknown
  critical extension, so the camera's own CA (critical NameConstraints) is parsed with a callback accepting
  just that extension. With no DNS SAN, OpenSSL/GnuTLS/NSS check the server certificate's CN against the
  DNS constraints, so an address-only certificate has no CN. Checked with openssl s_client, curl,
  gnutls-cli, ffmpeg RTSPS (`-tls_verify 1`) and Python `ssl` against the camera CA.
- **Hardware motion statistics** were seen frozen in bright daylight (identical across reboots); the software
  source works there.

## 5. Security model (tested with the scripts named)
- HTTP/RTSP/ONVIF Digest: uri must match, qop=auth, per-client nc replay window (nonce + cnonce), 5 min nonces
  renewed with `stale=true` (RTSP keeps longer ones for old clients). Per-IP login backoff shared by every
  front end (5 failures → 30 s, doubling to 15 min).
- Host allow-list (IP, DHCP name, certificate names, `web.hosts`) against DNS rebinding; `X-Requested-With` on
  state-changing and token requests; ONVIF refuses browser-style requests; port 8091 is loopback-only.
- HTTPS/RTSPS with an on-camera CA (name-constrained to private names/addresses) or your own certificate;
  reverse proxies via `web.proxies` (X-Forwarded-For / PROXY protocol), see `docs/ak_rtsp-auth/traefik.md`.
- Connections: 6 served + 6 queued per address, 16 total; slow clients dropped (absolute read deadlines).
- Development terminal: off by default; one-time token + same-origin WebSocket; the shell gets no camera
  descriptors.

## 6. Known limits
- Hardware JPEG is not used (stock only encodes 640×360 that way; snapshots are software JPEG of the sub frame).
- Wi-Fi AP fallback is disabled: the atbm driver lacks the management-frame support hostapd needs.
- Firmware update from the web page is intentionally not provided; flash over the SD card as above.
- RTSP over UDP: ffmpeg sometimes warns once about a non-monotonic audio timestamp (one 32 ms packet) when the
  first RTCP Sender Report arrives; it corrects it and plays on. TCP is not affected.
- The sensor is 1920×1080 at most (GC20C3): there is no 2K mode to unlock.
- Casting: the receiver keeps about 3 HLS segments of delay (6-8 s at the default GOP); use the live view or RTSP
  for real time.
