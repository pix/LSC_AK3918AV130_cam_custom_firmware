#!/bin/bash
# tlstest.sh <host> <http_port> <https_port> <user:pass>   (host = IP the cert must cover)
H=$1; P=$2; S=$3; U=$4; D=${TMPDIR:-/tmp}/tlstest; mkdir -p $D; mkdir -p $D
A=(-s -m 20 -u "$U" --digest)
X=(-H X-Requested-With:lsc)
ok(){ echo "PASS $*"; }; bad(){ echo "FAIL $*"; FAILS=$((FAILS+1)); }; FAILS=0
code=$(curl "${A[@]}" -o $D/ca.crt -w "%{http_code}" http://$H:$P/api/tls/ca.crt); [ "$code" = 200 ] && ok "CA download over HTTP" || bad "CA download $code"
openssl x509 -in $D/ca.crt -noout >/dev/null 2>&1 && ok "CA parses" || bad "CA parse"
code=$(curl "${A[@]}" --cacert $D/ca.crt -o /dev/null -w "%{http_code}" https://$H:$S/api/status); [ "$code" = 200 ] && ok "HTTPS by IP verifies against CA, login ok" || bad "HTTPS by IP: $code"
code=$(curl -s -m 20 --cacert $D/ca.crt -o /dev/null -w "%{http_code}" https://$H:$S/api/status); [ "$code" = 401 ] && ok "HTTPS without login -> 401" || bad "no-login $code"
code=$(curl -s -m 20 -o /dev/null -w "%{http_code}" https://$H:$S/ 2>/dev/null); [ "$code" = 000 ] && ok "untrusted client rejects cert (expected)" || bad "untrusted got $code"
# names
r=$(curl "${A[@]}" "${X[@]}" --cacert $D/ca.crt -d "names=Cam.Test.lan, other.example" https://$H:$S/api/tls); echo "  $r"
echo "$r" | grep -q '"names":"cam.test.lan,other.example"' && ok "names saved (lowercased)" || bad "names"
code=$(curl "${A[@]}" --cacert $D/ca.crt --resolve cam.test.lan:$S:$H -o /dev/null -w "%{http_code}" https://cam.test.lan:$S/api/status); [ "$code" = 200 ] && ok "HTTPS by DNS name verifies" || bad "by name $code"
code=$(curl "${A[@]}" "${X[@]}" --cacert $D/ca.crt -o /dev/null -w "%{http_code}" -d "names=bad_name!" https://$H:$S/api/tls); [ "$code" = 400 ] && ok "invalid name rejected" || bad "invalid name $code"
code=$(curl "${A[@]}" "${X[@]}" --cacert $D/ca.crt -o /dev/null -w "%{http_code}" -d "names=a,b,c,d,e" https://$H:$S/api/tls); [ "$code" = 400 ] && ok "5 names rejected" || bad "5 names $code"
code=$(curl "${A[@]}" --cacert $D/ca.crt -o /dev/null -w "%{http_code}" -d "names=x.lan" https://$H:$S/api/tls); [ "$code" = 403 ] && ok "POST without CSRF header refused" || bad "csrf $code"
# resumption
echo | openssl s_client -connect $H:$S -CAfile $D/ca.crt -sess_out $D/sess -tls1_2 >/dev/null 2>&1
echo | openssl s_client -connect $H:$S -CAfile $D/ca.crt -sess_in $D/sess -tls1_2 2>/dev/null | grep -q "^Reused" && ok "session resumption" || bad "no resumption"
echo | openssl s_client -connect $H:$S -tls1_1 >/dev/null 2>&1 && bad "TLS 1.1 accepted" || ok "TLS 1.1 refused"
# redirect
curl "${A[@]}" "${X[@]}" --cacert $D/ca.crt -o /dev/null -d "redirect=1" https://$H:$S/api/tls
loc=$(curl -s -m 10 -o /dev/null -w "%{http_code} %{redirect_url}" "http://$H:$P/api/status?x=1"); [ "$loc" = "301 https://$H/api/status?x=1" ] && ok "HTTP redirect: $loc" || bad "redirect: $loc"
code=$(curl "${A[@]}" -o /dev/null -w "%{http_code}" http://$H:$P/api/tls/ca.crt); [ "$code" = 200 ] && ok "CA still on HTTP with redirect on" || bad "CA with redirect $code"
curl "${A[@]}" "${X[@]}" --cacert $D/ca.crt -o /dev/null -d "redirect=0" https://$H:$S/api/tls
code=$(curl "${A[@]}" -o /dev/null -w "%{http_code}" http://$H:$P/api/status); [ "$code" = 200 ] && ok "redirect off again" || bad "redirect off $code"
# parallel
for i in $(seq 1 12); do curl "${A[@]}" --cacert $D/ca.crt -o /dev/null -w "%{http_code}\n" https://$H:$S/api/status & done > $D/par.txt; wait
n=$(grep -c 200 $D/par.txt); [ "$n" = 12 ] && ok "12 parallel HTTPS requests" || bad "parallel: $(sort $D/par.txt | uniq -c | tr '\n' ' ')"
# timing
t=$(curl "${A[@]}" --cacert $D/ca.crt -o /dev/null -w "%{time_appconnect}" https://$H:$S/api/status); echo "  full handshake ${t}s"
# wss upgrade
tok=$(curl "${A[@]}" --cacert $D/ca.crt https://$H:$S/api/live/token | grep -o '[0-9a-f]\{32\}')
code=$(curl -s -m 5 --cacert $D/ca.crt --http1.1 -o /dev/null -w "%{http_code}" -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" -H "Origin: https://$H$([ "$S" = 443 ] && echo || echo :$S)" "https://$H:$S/api/live?t=$tok")
[ "$code" = 101 ] && ok "wss upgrade 101" || bad "wss $code"
# CA regeneration
old=$(openssl x509 -in $D/ca.crt -noout -fingerprint -sha256)
curl "${A[@]}" "${X[@]}" --cacert $D/ca.crt -o /dev/null -X POST https://$H:$S/api/tls/regenerate-ca
curl "${A[@]}" -o $D/ca2.crt http://$H:$P/api/tls/ca.crt
new=$(openssl x509 -in $D/ca2.crt -noout -fingerprint -sha256)
[ "$old" != "$new" ] && ok "new CA after regenerate" || bad "CA unchanged"
code=$(curl "${A[@]}" --cacert $D/ca2.crt -o /dev/null -w "%{http_code}" https://$H:$S/api/status); [ "$code" = 200 ] && ok "new CA verifies" || bad "new CA $code"
code=$(curl "${A[@]}" --cacert $D/ca.crt -o /dev/null -w "%{http_code}" https://$H:$S/api/status); [ "$code" = 000 ] && ok "old CA no longer verifies" || bad "old CA $code"
cp $D/ca2.crt $D/ca.crt
curl "${A[@]}" "${X[@]}" --cacert $D/ca.crt -o /dev/null -d "names=" https://$H:$S/api/tls
echo "FAILS=$FAILS"
