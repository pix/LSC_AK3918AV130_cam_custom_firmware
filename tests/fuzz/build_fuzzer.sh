#!/bin/bash
# Build one libFuzzer harness against the host library.
#   build_fuzzer.sh <harness.c> <fuzz build dir (tests/host/build_host.sh <dir> fuzz)> [out binary]
# A harness may #include a firmware .c file to reach its static functions;
# the library's copy of that module is then simply not linked.
set -eu
H=$1; B=$2; OUT=${3:-$B/$(basename "$H" .c)}
T=$(cd "$(dirname "$0")" && pwd)
mapfile -t CFL < "$B/cflags"
mapfile -t WR < "$B/wraps.flags"
S=$(cd "$T/../.." && pwd)/src/ak_rtsp; [ -d "$S" ] || S=$(cd "$T/../.." && pwd)/firmware/src/ak_rtsp
"$(cat "$B/cc")" "${CFL[@]}" -fsanitize=fuzzer,address,undefined -I"$T" -I"$T/../host" -DHAVE_CONFIG_H \
  -I"$S/third_party/speexdsp" -iquote "$S" "$H" "$B/lib.a" "${WR[@]}" -lpthread -lm -o "$OUT" \
  -Wl,--allow-multiple-definition
echo "$OUT"
