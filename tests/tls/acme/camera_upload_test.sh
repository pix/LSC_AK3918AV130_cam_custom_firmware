#!/bin/bash
# Custom certificate upload on a camera running this build, through the web API:
# rejections, SNI selection on HTTPS (443) and RTSPS (322), host allow-list,
# persistence across a restart of ak_rtsp, removal.
# Usage: CAM_USER=admin CAM_PASS=... bash camera_upload_test.sh <camera-ip> <fixtures dir from gen_fixtures.sh>
set -u
H=$1; F=$2
C=(curl -s -m 30 --digest -u "$CAM_USER:$CAM_PASS" -H "X-Requested-With: lsc")
fails=0; passes=0
ok()  { echo "PASS $*"; passes=$((passes + 1)); }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
up() { cat "$@" | "${C[@]}" --data-binary @- -o /tmp/up.$$ -w "%{http_code}" "http://$H/api/tls/custom"; }
reject() {   # <label> <text> files...
  local label=$1 want=$2; shift 2
  local code; code=$(up "$@")
  if [ "$code" = 400 ] && grep -qF "$want" /tmp/up.$$; then ok "reject $label: $(cat /tmp/up.$$)"; else bad "reject $label: HTTP $code $(head -c 200 /tmp/up.$$)"; fi
}
served() { openssl s_client -connect "$H:${2:-443}" ${1:+-servername "$1"} </dev/null 2>/dev/null | openssl x509 -noout -subject 2>/dev/null; }
camcert() { served "" | grep -q "OU=LSC camera"; }

code=$(curl -s -o /dev/null -w "%{http_code}" --data-binary @"$F/good.crt" "http://$H/api/tls/custom")
[ "$code" = 401 ] && ok "upload without login: 401" || bad "upload without login: $code"
code=$(curl -s -o /dev/null -w "%{http_code}" --digest -u "$CAM_USER:$CAM_PASS" --data-binary @"$F/good.crt" "http://$H/api/tls/custom")
[ "$code" = 403 ] && ok "upload without X-Requested-With: 403" || bad "no XRW: $code"
code=$(curl -s -o /dev/null -w "%{http_code}" "http://$H/.well-known/acme-challenge/abc")
[ "$code" = 404 ] && ok "unknown http-01 token: 404 without login" || bad "http-01: $code"
"${C[@]}" -X POST "http://$H/api/tls/custom/clear" >/dev/null
code=$(curl -s -o /dev/null -w "%{http_code}" --digest -u "$CAM_USER:$CAM_PASS" -H "Host: cam.example.org" "http://$H/api/status")
[ "$code" = 421 ] && ok "Host cam.example.org refused before the upload (421)" || bad "host before: $code"

reject "certificate without key" "no private key" "$F/good.crt" "$F/inter.crt"
reject "wrong key" "does not belong" "$F/good.crt" "$F/other.key"
reject "chain in the wrong order" "must come first" "$F/inter.crt" "$F/good.crt" "$F/ec.key"
reject "expired" "expired" "$F/expired.crt" "$F/e1.key"
reject "not yet valid" "not valid yet" "$F/future.crt" "$F/e2.key"
reject "clientAuth only" "not for servers" "$F/client.crt" "$F/e6.key"
reject "P-521" "P-256 / P-384" "$F/p521.crt" "$F/p521.key"
reject "encrypted key" "encrypted" "$F/good.crt" "$F/ec_enc.key"
reject "no DNS name" "no DNS name" "$F/nosan.crt" "$F/e4.key"
reject "RSA-1024" "2048 to 4096" "$F/rsa1024.crt" "$F/rsa1024.key"
head -c 40000 /dev/urandom | base64 > /tmp/big.$$
code=$("${C[@]}" --data-binary @/tmp/big.$$ -o /dev/null -w "%{http_code}" "http://$H/api/tls/custom")
[ "$code" != 200 ] && ok "54 KB upload refused (HTTP $code)" || bad "big upload accepted"
"${C[@]}" -o /dev/null -w "" "http://$H/api/status" && ok "web server still answers after the oversized upload" || bad "web server down"
camcert && ok "camera certificate still served after the rejections" || bad "camera certificate"

code=$(up "$F/rsa.crt" "$F/inter.crt" "$F/rsa.key")
[ "$code" = 200 ] && ok "RSA-2048 accepted" || bad "RSA: $code $(cat /tmp/up.$$)"
served rsa.example.org | grep -q "CN=rsa$" && ok "RSA certificate served for rsa.example.org" || bad "RSA SNI: $(served rsa.example.org)"
code=$(up "$F/good.crt" "$F/inter.crt" "$F/ec.key")
[ "$code" = 200 ] && ok "EC P-256 + chain accepted" || bad "EC: $code $(cat /tmp/up.$$)"
python3 -I -c 'import json,sys; j=json.load(open(sys.argv[1])); c=j["custom"]; assert c["present"] and c["active"] and c["names"]=="cam.example.org,*.wild.example.org", c' /tmp/up.$$ \
  && ok "status JSON describes the certificate" || bad "status JSON: $(head -c 300 /tmp/up.$$)"
for n in cam.example.org CAM.EXAMPLE.ORG x.wild.example.org; do
  served "$n" | grep -q "CN=good$" && ok "HTTPS SNI $n -> custom" || bad "HTTPS SNI $n: $(served "$n")"
done
for n in "" other.lan wild.example.org "$H"; do
  served "$n" | grep -q "OU=LSC camera" && ok "HTTPS SNI '${n:-none}' -> camera certificate" || bad "HTTPS SNI '$n': $(served "$n")"
done
served cam.example.org 322 | grep -q "CN=good$" && ok "RTSPS (322) SNI cam.example.org -> custom" || bad "RTSPS SNI: $(served cam.example.org 322)"
served "" 322 | grep -q "OU=LSC camera" && ok "RTSPS without SNI -> camera certificate" || bad "RTSPS no SNI"
openssl s_client -connect "$H:443" -servername cam.example.org -CAfile "$F/root.crt" -verify_hostname cam.example.org -verify_return_error </dev/null 2>&1 | grep -q "Verify return code: 0" \
  && ok "chain sent by the camera verifies to the test root" || bad "chain verification"
code=$(curl -sk -o /dev/null -w "%{http_code}" --digest -u "$CAM_USER:$CAM_PASS" --resolve "cam.example.org:443:$H" "https://cam.example.org/api/status")
[ "$code" = 200 ] && ok "https://cam.example.org/ accepted by the host allow-list" || bad "host allow-list: $code"
code=$(curl -s -o /dev/null -w "%{http_code}" --cacert "$F/root.crt" --digest -u "$CAM_USER:$CAM_PASS" --resolve "x.wild.example.org:443:$H" "https://x.wild.example.org/api/status")
[ "$code" = 200 ] && ok "curl with full verification via the wildcard name: 200" || bad "wildcard curl: $code"

CAMSH=${CAMSH:-$(dirname "$0")/../../../tools/dev/camsh.py}
camsh() { echo "$1" | python3 "$CAMSH" "$H" 8 -; }
restart_akr() {
  camsh 'kill $(pidof ak_rtsp)' >/dev/null
  sleep 15
  for i in $(seq 1 30); do "${C[@]}" -o /dev/null "http://$H/api/status" && break; sleep 2; done
  sleep 5
}
echo "-- restart ak_rtsp (certificate must be loaded from /etc/config)"
restart_akr
served cam.example.org | grep -q "CN=good$" && ok "custom certificate served after restart" || bad "after restart: $(served cam.example.org)"

echo "-- use it for every connection"
"${C[@]}" --data "custom_all=1&redirect=1" -o /tmp/up.$$ "http://$H/api/tls"
grep -q '"custom_all":true' /tmp/up.$$ && grep -q '"redirect":true' /tmp/up.$$ && ok "custom_all and redirect on" || bad "custom_all: $(head -c 300 /tmp/up.$$)"
for n in "" other.lan "$H" cam.example.org; do
  served "$n" | grep -q "CN=good$" && ok "custom_all: SNI '${n:-none}' -> custom" || bad "custom_all SNI '$n': $(served "$n")"
done
served "" 322 | grep -q "CN=good$" && ok "custom_all: RTSPS without SNI -> custom" || bad "custom_all RTSPS"
code=$(curl -s -o /dev/null -w "%{http_code}" --digest -u "$CAM_USER:$CAM_PASS" "http://$H/api/tls/ca.crt")
[ "$code" = 200 ] && ok "camera CA still served over HTTP with redirect on (not redirected)" || bad "CA over HTTP: $code"
code=$(curl -s -o /dev/null -w "%{http_code}" "http://$H/")
[ "$code" = 301 ] && ok "page redirected to HTTPS" || bad "redirect: $code"
if [ "${RESET_TEST:-}" = 1 ]; then
  echo "-- reset_auth flag on the SD card (same action as holding the button): login admin/admin, redirect + custom_all off"
  camsh 'touch /tmp/sd/reset_auth; sync' >/dev/null
  restart_akr
  "${C[@]}" -o /tmp/up.$$ "http://$H/api/tls"
  grep -q '"custom_all":false' /tmp/up.$$ && grep -q '"redirect":false' /tmp/up.$$ && ok "after reset: custom_all and redirect off" || bad "after reset: $(head -c 300 /tmp/up.$$)"
  grep -q '"present":true' /tmp/up.$$ && ok "after reset: certificate kept" || bad "after reset: certificate lost"
  served "" | grep -q "OU=LSC camera" && served cam.example.org | grep -q "CN=good$" && ok "after reset: camera certificate for addresses, custom for its names" || bad "after reset SNI"
  camsh 'ls /tmp/sd/reset_auth.done && rm -f /tmp/sd/reset_auth.done' | grep -q "reset_auth.done" && ok "flag consumed (reset_auth.done)" || bad "flag not consumed"
else
  "${C[@]}" -k --data "custom_all=0&redirect=0" -o /dev/null "https://$H/api/tls"   # plain HTTP is redirected now
fi

echo "-- certificate for the camera's address (company CA)"
W=$(mktemp -d)
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$W/ip.key" -out "$W/ip.csr" -subj "/CN=ipcert" 2>/dev/null
printf "basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:%s\n" "$H" > "$W/ip.ext"
openssl x509 -req -in "$W/ip.csr" -CA "$F/inter.crt" -CAkey "$F/inter.key" -out "$W/ip.crt" -days 30 -extfile "$W/ip.ext" 2>/dev/null
code=$(up "$W/ip.crt" "$F/inter.crt" "$W/ip.key")
[ "$code" = 200 ] && ok "address-only certificate accepted" || bad "IP cert: $code $(cat /tmp/up.$$)"
served "" | grep -q "CN=ipcert" && ok "no SNI (https://$H/) -> address certificate" || bad "IP no SNI: $(served "")"
served other.lan | grep -q "OU=LSC camera" && ok "other.lan -> camera certificate" || bad "IP cert other.lan: $(served other.lan)"
code=$(curl -s -o /dev/null -w "%{http_code}" --cacert "$F/root.crt" --digest -u "$CAM_USER:$CAM_PASS" "https://$H/api/status")
[ "$code" = 200 ] && ok "curl https://$H/ with full verification against the company CA: 200" || bad "curl IP verify: $code"

echo "-- RSA-4096 handshake time (ARM9 without crypto hardware)"
code=$(up "$F/rsa4096.crt" "$F/inter.crt" "$F/rsa4096.key")
[ "$code" = 200 ] && ok "RSA-4096 accepted" || bad "RSA-4096: $code $(cat /tmp/up.$$)"
t0=$(date +%s.%N); served big.example.org | grep -q "CN=rsa4096" && r1=ok || r1=bad; t1=$(date +%s.%N)
t2=$(date +%s.%N); served other.lan >/dev/null; t3=$(date +%s.%N)
d4096=$(python3 -c "print(round($t1 - $t0, 2))"); dcam=$(python3 -c "print(round($t3 - $t2, 2))")
[ "$r1" = ok ] && ok "RSA-4096 served: handshake ${d4096} s (camera EC certificate: ${dcam} s)" || bad "RSA-4096 not served"
python3 -c "import sys; sys.exit(0 if $d4096 < 8 else 1)" && ok "RSA-4096 handshake under 8 s" || bad "RSA-4096 handshake ${d4096} s"
"${C[@]}" -o /dev/null -w "" "http://$H/api/status" && ok "web server answers after RSA-4096 handshakes" || bad "web server"
rm -rf "$W"

"${C[@]}" -X POST -o /tmp/up.$$ "http://$H/api/tls/custom/clear"
grep -q '"custom":null' /tmp/up.$$ && ok "removed" || bad "remove: $(head -c 200 /tmp/up.$$)"
served cam.example.org | grep -q "OU=LSC camera" && ok "after removal: camera certificate" || bad "after removal"
code=$(curl -sk -o /dev/null -w "%{http_code}" --digest -u "$CAM_USER:$CAM_PASS" --resolve "cam.example.org:443:$H" "https://cam.example.org/api/status")
[ "$code" = 421 ] && ok "after removal the name is refused again (421)" || bad "host after removal: $code"
rm -f /tmp/up.$$ /tmp/big.$$
echo "$passes passed, $fails failed"
[ $fails -eq 0 ]
