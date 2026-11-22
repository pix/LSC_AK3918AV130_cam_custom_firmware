#!/bin/bash
# Host tests for the custom HTTPS certificate and the ACME client
# (ASan + UBSan, no camera). Needs gcc, openssl 3.4+, python3.
# Usage: bash tests/tls/acme/run.sh   (VALGRIND=1: also under valgrind memcheck;
#        LEAKS=0: LeakSanitizer off; KEEP=1: keep the work directory and its logs)
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../../.." && pwd)
S=$R/src/ak_rtsp; [ -d "$S" ] || S=$R/firmware/src/ak_rtsp
M=$S/third_party/mbedtls
[ -f "$M/library/ssl_tls.c" ] || { echo "mbedTLS sources missing: run make in $S once"; exit 1; }
O=$(mktemp -d); trap '[ -n "${KEEP:-}" ] && echo "work directory kept: $O" || rm -rf "$O"' EXIT
F=(-std=gnu99 -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer -iquote "$S" -I"$M/include" -DMBEDTLS_CONFIG_FILE='"tls_config.h"')

echo "building mbedTLS for the host..."
mkdir -p "$O/mb"
ls "$M"/library/*.c | xargs -P8 -I{} sh -c 'gcc "$@" -I"'"$M"'/library" -c {} -o "'"$O"'/mb/$(basename {} .c).o"' _ "${F[@]}" || exit 1
sh "$T/gen_fixtures.sh" "$O/fx" >/dev/null || { echo "FAIL fixtures"; exit 1; }
(cd "$S" && python3 -I gen_web_page.py acme_roots.pem "$O/acme_roots.c" acme_roots_pem)

fails=0
run() {   # run <name> <args> -- <sources>
    local n=$1; shift; local args=(); while [ "$1" != "--" ]; do args+=("$1"); shift; done; shift
    mkdir -p "$O/$n.cfg"
    if ! gcc "${F[@]}" -DTLS_DIR="\"$O/$n.cfg\"" "$@" "$S/monotime.c" "$O/acme_roots.c" "$O"/mb/*.o -lpthread -o "$O/$n" 2>"$O/$n.err"; then
        echo "FAIL $n: build"; grep -m5 error "$O/$n.err"; fails=$((fails + 1)); return
    fi
    local out; out=$(cd "$O" && ASAN_OPTIONS=detect_leaks=${LEAKS:-1} timeout 900 "./$n" "${args[@]}" 2>&1); local rc=$?
    echo "$out" > "$O/$n.log"
    if [ $rc -eq 0 ] && ! grep -q "^FAIL" <<<"$out"; then echo "ok   $n ($(grep -c '^PASS' <<<"$out") checks)"
    else echo "FAIL $n (exit $rc)"; grep -E "^FAIL|ERROR|runtime error" <<<"$out" | head -20; fails=$((fails + 1)); fi
    [ -n "${VERBOSE:-}" ] && cat "$O/$n.log"
}
run custom_cert "$O/fx" "$O/custom.json" -- "$T/test_custom_cert.c"
run acme "$O/fx" "$O/acme_out" -- "$T/test_acme.c" "$S/tls.c"
# JSON produced by the camera code must parse; JWS / thumbprint checked independently
python3 -I "$T/check_json.py" "$O/custom.json" "$O/acme_out" || fails=$((fails + 1))

# valgrind memcheck (VALGRIND=1): the same two programs without ASan, -O0; any invalid
# access, uninitialised use or definite / indirect leak fails.
if [ -n "${VALGRIND:-}" ]; then
  V=(-std=gnu99 -O0 -g -w -iquote "$S" -I"$M/include" -DMBEDTLS_CONFIG_FILE='"tls_config.h"')
  mkdir -p "$O/vmb"
  ls "$M"/library/*.c | xargs -P8 -I{} sh -c 'gcc "$@" -I"'"$M"'/library" -c {} -o "'"$O"'/vmb/$(basename {} .c).o"' _ "${V[@]}" || exit 1
  for n in custom_cert acme; do
    src=("$T/test_custom_cert.c"); [ $n = acme ] && src=("$T/test_acme.c" "$S/tls.c")
    rm -rf "$O/v$n.cfg" "$O/v${n}_out"; mkdir -p "$O/v$n.cfg"
    gcc "${V[@]}" -DTLS_DIR="\"$O/v$n.cfg\"" "${src[@]}" "$S/monotime.c" "$O/acme_roots.c" "$O"/vmb/*.o -lpthread -o "$O/v$n" || { fails=$((fails + 1)); continue; }
    args=("$O/fx" "$O/vcustom.json"); [ $n = acme ] && args=("$O/fx" "$O/v${n}_out")
    (cd "$O" && timeout 3600 valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,indirect \
        --show-leak-kinds=definite,indirect "./v$n" "${args[@]}" > "$O/v$n.log" 2>&1); rc=$?
    if [ $rc -eq 0 ] && ! grep -q "^FAIL" "$O/v$n.log"; then echo "ok   valgrind $n ($(grep -c '^PASS' "$O/v$n.log") checks, no memcheck errors)"
    else echo "FAIL valgrind $n (exit $rc)"; grep -E "^FAIL|==[0-9]+== (Invalid|Conditional|Use of|.*definitely|.*indirectly)" "$O/v$n.log" | head -20; fails=$((fails + 1)); fi
  done
fi
[ $fails -eq 0 ] && echo "ALL ACME/TLS HOST TESTS PASS" || { echo "$fails test(s) failed"; exit 1; }
