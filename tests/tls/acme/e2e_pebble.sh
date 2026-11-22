#!/bin/bash
# End-to-end ACME tests against Pebble (Let's Encrypt's test server).
#   bash e2e_pebble.sh host                 the camera code built for the PC
#   bash e2e_pebble.sh camera <camera-ip>   the camera itself (test build running,
#                                           login in CAM_USER / CAM_PASS)
# Needs pebble + pebble-challtestsrv in PATH or PEBBLE_BIN (go install
# github.com/letsencrypt/pebble/v2/cmd/...@v2.8.0), openssl, python3, gcc.
# Covers http-01, Cloudflare / DuckDNS / acme-dns (stand-in APIs feeding
# Pebble's DNS), wildcard names, the account cache, renewal decisions and
# the usual failures. Pebble also rejects ~5% of nonces on purpose.
set -u
MODE=${1:-host}; CAM=${2:-}
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/../../.." && pwd)
S=$R/src/ak_rtsp; [ -d "$S" ] || S=$R/firmware/src/ak_rtsp
PB=${PEBBLE_BIN:-$(dirname "$(command -v pebble 2>/dev/null || echo /nonexistent/x)")}
W=$(mktemp -d); PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$W"; }
trap cleanup EXIT
fails=0; passes=0
ok()  { echo "PASS $*"; passes=$((passes + 1)); }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

HOSTIP=$(ip -4 route get "${CAM:-1.1.1.1}" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
if [ "$MODE" = camera ]; then TARGET=$CAM; HTTP_PORT=80; else TARGET=127.0.0.1; HTTP_PORT=5002; HTTPS_PORT=5443; fi
[ -x "$PB/pebble" ] || { echo "pebble not found (PEBBLE_BIN)"; exit 1; }

# ---- a CA for Pebble's API certificate and the HTTPS stand-in API ----
cd "$W"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout tca.key -out tca.crt -days 30 -subj "/CN=E2E test CA" \
  -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign 2>/dev/null
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout api.key -out api.csr -subj "/CN=pebble" 2>/dev/null
printf "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:%s\nextendedKeyUsage=serverAuth\n" "$HOSTIP" > api.ext
openssl x509 -req -in api.csr -CA tca.crt -CAkey tca.key -out api.crt -days 30 -extfile api.ext 2>/dev/null
cat > pebble.json <<EOF
{"pebble":{"listenAddress":"0.0.0.0:14000","managementListenAddress":"0.0.0.0:15000","certificate":"$W/api.crt","privateKey":"$W/api.key",
 "httpPort":$HTTP_PORT,"tlsPort":5001,"ocspResponderURL":"","externalAccountBindingRequired":false,"retryAfter":{"authz":1,"order":1},
 "profiles":{"default":{"description":"90 days","validityPeriod":7776000}}}}
EOF
"$PB/pebble-challtestsrv" -defaultIPv4 "$TARGET" -defaultIPv6 "" -dns01 :8053 -management :8055 -http01 "" -https01 "" -tlsalpn01 "" -doh "" >chall.log 2>&1 & PIDS+=($!)
PEBBLE_VA_NOSLEEP=1 "$PB/pebble" -config pebble.json -dnsserver 127.0.0.1:8053 >pebble.log 2>&1 & PIDS+=($!)
python3 -I "$T/mock_dns_apis.py" 8090 http://127.0.0.1:8055 >mock.log 2>&1 & PIDS+=($!)
python3 -I "$T/mock_dns_apis.py" 8091 http://127.0.0.1:8055 api.crt api.key >mock_tls.log 2>&1 & PIDS+=($!)
for i in $(seq 1 50); do curl -sk -m1 https://127.0.0.1:14000/dir >/dev/null && grep -q ready mock.log && break; sleep 0.2; done
curl -sk https://127.0.0.1:14000/dir | grep -q newOrder || { echo "pebble did not start"; cat pebble.log; exit 1; }
DIR=https://$HOSTIP:14000/dir; [ "$MODE" = host ] && DIR=https://127.0.0.1:14000/dir
API=http://$HOSTIP:8090; APIS=https://$HOSTIP:8091; [ "$MODE" = host ] && { API=http://127.0.0.1:8090; APIS=https://127.0.0.1:8091; }

# ---- how one issuance is run ----
if [ "$MODE" = host ]; then
  mkdir -p cfg mb
  M=$S/third_party/mbedtls
  F=(-std=gnu99 -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer -iquote "$S" -I"$M/include" -DMBEDTLS_CONFIG_FILE='"tls_config.h"')
  ls "$M"/library/*.c | xargs -P8 -I{} sh -c 'gcc "$@" -I"'"$M"'/library" -c {} -o "'"$W"'/mb/$(basename {} .c).o"' _ "${F[@]}" || exit 1
  (cd "$S" && python3 -I gen_web_page.py acme_roots.pem "$W/acme_roots.c" acme_roots_pem)
  gcc "${F[@]}" -DTLS_DIR="\"$W/cfg\"" "$T/e2e_host.c" "$S/tls.c" "$S/acme.c" "$S/monotime.c" "$W/acme_roots.c" mb/*.o -lpthread -o e2e || exit 1
  # issue <ca|-> <register|""> settings...   (stdout -> run.log; leaves the harness serving for 25 s)
  issue() {
    local ca=$1 reg=$2; shift 2
    (cd "$W" && ASAN_OPTIONS=detect_leaks=1 E2E_LINGER=25 ./e2e $HTTP_PORT $HTTPS_PORT "$ca" $reg acme.enabled=off "$@" acme.enabled=on > run.log 2>&1) &
    LAST=$!
    for i in $(seq 1 450); do grep -q "^STATUS" run.log 2>/dev/null && break; sleep 1; done
    grep "^STATUS" run.log | sed 's/^STATUS //' > status.json
  }
  sni_cert() { openssl s_client -connect 127.0.0.1:$HTTPS_PORT -servername "$1" </dev/null 2>/dev/null | openssl x509 -noout -subject -issuer -ext subjectAltName 2>/dev/null; }
  sni_verify() { openssl s_client -connect 127.0.0.1:$HTTPS_PORT -servername "$1" -verify_hostname "$1" -CAfile pebble_root.pem -verify_return_error </dev/null 2>&1 | grep -q "Verify return code: 0"; }
  # every run of the harness ends with LeakSanitizer: collect its reports (and any ASan / UBSan one)
  finish() { wait "$LAST" 2>/dev/null; grep -E "Direct leak|Indirect leak|ERROR: AddressSanitizer|runtime error" run.log >> sanitizer.log; true; }
else
  : "${CAM_USER:?}" "${CAM_PASS:?}"
  C=(curl -s --digest -u "$CAM_USER:$CAM_PASS" -H "X-Requested-With: lsc" -m 20)
  issue() {
    local ca=$1 reg=$2; shift 2
    if [ "$ca" = - ]; then "${C[@]}" -X POST --data-binary "" "http://$CAM/api/acme/ca" >/dev/null
    else "${C[@]}" --data-binary @"$ca" "http://$CAM/api/acme/ca" >/dev/null; fi
    local args=(--data-urlencode "acme.enabled=off")
    for kv in "$@"; do args+=(--data-urlencode "$kv"); done
    "${C[@]}" "${args[@]}" "http://$CAM/api/config" >/dev/null
    if [ -n "$reg" ]; then      # done by the ACME thread: wait for acme.dns_target
      "${C[@]}" -X POST "http://$CAM/api/acme/register" > register.json
      for i in $(seq 1 30); do "${C[@]}" "http://$CAM/api/config?prefix=acme." | grep -q '"acme.dns_target":"[^"]' && break; sleep 1; done
      echo "register: $("${C[@]}" "http://$CAM/api/config?prefix=acme." | grep -o '"acme.dns_target":"[^"]*"')"
    fi
    "${C[@]}" --data-urlencode "acme.enabled=on" "http://$CAM/api/config" >/dev/null
    "${C[@]}" -X POST "http://$CAM/api/acme/run" >/dev/null
    sleep 5
    for i in $(seq 1 150); do "${C[@]}" "http://$CAM/api/acme" > status.json; grep -qE '"state":"(running|waiting)"' status.json || break; sleep 2; done
    "${C[@]}" "http://$CAM/api/tls" > tls.json
    # the camera keeps its last 24 log lines across runs: keep this run's only (from the last "starting:")
    python3 -I -c 'import json,sys; s=json.load(open("status.json")); l=s["log"]; i=max([k for k,x in enumerate(l) if " starting: " in x] or [0]); print("\n".join(l[i:])); print(s["message"])' > run.log 2>&1
  }
  sni_cert() { openssl s_client -connect "$CAM:443" -servername "$1" </dev/null 2>/dev/null | openssl x509 -noout -subject -issuer -ext subjectAltName 2>/dev/null; }
  sni_verify() { openssl s_client -connect "$CAM:443" -servername "$1" -verify_hostname "$1" -CAfile pebble_root.pem -verify_return_error </dev/null 2>&1 | grep -q "Verify return code: 0"; }
  finish() { :; }
fi
state() { python3 -I -c 'import json,sys; s=json.load(open(sys.argv[1])); print(s["state"]); print(s["message"])' status.json 2>/dev/null; }
roots() { : > pebble_root.pem; for i in 0 1 2; do curl -sk "https://127.0.0.1:15000/roots/$i" >> pebble_root.pem 2>/dev/null; curl -sk "https://127.0.0.1:15000/intermediates/$i" >> pebble_root.pem 2>/dev/null; done; }
expect_ok() {   # <label> <sni names...>
  local label=$1; shift
  if [ "$(state | head -1)" = ok ]; then ok "$label: issued"; else bad "$label: $(state | tail -1)"; tail -15 run.log; return; fi
  roots
  for n in "$@"; do
    if sni_verify "$n"; then ok "$label: $n served, chain verifies against Pebble's root"
    else bad "$label: $n not served correctly: $(sni_cert "$n" | tr '\n' ' ')"; fi
  done
}
expect_err() {  # <label> <text in the error>
  if [ "$(state | head -1)" = error ] && state | tail -1 | grep -qF -- "$2"; then ok "$1: $(state | tail -1)"
  else bad "$1: expected error \"$2\", got: $(state | tr '\n' ' ')"; tail -8 run.log; fi
}

CHAL=http; [ "$MODE" = camera ] && echo "camera $CAM, Pebble at $DIR, HTTP-01 to port 80"
echo "== http-01"
issue tca.crt "" acme.server=$DIR acme.domains=cam.example.test acme.challenge=http acme.email=test@example.test
expect_ok "http-01" cam.example.test
grep -q "account created" run.log && ok "account created on first use" || bad "no account creation in log"
if [ "$MODE" = host ]; then
  sni_cert other.lan | grep -q "issuer=.*LSC camera CA" && ok "other names keep the camera certificate" || bad "other name: $(sni_cert other.lan)"
else
  sni_cert "$CAM" | grep -q "LSC camera CA" && ok "address keeps the camera certificate" || bad "address: $(sni_cert "$CAM")"
fi
finish

echo "== cloudflare (HTTP stand-in), wildcard"
issue tca.crt "" acme.server=$DIR "acme.domains=cf.example.test, *.example.test" acme.challenge=cloudflare acme.dns_token=cf-test-token_123 \
      acme.dns_api=$API/client/v4 acme.dns_wait=1
expect_ok "cloudflare" cf.example.test anything.example.test
grep -q "account created" run.log && bad "account registered again (cache not used)" || ok "cached account reused"
c=$(grep -c '"op": "create"' mock.log); d=$(grep -c '"op": "delete"' mock.log)
[ "$c" -ge 2 ] && [ "$c" = "$d" ] && ok "cloudflare: $c TXT records created, $d removed" || bad "cloudflare records created $c removed $d"
grep '"op": "create"' mock.log | grep -q '"ttl": 60' && ok "cloudflare: ttl 60" || bad "cloudflare ttl"
finish

echo "== cloudflare over HTTPS (stand-in trusted through the extra CA)"
issue tca.crt "" acme.server=$DIR acme.domains=tls.example.test acme.challenge=cloudflare acme.dns_token=cf-test-token_123 acme.dns_api=$APIS/client/v4 acme.dns_wait=1
expect_ok "cloudflare https" tls.example.test
grep -q '"op": "create"' mock_tls.log && ok "HTTPS API used" || bad "HTTPS API not used"
finish

echo "== duckdns, wildcard on one TXT name"
issue tca.crt "" acme.server=$DIR "acme.domains=camtest.duckdns.org,*.camtest.duckdns.org" acme.challenge=duckdns acme.dns_token=duck-token-0000 acme.dns_api=$API acme.dns_wait=1
expect_ok "duckdns" camtest.duckdns.org x.camtest.duckdns.org
grep '"api": "duckdns"' mock.log | grep -q '"domains": "camtest"' && ok "duckdns: domain label sent" || bad "duckdns domains param"
grep -q '"clear": "true"' mock.log && ok "duckdns: record cleared" || bad "duckdns: no clear"
finish

echo "== acme-dns (register from the camera)"
curl -s -X POST --data '["adns.example.test", "*.adns.example.test"]' $API/test/acmedns-names >/dev/null
issue tca.crt register acme.server=$DIR "acme.domains=adns.example.test,*.adns.example.test" acme.challenge=acmedns acme.dns_api=$API acme.dns_wait=1
expect_ok "acme-dns" adns.example.test a.adns.example.test
grep -q '"op": "register"' mock.log && ok "acme-dns: registered" || bad "acme-dns: no register"
[ "$(grep -c '"api": "acmedns", "op": "update"' mock.log)" -ge 2 ] && ok "acme-dns: two updates" || bad "acme-dns updates"
finish

echo "== failures"
issue tca.crt "" acme.server=$DIR acme.domains=badtok.example.test acme.challenge=cloudflare acme.dns_token=wrong-token acme.dns_api=$API/client/v4
expect_err "cloudflare bad token" "Cloudflare: Invalid format for Authorization header (HTTP 400)"; finish
issue tca.crt "" acme.server=$DIR acme.domains=nozone.other.test acme.challenge=cloudflare acme.dns_token=cf-test-token_123 acme.dns_api=$API/client/v4
expect_err "cloudflare unknown zone" "no Cloudflare zone found for nozone.other.test"; finish
issue tca.crt "" acme.server=$DIR acme.domains=badtok.duckdns.org acme.challenge=duckdns acme.dns_token=bad-token acme.dns_api=$API
expect_err "duckdns bad token" "DuckDNS refused the update"; finish
issue tca.crt "" acme.server=$DIR acme.domains=cam.example.org acme.challenge=duckdns acme.dns_token=duck-token-0000 acme.dns_api=$API
expect_err "duckdns non-duckdns name" "is not a duckdns.org name"; finish
issue tca.crt "" acme.server=$DIR "acme.domains=*.example.test" acme.challenge=http
expect_err "wildcard with http-01" "wildcard names need a DNS challenge"; finish
issue - "" acme.server=$DIR acme.domains=cam.example.test acme.challenge=http
expect_err "ACME server not trusted" "not signed by a trusted CA"; finish
issue tca.crt "" acme.server=https://${DIR#https://*}x acme.domains=cam.example.test acme.challenge=http
expect_err "wrong directory URL" "not an ACME directory (HTTP 404)"; finish
issue tca.crt "" acme.server=https://127.0.0.1:1/dir acme.domains=cam.example.test acme.challenge=http
expect_err "ACME server down" "cannot connect"; finish
curl -s -X POST -d "{\"host\":\"unreach.example.test.\",\"addresses\":[\"192.0.2.1\"]}" http://127.0.0.1:8055/add-a >/dev/null
[ "$MODE" = camera ] && curl -s -X POST -d "{\"host\":\"unreach.example.test.\",\"addresses\":[\"$HOSTIP\"]}" http://127.0.0.1:8055/add-a >/dev/null
issue tca.crt "" acme.server=$DIR acme.domains=unreach.example.test acme.challenge=http
expect_err "http-01 answered by another server" "validation failed"; finish
issue tca.crt "" acme.server=$DIR acme.domains=adnsbad.example.test acme.challenge=acmedns acme.dns_api=$API acme.dns_user=nobody acme.dns_token=wrongpass acme.dns_sub=nosub
expect_err "acme-dns bad credentials" "acme-dns refused the update: HTTP 401"; finish

echo "== renewal decision: nothing to do when the certificate matches"
issue tca.crt "" acme.server=$DIR acme.domains=cam.example.test acme.challenge=http
expect_ok "re-issue for a new name set" cam.example.test; finish
if [ "$MODE" = host ]; then
  (cd "$W" && ASAN_OPTIONS=detect_leaks=1 E2E_SERVE=12 ./e2e $HTTP_PORT $HTTPS_PORT tca.crt acme.server=$DIR acme.domains=cam.example.test acme.challenge=http acme.enabled=on > idle.log 2>&1)
  grep -q "\[acme\] starting" idle.log && bad "issued again although the certificate matches" || ok "no new issuance when the certificate matches"
  grep -E "Direct leak|Indirect leak|ERROR: AddressSanitizer|runtime error" idle.log >> sanitizer.log
  [ -s sanitizer.log ] && { bad "sanitizer reports:"; sort sanitizer.log | uniq -c | head; } || ok "no ASan / UBSan / LeakSanitizer reports in any run"
fi
grep -c "badNonce\|bad nonce" pebble.log >/dev/null; echo "(Pebble rejected $(grep -ci 'nonce' pebble.log) nonce-related requests on purpose)"
echo "$passes passed, $fails failed"
[ $fails -eq 0 ] && echo "ALL E2E TESTS PASS" || exit 1
