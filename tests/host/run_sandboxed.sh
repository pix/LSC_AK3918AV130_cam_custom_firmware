#!/bin/bash
# Run host_server inside unprivileged namespaces (own network with only
# loopback, own process tree), then a test command against it, then stop it
# with SIGTERM so LeakSanitizer / valgrind report.
#   run_sandboxed.sh <build dir> <video.h264> <log> [valgrind] -- <test command...>
# Inside: the camera is $CAM = 10.99.0.1 (also 127.0.0.1; ports 80 443 554 322 8091), login admin/admin.
# Exit status: the test command's; the server log ends with the sanitizer verdict.
set -u
B=$1; V=$2; LOG=$3; shift 3
VG=""; [ "${1:-}" = valgrind ] && { VG=valgrind; shift; }
[ "${1:-}" = -- ] && shift
if [ -z "${AKR_IN_NS:-}" ]; then
  exec unshare --user --map-root-user --net --pid --fork --mount --mount-proc \
       env AKR_IN_NS=1 bash "$0" "$B" "$V" "$LOG" ${VG:+valgrind} -- "$@"
fi
ip link set lo up
ip link add dummy0 type dummy && ip addr add 10.99.0.1/24 dev dummy0 && ip link set dummy0 up   # the "Wi-Fi" address
export CAM=10.99.0.1
ip link set dummy0 multicast on && ip route add 224.0.0.0/4 dev dummy0     # WS-Discovery (ONVIF)
cd "$B"
# an "SD card": recordings need REC_ROOT to be a mount point (record.c sd_present)
mount -t tmpfs -o size=512m tmpfs "$B/root/sd" && mkdir -p "$B/root/sd/maint"
if [ -n "$VG" ]; then
  valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,indirect \
    --show-leak-kinds=definite,indirect --track-origins=yes ./host_server "$V" > "$LOG" 2>&1 &
else
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 \
    ./host_server "$V" > "$LOG" 2>&1 &
fi
SRV=$!
for i in $(seq 1 600); do grep -q "\[host\] ready" "$LOG" 2>/dev/null && break; kill -0 $SRV 2>/dev/null || break; sleep 0.1; done
sleep 1
"$@"; rc=$?
kill -TERM $SRV 2>/dev/null
for i in $(seq 1 300); do kill -0 $SRV 2>/dev/null || break; sleep 0.1; done
kill -0 $SRV 2>/dev/null && { echo "server did not stop: killing" >> "$LOG"; kill -9 $SRV; }
wait $SRV; src=$?
echo "[sandbox] server exit $src" >> "$LOG"
exit $rc
