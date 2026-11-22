#!/bin/bash
# Push notification tests (notify.c) against stand-in servers, on the PC.
#   tests/host/build_host.sh <build dir> asan|valgrind
#   tests/notify/run.sh <build dir> [asan|valgrind|plain] [video.h264]
# Runs host_server in tests/host/run_sandboxed.sh (own network namespace:
# nothing reaches the real ntfy / Telegram / Pushover / Discord / Gotify or
# the LAN; names resolve only from a private hosts file), then suite.sh.
# asan: an asan build; valgrind: a valgrind build under memcheck; plain: a
# valgrind build run natively (adds the memory-growth check of a clip burst,
# which ASan's quarantine / valgrind would distort).
# Exit status 0 only if every check passed AND the server ended without any
# ASan / UBSan / LeakSanitizer or valgrind memcheck report.
set -u
B=$(cd "$1" && pwd); MODE=${2:-asan}; VG=""; [ "$MODE" = valgrind ] && VG=valgrind
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/../.." && pwd)
W=$B/notify_work
rm -rf "$W"; mkdir -p "$W/pki"
V=${3:-$W/video.h264}
command -v ffprobe >/dev/null || { echo "needs ffmpeg/ffprobe"; exit 2; }

# test video: 15 fps, AUD NALs, ~2 Mbit/s (a 10 s clip passes the 2 MB cap)
if [ ! -f "$V" ]; then
  ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=15 -t 12 -vf noise=alls=12:allf=t \
         -c:v libx264 -preset veryfast -b:v 2M -maxrate 2M -bufsize 2M -g 30 -bf 0 -profile:v main \
         -bsf:v h264_metadata=aud=insert -f h264 "$V" || exit 2
fi
# test CA (uploaded as the extra CA) + server certificate for push.test / 127.0.0.1
P=$W/pki
openssl ecparam -name prime256v1 -genkey -noout -out "$P/ca.key" 2>/dev/null
openssl req -x509 -new -key "$P/ca.key" -subj "/CN=notify test CA" -days 30 -out "$P/ca.crt" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out "$P/srv.key" 2>/dev/null
openssl req -new -key "$P/srv.key" -subj "/CN=push.test" -out "$P/srv.csr" 2>/dev/null
printf 'subjectAltName=DNS:push.test,IP:127.0.0.1\nextendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n' > "$P/ext"
openssl x509 -req -in "$P/srv.csr" -CA "$P/ca.crt" -CAkey "$P/ca.key" -CAcreateserial -days 30 -extfile "$P/ext" -out "$P/srv.crt" 2>/dev/null
[ -s "$P/srv.crt" ] || { echo "certificate generation failed"; exit 2; }

LOG=$W/server.log
rm -f "$B/root/cfg/ak_rtsp_ext" "$B/root/cfg/ak_rtsp_acme_ca.crt"    # module settings + extra CA from earlier runs
# one glibc artifact (see valgrind.supp); run_sandboxed.sh's valgrind reads VALGRIND_OPTS
[ "$MODE" = valgrind ] && export VALGRIND_OPTS="--suppressions=$T/valgrind.supp"
bash "$R/tests/host/run_sandboxed.sh" "$B" "$V" "$LOG" ${VG:+valgrind} -- bash "$T/suite.sh" "$B" "$W" "$MODE"
rc=$?

echo "== server log verdict ($LOG)"
bad=0
m=$(grep -nE 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:|SUMMARY: (Address|Undefined|Leak)Sanitizer|==[0-9]+== (Invalid|Conditional|Use of uninit|Syscall param|Mismatched|Source and destination|.*are definitely lost|.*are indirectly lost)|definitely lost in|indirectly lost in' "$LOG")
[ -n "$m" ] && { echo "$m" | head -20; echo "FAIL sanitizer / memcheck report"; bad=1; }
ex=$(sed -n 's/^\[sandbox\] server exit //p' "$LOG" | tail -1)
[ "$ex" = 0 ] || { echo "FAIL server exit status '$ex'"; bad=1; }
# secrets of the test accounts must never appear in the camera's log
m=$(grep -nE 'tk_goodtoken|ABC-good_token|azGoodAppToken|uGoodUserKey|goodhook|goodbot|AgoodGotify|camtopic|cam:secret' "$LOG")
[ -n "$m" ] && { echo "$m" | head -5; echo "FAIL a secret appears in the log"; bad=1; }
# valgrind -q prints nothing when clean; any "==pid==" line is a report
if [ "$MODE" = valgrind ] && grep -qE '^==[0-9]+==' "$LOG"; then grep -E '^==[0-9]+==' "$LOG" | head -20; echo "FAIL valgrind output"; bad=1; fi
[ $bad = 0 ] && echo "PASS no sanitizer / memcheck report, clean exit, no secrets in the log"
[ $rc = 0 ] && [ $bad = 0 ] && { echo "notify tests: ALL PASS"; exit 0; }
echo "notify tests: FAILED (suite $rc, log $bad)"
exit 1
