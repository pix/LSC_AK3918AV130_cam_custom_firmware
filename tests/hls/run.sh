#!/bin/bash
# HLS tests on the PC (no camera, no LAN).
#   run.sh <host build dir> <video.h264> <work dir> [valgrind]
# <host build dir> from tests/host/build_host.sh <dir> asan|valgrind.
# 1. unit test (tokens with a fake clock, path parser, ring / playlist rules),
#    under ASan+UBSan or valgrind like the build;
# 2. hls_test.sh against host_server inside run_sandboxed.sh;
# 3. the server log must end with exit 0 and no sanitizer / valgrind report.
set -u
B=$(cd "$1" && pwd); V=$2; W=$3; VG=${4:-}
T=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W"; LOG=$W/server.log
fail=0

mapfile -t CFL < "$B/cflags"; mapfile -t WR < "$B/wraps.flags"
"$(cat "$B/cc")" "${CFL[@]}" -I"$T/../host" "$T/test_hls_unit.c" "$B/lib.a" "${WR[@]}" -lpthread -lm \
  -Wl,--allow-multiple-definition -o "$W/test_hls_unit" || exit 1
if [ -n "$VG" ]; then
  valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,indirect "$W/test_hls_unit" > "$W/unit.txt" 2>&1
else
  ASAN_OPTIONS=detect_leaks=1 "$W/test_hls_unit" > "$W/unit.txt" 2>&1
fi
urc=$?; tail -n 1 "$W/unit.txt"; [ $urc = 0 ] || { grep -v "^PASS" "$W/unit.txt" | head -20; fail=1; }

bash "$T/../host/run_sandboxed.sh" "$B" "$V" "$LOG" ${VG:+valgrind} -- bash "$T/hls_test.sh" "$W/out" ${VG:+slow} | tee "$W/test.txt"
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] || fail=1
rep=$(grep -cE "ERROR: (Address|Leak)Sanitizer|runtime error:|==[0-9]+== +(Invalid|Conditional jump|Use of uninit|Syscall param|Mismatched|Source and destination|Thread [0-9]+:)|definitely lost|indirectly lost" "$LOG")
ex=$(sed -n 's/^\[sandbox\] server exit //p' "$LOG")
echo "server: exit ${ex:-?}, sanitizer/valgrind reports: $rep, producer sessions: $(grep -c '\[hls\] producer started' "$LOG")"
[ "$rep" = 0 ] && [ "${ex:-1}" = 0 ] || { fail=1; grep -E -A12 "ERROR: |runtime error|==[0-9]+== " "$LOG" | head -60; }
[ $fail = 0 ] && echo "HLS TESTS PASSED (${VG:-asan})" || echo "HLS TESTS FAILED (${VG:-asan})"
exit $fail
