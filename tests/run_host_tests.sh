#!/bin/bash
# Host test suite (no camera needed). Builds each test with ASan + UBSan and
# runs it. Usage: bash tests/run_host_tests.sh   (from anywhere; needs gcc)
# The netcfg simulation runs real timers and takes about 3-4 minutes.
set -u
R=$(cd "$(dirname "$0")/.." && pwd); S=$R/src/ak_rtsp; Q=$S/third_party/quirc
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
F=(-std=gnu99 -O1 -g -w -fsanitize=address,undefined -I"$S")
fails=0
run() {                     # run <name> <binary args...> -- <cc args...>
    local n=$1; shift; local args=(); while [ "$1" != "--" ]; do args+=("$1"); shift; done; shift
    if ! gcc "${F[@]}" "$@" -o "$O/$n" 2>"$O/$n.err"; then
        echo "FAIL $n: build: $(grep -m1 error "$O/$n.err")"; fails=$((fails + 1)); return
    fi
    local out; out=$(cd "$O" && timeout 600 "./$n" "${args[@]}" 2>&1); local rc=$?
    if [ $rc -eq 0 ] && ! grep -q "FAIL" <<<"$out"; then echo "ok   $n ($(grep -c PASS <<<"$out") checks)"
    else echo "FAIL $n (exit $rc)"; grep FAIL <<<"$out" | head -5; fails=$((fails + 1)); fi
}
run sha1     -- "$R/tests/live/test_sha1.c" "$S/sha1.c"
run apframe  -- "$R/tests/apframe/test_apframe.c" "$S/apframe.c"
run md5jpeg  -- "$R/tests/test_md5_jpeg.c" "$S/md5.c" "$S/jpeg_enc.c"
run qrwifi   -- -DQRWIFI_NO_CAMERA -I"$Q" "$R/tests/qr/test_qrwifi.c" "$S/qrwifi.c" \
                "$Q/quirc.c" "$Q/decode.c" "$Q/identify.c" "$Q/version_db.c" -lm
mkdir -p "$O/cfg"
run backup "$O/cfg" -- -DCFG_DIR="\"$O/cfg\"" "$R/tests/backup/test_backup.c" "$S/backup.c"
N=$O/net; mkdir -p "$N"
run netcfg   -- -DNETCFG_SIM -DNET_ENABLE_AP=1 -DNET_TMP="\"$N\"" -DNET_STA_CONF="\"$N/sta.conf\"" -DNET_STA_CONF_NAME="\"sta.conf\"" \
                -DNET_AP_FILE="\"$N/ap\"" -DAP_MINUTE_S=5 -DBOOT_GRACE_S=0 -DFALLBACK_AFTER_S=3 \
                -DCONNECT_TIMEOUT_S=4 -DAP_UP_TIMEOUT_S=3 -DAP_EXTEND_S=3 \
                "$R/tests/test_netcfg.c" "$S/netcfg.c" "$S/monotime.c" -lpthread
if out=$(sh "$R/tests/flash/run_flash_tests.sh" 2>&1) && grep -q "ALL PASS" <<<"$out"; then echo "ok   flash_image.sh"
else echo "FAIL flash_image.sh"; tail -5 <<<"$out"; fails=$((fails + 1)); fi
[ $fails -eq 0 ] && echo "ALL HOST TESTS PASS" || { echo "$fails test(s) failed"; exit 1; }
