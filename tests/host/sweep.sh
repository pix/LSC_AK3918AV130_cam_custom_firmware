#!/bin/bash
# Drives host_server (inside run_sandboxed.sh) through every web endpoint, every
# setting (valid and invalid values), uploads, backup/restore, RTSP/RTSPS
# (TCP, UDP, several clients), the live WebSocket + talk-back, ONVIF and the
# Home Assistant checks, MQTT with the fake broker. Functional failures are
# listed; the point is that the server survives and the sanitizers stay quiet.
#   sweep.sh <fixtures dir> <venv dir>
set -u
FX=$1; VENV=$2
H=${CAM:-127.0.0.1}; T=$(cd "$(dirname "$0")/.." && pwd)
A=(--digest -u admin:admin -m 30 -s -H "X-Requested-With: lsc")
W=$(mktemp -d)
fails=0; n=0
ok()  { n=$((n + 1)); echo "PASS $*"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "FAIL $*"; }
alive() { [ "$(curl -s -o /dev/null -m 10 -w %{http_code} http://$H/)" = 401 ]; }
code() { curl "${A[@]}" -o "$W/body" -w "%{http_code}" "$@"; }

echo "== GET endpoints"
for p in / /api/acme /api/audio/aec /api/config "/api/config?prefix=mqtt." /api/events /api/health /api/hook/token \
         /api/isp/ae "/api/isp/get?m=nr1" /api/live/token /api/md/raw /api/mqtt/camera.crt /api/mqtt/tls /api/osd \
         /api/params /api/rec "/api/rec/list?day=2026-01-01" /api/rec/all /api/status /api/sub /api/term/token /api/tls \
         /api/tls/ca.crt /api/tls/server.crt /api/tls/fullchain.pem /api/tz /api/wifi /api/wifi/scan /recordings \
         /snapshot.jpg "/snapshot.jpg?zoom=1" /terminal /api/nonexistent "/rec/../../etc/passwd" "/rec-tar/x" \
         "/.well-known/acme-challenge/x"; do
  c=$(code "http://$H$p")
  if [ "${c:0:1}" = 5 ] && [ "$c" != 503 ]; then bad "GET $p -> $c"; else ok "GET $p -> $c"; fi
  case "$p" in /api/config*|/api/status|/api/tls|/api/acme|/api/events|/api/health|/api/osd|/api/wifi|/api/tz|/api/rec*)
    [ "$c" = 200 ] && { python3 -I -c "import json,sys; json.load(open(sys.argv[1]))" "$W/body" 2>/dev/null && ok "$p is JSON" || bad "$p is not JSON: $(head -c 120 "$W/body")"; } ;;
  esac
done

echo "== every setting: current value, then invalid ones"
code "http://$H/api/config" >/dev/null
cp "$W/body" "$W/config.json"
python3 -I -c 'import json,sys; [print(k) for k in json.load(open(sys.argv[1]))]' "$W/config.json" > "$W/names"
echo "$(wc -l < "$W/names") settings"
LONG=$(head -c 700 /dev/zero | tr '\0' 'A')
while read -r name; do
  cur=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$W/config.json" "$name")
  [ "$cur" = "(set)" ] && cur=""
  case "$name" in web.hosts|web.proxies|dev.*|net.*) continue ;; esac     # keep the sweep reachable
  for v in "$cur" "" "-1" "0" "99999999999" "on" "off" "$LONG" $'a\x01b' "../../x" "é漢字" "1e9" " 5" "%00"; do
    curl "${A[@]}" -o /dev/null --data-urlencode "$name=$v" "http://$H/api/config"
  done
  curl "${A[@]}" -o /dev/null --data-urlencode "$name=$cur" "http://$H/api/config"
  for v in "$cur" "" "x" "$LONG"; do curl "${A[@]}" -o /dev/null --data-urlencode "name=$name" --data-urlencode "value=$v" "http://$H/api/set"; done
  curl "${A[@]}" -o /dev/null --data-urlencode "name=$name" --data-urlencode "value=$cur" "http://$H/api/set"
done < "$W/names"
alive && ok "server alive after the settings sweep" || bad "server down after the settings sweep"

echo "== long setting values survive (acme.domains up to 599 chars: were cut at 255 in the file)"
L63=$(head -c 63 /dev/zero | tr '\0' 'a'); D="$L63.$L63.x"; LONGD="$D.one.example.org,$D.two.example.org,$D.three.example.org,$D.four.example.org"
curl "${A[@]}" -o /dev/null --data-urlencode "acme.domains=$LONGD" "http://$H/api/config"
GOT=$(curl "${A[@]}" "http://$H/api/config?prefix=acme." | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["acme.domains"])')
[ "$GOT" = "$LONGD" ] && ok "acme.domains ${#LONGD} chars read back whole" || bad "acme.domains read back ${#GOT} of ${#LONGD} chars"
if [ -n "${EXT_FILE:-}" ]; then grep -qF "acme.domains	$LONGD" "$EXT_FILE" && ok "saved whole in the settings file" || bad "settings file holds a cut value"; fi
curl "${A[@]}" -o /dev/null --data-urlencode "acme.domains=" "http://$H/api/config"

echo "== POST endpoints"
post() { local p=$1; shift; local c; c=$(code "$@" "http://$H$p"); if [ "${c:0:1}" = 5 ] && [ "$c" != 502 ] && [ "$c" != 503 ]; then bad "POST $p -> $c"; else ok "POST $p -> $c"; fi; }
post /api/set --data "name=image.jpeg_quality&value=80"
post /api/osd --data "enabled=1&text=Test%20%C3%A9&pos=tl&size=2&time=1"
post /api/osd --data "enabled=0"
post /api/tls --data "names=cam.lan,x.lan&redirect=0"
post /api/tls --data "names=bad name!&redirect=7"
post /api/tls --data "names=&custom_all=0"
cat "$FX/good.crt" "$FX/inter.crt" "$FX/ec.key" > "$W/up.pem"
post /api/tls/custom --data-binary @"$W/up.pem"
post /api/tls --data "custom_all=1"; post /api/tls --data "custom_all=0"
post /api/tls/custom/clear -X POST
post /api/tls/custom --data-binary @"$FX/good.crt"
post /api/acme/ca --data-binary @"$FX/root.crt"; post /api/acme/ca --data-binary ""
post /api/acme/register -X POST
post /api/acme/run -X POST
post /api/mqtt/upload?what=ca --data-binary @"$FX/root.crt"
post /api/mqtt/upload?what=key --data-binary @"$FX/ec.key"
post /api/mqtt/upload?what=cert --data-binary @"$FX/good.crt"
post /api/mqtt/csr --data "cn=camera"
post /api/mqtt/clear --data "what=ca"; post /api/mqtt/clear --data "what=client"
post /api/mqtt/trust --data "what=ca&expect=00"
post /api/hook/token -X POST
post /api/sound/play --data "name=chime"; post /api/sound/play --data "name=nonexistent"
post /api/wifi/connect --data "ssid=TestNet&psk=password123"
post /api/wifi/qr --data "seconds=5"
post /api/ap/password --data "password=short"; post /api/ap/password --data "password=longenough123"
c=$(code -X POST "http://$H/api/npu/test"); [ "$c" = 500 ] && grep -q "npu init failed" "$W/body" && ok "POST /api/npu/test -> 500 (no NPU on the PC)" || bad "POST /api/npu/test -> $c"
post /api/rec/delete --data "day=2026-01-01&name=00-00-00_motion.mp4"
post /api/rec/tar --data "files=2026-01-01/00-00-00_motion.mp4"
post /api/tls/regenerate-ca -X POST
TOK=$(curl "${A[@]}" "http://$H/api/hook/token" | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("token",""))' 2>/dev/null)
python3 -I -c '
import struct, math, sys
n = 8000; d = b"".join(struct.pack("<h", int(8000*math.sin(i*0.05))) for i in range(n))
h = b"RIFF" + struct.pack("<I", 36 + len(d)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16) + b"data" + struct.pack("<I", len(d))
open(sys.argv[1], "wb").write(h + d)' "$W/tone.wav"
c=$(curl -s -m 30 -o /dev/null -w "%{http_code}" --data-binary @"$W/tone.wav" "http://$H/hook/sound?token=$TOK")
[ "${c:0:1}" != 5 ] && ok "POST /hook/sound WAV -> $c" || bad "POST /hook/sound -> $c"
c=$(curl -s -m 30 -o /dev/null -w "%{http_code}" --data-binary @"$W/up.pem" "http://$H/hook/sound?token=$TOK")
[ "${c:0:1}" != 5 ] && ok "POST /hook/sound non-WAV -> $c" || bad "POST /hook/sound garbage -> $c"
curl "${A[@]}" -o "$W/backup.tar" -X POST "http://$H/api/backup"
[ -s "$W/backup.tar" ] && ok "backup downloaded ($(stat -c%s "$W/backup.tar") bytes)" || bad "no backup"
post /api/restore --data-binary @"$W/backup.tar"
head -c 3000 /dev/urandom > "$W/junk"; post /api/restore --data-binary @"$W/junk"
post /api/credentials --data "user=admin&old=admin&password=admin2&password2=admin2"
curl -s -m 10 --digest -u admin:admin2 -H "X-Requested-With: lsc" -o /dev/null --data "user=admin&old=admin2&password=admin&password2=admin" "http://$H/api/credentials"
alive && ok "server alive after the POST sweep" || bad "server down after the POST sweep"

echo "== RTSP / RTSPS"
ff() { timeout 40 ffmpeg -nostdin -loglevel error "$@" -t 6 -f null - 2>"$W/ff.err"; local r=$?; [ $r = 0 ] && [ ! -s "$W/ff.err" ]; }
ff -rtsp_transport tcp -i "rtsp://admin:admin@$H:554/" && ok "RTSP TCP" || bad "RTSP TCP: $(head -2 "$W/ff.err")"
ff -rtsp_transport udp -i "rtsp://admin:admin@$H:554/" && ok "RTSP UDP" || bad "RTSP UDP: $(head -2 "$W/ff.err")"
curl -sk "${A[@]}" "https://$H/api/tls/ca.crt" -o "$W/ca.crt"
ff -rtsp_transport tcp -tls_verify 1 -ca_file "$W/ca.crt" -i "rtsps://admin:admin@$H:322/" && ok "RTSPS verified" || bad "RTSPS: $(head -2 "$W/ff.err")"
timeout 20 ffmpeg -nostdin -loglevel error -rtsp_transport tcp -i "rtsp://admin:wrong@$H:554/" -t 2 -f null - 2>/dev/null && bad "RTSP wrong password accepted" || ok "RTSP wrong password refused"
for i in 1 2 3 4; do timeout 40 ffmpeg -nostdin -loglevel error -rtsp_transport tcp -i "rtsp://admin:admin@$H:554/" -t 8 -f null - 2>/dev/null & done; wait
ok "4 parallel RTSP clients finished"
timeout 20 ffmpeg -nostdin -loglevel error -rtsp_transport tcp -i "rtsp://admin:admin@$H:554/sub" -t 3 -f null - 2>/dev/null; ok "RTSP /sub (no second stream on the PC) handled"
python3 -I "$T/tls/rtsps_udp.py" "$H" "$W/ca.crt" >"$W/x" 2>&1 && ok "RTSPS UDP refused (461)" || bad "rtsps_udp: $(tail -2 "$W/x")"

echo "== live view WebSocket + talk-back, terminal"
"$VENV/bin/python" -I "$T/tls/wsclient.py" "$H" 80 6 - admin:admin >"$W/ws" 2>&1 && ok "WebSocket live view: $(tail -1 "$W/ws")" || bad "wsclient: $(tail -3 "$W/ws")"

echo "== ONVIF (Home Assistant's library)"
timeout 180 "$VENV/bin/python" -I "$T/onvif/onviftest.py" "$H" admin admin >"$W/onvif" 2>&1
# motion events need the camera's motion detector (hardware): the only expected failure
if ! grep -qi traceback "$W/onvif" && [ "$(grep -o 'FAIL [a-z ]*' "$W/onvif" | grep -vc 'FAIL motion event delivered')" = 0 ]; then
  ok "onviftest: $(grep -o 'PASS' "$W/onvif" | wc -l) passed (motion event needs the camera)"
else bad "onviftest: $(grep -iE 'FAIL|Error' "$W/onvif" | head -3)"; fi
timeout 180 "$VENV/bin/python" -I "$T/onvif/ha_compat_test.py" "$H" admin admin >"$W/ha" 2>&1
grep -q "FAILURES: 0" "$W/ha" && ok "ha_compat: $(grep -c '^PASS' "$W/ha") passed" || bad "ha_compat: $(grep -E 'FAIL' "$W/ha" | head -3)"
timeout 60 "$VENV/bin/python" -I "$T/onvif/backchannel_test.py" "$H" admin admin 5 >"$W/bc" 2>&1
grep -q "sent [0-9]* RTP" "$W/bc" && ok "backchannel: $(tail -1 "$W/bc")" || bad "backchannel: $(tail -3 "$W/bc")"

echo "== recordings (tmpfs SD card)"
curl "${A[@]}" -o /dev/null --data "record.mode=continuous" "http://$H/api/config"
sleep 25
curl "${A[@]}" "http://$H/api/rec/all" -o "$W/rec.json"
# entries: {"d": day, "n": name, "s": size, "r": 1 while being recorded}; take a finished one
F=$(python3 -I -c 'import json,sys; l=[x for x in json.load(open(sys.argv[1])) if not x.get("r")]; print(l[0]["d"] + "/" + l[0]["n"] if l else "")' "$W/rec.json")
if [ -n "$F" ]; then
  ok "recording listed: $F"
  curl "${A[@]}" -o "$W/rec.mp4" "http://$H/rec/$F"
  ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$W/rec.mp4" > "$W/probe" 2>&1 && grep -q h264 "$W/probe" && ok "recording plays: $(tr '\n' ' ' < "$W/probe")" || bad "recording: $(head -2 "$W/probe")"
  post /api/rec/tar --data-urlencode "files=$F"
  curl "${A[@]}" -o /dev/null "http://$H/rec-tar/x"
  curl "${A[@]}" -o /dev/null -H "Range: bytes=100-200" "http://$H/rec/$F"
  curl "${A[@]}" -o /dev/null -H "Range: bytes=999999999-" "http://$H/rec/$F"
  curl "${A[@]}" -o /dev/null "http://$H/rec/../../../etc/passwd"
  c=$(code --data-urlencode "day=${F%%/*}" --data-urlencode "name=${F#*/}" "http://$H/api/rec/delete")
  curl "${A[@]}" "http://$H/api/rec/all" -o "$W/rec2.json"
  [ "$c" = 200 ] && ! grep -qF "\"n\":\"${F#*/}\"" "$W/rec2.json" && ok "recording deleted: $F" || bad "delete $F -> $c, still listed: $(grep -cF "${F#*/}" "$W/rec2.json")"
else bad "no recording listed: $(head -c 300 "$W/rec.json")"; fi
curl "${A[@]}" -o /dev/null --data "record.mode=off" "http://$H/api/config"

echo "== MQTT with the fake broker"
curl "${A[@]}" -o /dev/null --data "mqtt.host=127.0.0.1&mqtt.port=18830&mqtt.enabled=on&mqtt.discovery=on" "http://$H/api/config"
timeout 40 python3 -I "$T/events/fakebroker.py" 18830 25 "cam/cmd/image.jpeg_quality=70" >"$W/mqtt" 2>&1
grep -q "CONNECT" "$W/mqtt" && [ "$(grep -c '^PUB' "$W/mqtt")" -gt 5 ] && ok "MQTT: connected, $(grep -c '^PUB' "$W/mqtt") publishes, $(grep -c SUBSCRIBE "$W/mqtt") subscribe" || bad "MQTT: $(tail -3 "$W/mqtt")"
curl "${A[@]}" -o /dev/null --data "mqtt.enabled=off" "http://$H/api/config"

echo "== reset settings, reboot request (wrapped)"
post /api/reset-settings -X POST
alive && ok "server alive at the end" || bad "server down at the end"
rm -rf "$W"
echo "sweep: $n checks, $fails failed"
exit $fails
