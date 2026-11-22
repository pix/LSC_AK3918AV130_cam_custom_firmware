#!/bin/bash
# ONVIF Profile G end to end on the PC: host_server (an asan or valgrind build of
# tests/host/build_host.sh) in the sandbox, then profile_g_test.py (search /
# recording / replay services with onvif-zeep-async) and replay_test.py (RTSP
# replay with ffmpeg / ffprobe and a raw RTSP client), then the sanitizer verdict.
#   profile_g.sh <build dir> <video.h264> <venv> [valgrind]
# Exit status: 0 only if every check passed and the server log has no report.
set -u
B=$(cd "$1" && pwd); V=$2; VENV=$3; VG=${4:-}
T=$(cd "$(dirname "$0")/.." && pwd)
LOG=$B/profile_g_server.log
bash "$T/host/run_sandboxed.sh" "$B" "$V" "$LOG" $VG -- bash -c '
  "$0/bin/python" -I "$1/onvif/profile_g_test.py" "$CAM" admin admin "$2/root/sd/rec"; a=$?
  "$0/bin/python" -I "$1/onvif/replay_test.py" "$CAM" admin admin "$2/root/sd/rec"; b=$?
  exit $((a + b))' "$VENV" "$T" "$B"
rc=$?
echo "== server log: $LOG ($(grep -c . "$LOG") lines), $(tail -1 "$LOG")"
if grep -qE "ERROR: (Address|Leak)Sanitizer|runtime error:|==[0-9]+== (Invalid|Conditional|Use of|Syscall param|Mismatched|Source and)|definitely lost in|indirectly lost in|server exit [1-9]" "$LOG"; then
  echo "SANITIZER / VALGRIND REPORTS:"
  grep -nE "ERROR: |runtime error:|==[0-9]+== |server exit" "$LOG" | head -40
  rc=$((rc + 100))
else
  echo "no sanitizer / valgrind reports"
fi
exit $rc
