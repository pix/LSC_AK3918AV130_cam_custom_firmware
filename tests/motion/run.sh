#!/bin/bash
# Host tests for the motion decision (md_logic.c), no camera needed:
#   tests/motion/run.sh [frames]       (default 40000 random frames per source)
# 1. unit test with ASan + UBSan (regression against the original md.c decision,
#    synthetic scenes for every option, parsers, cost)
# 2. the same under valgrind (-O0, shorter run)
# 3. md_replay on a generated hardware log (an object crossing a line)
# Build output goes to $OUT (default: a new temporary directory).
set -eu
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/../.." && pwd)
S=$R/src/ak_rtsp; [ -d "$S" ] || S=$R/firmware/src/ak_rtsp
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
CF=(-std=gnu99 -Wall -Wextra -Werror -ffp-contract=off -iquote "$S")
SAN=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined)

echo "== unit test (ASan + UBSan)"
gcc "${CF[@]}" "${SAN[@]}" "$T/test_md_logic.c" "$S/md_logic.c" -o "$OUT/test_asan"
"$OUT/test_asan" "${1:-40000}"

echo "== unit test (valgrind)"
gcc "${CF[@]}" -O0 -g "$T/test_md_logic.c" "$S/md_logic.c" -o "$OUT/test_vg"
valgrind -q --error-exitcode=9 --leak-check=full "$OUT/test_vg" 3000 | tail -1

echo "== replay"
gcc "${CF[@]}" "${SAN[@]}" "$T/md_replay.c" "$S/md_logic.c" -o "$OUT/md_replay"
"$OUT/test_asan" --write-log "$OUT/cross.log"
"$OUT/md_replay" "$OUT/cross.log" line=500,0,500,1000 hold=2 | tee "$OUT/replay.txt"
grep -q "line crossed by track [0-9]* toward the left" "$OUT/replay.txt"
grep -q "motion ON" "$OUT/replay.txt"
"$OUT/md_replay" "$OUT/cross.log" line=500,0,500,1000 line_dir=right > "$OUT/replay2.txt"
! grep -q "line crossed" "$OUT/replay2.txt"
echo "motion tests: all passed ($OUT)"
