#!/bin/bash
# HLS end-to-end checks against host_server; runs INSIDE run_sandboxed.sh
# (use tests/hls/run.sh). Camera = $CAM, login admin/admin.
#   hls_test.sh <work dir> [slow]     slow: valgrind build (longer waits)
# Prints PASS/FAIL lines; exit status = number of failures.
set -u
W=$1; SLOW=${2:-}
mkdir -p "$W"
H=${CAM:-127.0.0.1}; T=$(cd "$(dirname "$0")" && pwd)
A=(--digest -u admin:admin -s -m 60 -H "X-Requested-With: lsc")
PY=(python3 -I "$T/hls_check.py")
F=1; [ -n "$SLOW" ] && F=3                       # timeout factor under valgrind
fails=0; n=0
ok()  { n=$((n + 1)); echo "PASS $*"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "FAIL $*"; }
expect() { local want=$1 got=$2; shift 2; if [ "$got" = "$want" ]; then ok "$* -> $got"; else bad "$* -> $got (want $want)"; fi; }
code() { curl -s -o /dev/null -m $((30 * F)) -w "%{http_code}" "$@"; }
jget() { python3 -I -c 'import json,sys; d=json.load(sys.stdin); print(d[sys.argv[1]])' "$1"; }
status_ms() { curl "${A[@]}" -o /dev/null -w "%{time_total}" "http://$H/api/status" | awk '{printf "%d", $1 * 1000}'; }
hls_state() { curl "${A[@]}" "http://$H/api/hls"; }
plfield() { curl -s -m $((30 * F)) "$1" | sed -n "s/^#EXT-X-$2://p"; }
lastseg() { curl -s -m $((30 * F)) "$1" | sed -n 's/^seg\([0-9]*\)\.m4s$/\1/p' | tail -1; }
mem_poll() { while :; do hls_state >> "$W/mem.jsonl"; echo >> "$W/mem.jsonl"; sleep 1; done; }

echo "== token flow and access checks"
curl "${A[@]}" -o /dev/null --data "hls.enabled=off" "http://$H/api/config"   # the build dir may keep it from a run before
expect 401 "$(code "http://$H/api/hls/token")" "token without login"
expect 403 "$(code --digest -u admin:admin "http://$H/api/hls/token")" "token without X-Requested-With"
expect 403 "$(code "${A[@]}" "http://$H/api/hls/token")" "token while hls.enabled=off"
expect 403 "$(code "http://$H/hls/0123456789abcdef0123456789abcdef/index.m3u8")" "playlist while off"
expect off "$(curl "${A[@]}" "http://$H/api/config?prefix=hls." | jget hls.enabled)" "hls.enabled off (default: test_hls_unit)"
curl "${A[@]}" -o /dev/null --data "hls.enabled=on" "http://$H/api/config"
expect on "$(curl "${A[@]}" "http://$H/api/config?prefix=hls." | jget hls.enabled)" "hls.enabled after POST /api/config"
J=$(curl "${A[@]}" "http://$H/api/hls/token")
TOK=$(echo "$J" | jget token); PATH_=$(echo "$J" | jget url)
[ "${#TOK}" = 32 ] && [ "$PATH_" = "/hls/$TOK/index.m3u8" ] && ok "token JSON $J" || bad "token JSON $J"
URL="http://$H$PATH_"; B="http://$H/hls/$TOK"
RND=$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')
expect 403 "$(code "http://$H/hls/$RND/index.m3u8")" "unknown token"
expect 404 "$(code "http://$H/hls/${TOK^^}/index.m3u8")" "upper-case token"
expect 404 "$(code "http://$H/hls/${TOK:0:31}/index.m3u8")" "short token"
expect 404 "$(code "$B/seg01.m4s")" "segment number with a leading zero"
expect 404 "$(code "$B/seg99999999999.m4s")" "segment number > 32 bits"
expect 404 "$(code "$B/index.m3u")" "unknown file"
expect 404 "$(code --path-as-is "$B/../$TOK/index.m3u8")" "dot-dot path"
expect 401 "$(code -X POST "$URL")" "POST to a playlist (Digest area)"
expect 421 "$(code -H "Host: rebind.example" "$URL")" "foreign Host header (DNS rebinding)"

echo "== cold start (producer starts on the first request), web stays responsive"
t0=$(date +%s%N)
curl -s -m $((30 * F)) -D "$W/pl.hdr" -o "$W/pl0.m3u8" "$URL" &
CP=$!
sleep 0.5
ms=$(status_ms); [ "$ms" -lt $((1500 * F)) ] && ok "/api/status during the cold-start wait: $ms ms" || bad "/api/status took $ms ms during the cold-start wait"
wait $CP
cold=$(( ($(date +%s%N) - t0) / 1000000 ))
grep -q '^seg' "$W/pl0.m3u8" && ok "first playlist after $cold ms: $(grep -c '^seg' "$W/pl0.m3u8") segments" || bad "first playlist: $(head -c 200 "$W/pl0.m3u8")"
grep -qi '^Access-Control-Allow-Origin: \*' "$W/pl.hdr" && ok "CORS header on /hls/" || bad "no CORS header on /hls/"
curl "${A[@]}" -D "$W/st.hdr" -o /dev/null "http://$H/api/status"
grep -qi '^Access-Control-Allow-Origin' "$W/st.hdr" && bad "CORS header on /api/status" || ok "no CORS header on /api/ paths"
r=$("${PY[@]}" get "$B/init.mp4"); [[ "$r" == "200 video/mp4 * "* ]] && ok "init.mp4: $r" || bad "init.mp4: $r"
expect 404 "$(code "$B/seg1000000.m4s")" "segment not in the ring"

echo "== playback: ffmpeg 35 s + emulated live client (playlist / segment / timeline checks)"
mem_poll & MP=$!
timeout $((80 * F)) ffmpeg -nostdin -v warning -i "$URL" -t 35 -c copy -y "$W/ff1.mp4" 2> "$W/ff1.err" &
FP=$!
"${PY[@]}" client "$URL" 40 single && ok "live client 40 s" || bad "live client 40 s"
wait $FP && ok "ffmpeg HLS exit 0" || bad "ffmpeg HLS exit $?"
"${PY[@]}" ffprobe "$W/ff1.mp4" 33 && ok "ffmpeg recording timeline" || bad "ffmpeg recording timeline"
grep -v "duplicated MOOV\|Last message repeated" "$W/ff1.err" > "$W/ff1.err2"   # ffmpeg says that for any live fMP4 HLS (its own too)
[ ! -s "$W/ff1.err2" ] && ok "ffmpeg: no warnings" || bad "ffmpeg warnings: $(head -3 "$W/ff1.err2")"
# (the PC feeder runs at 15.6 fps while the SPS says 15: keep the real timestamps)
dec=$(ffmpeg -nostdin -v error -i "$W/ff1.mp4" -fps_mode passthrough -enc_time_base 1:90000 -f null - 2>&1 | head -3)
[ -z "$dec" ] && ok "recording decodes without errors" || bad "decode errors: $dec"

echo "== concurrent: 3 live clients + ffmpeg HLS + RTSP + WebSocket viewer (30 s)"
pids=()
for i in 1 2 3; do "${PY[@]}" client "$URL" 30 "c$i" > "$W/c$i.txt" 2>&1 & pids+=($!); done
timeout $((70 * F)) ffmpeg -nostdin -v warning -i "$URL" -t 30 -c copy -y "$W/ff2.mp4" 2> "$W/ff2.err" & pids+=($!)
timeout $((70 * F)) ffmpeg -nostdin -v warning -rtsp_transport tcp -i "rtsp://admin:admin@$H:554/" -t 30 -c copy -y "$W/rtsp.mkv" 2> "$W/rtsp.err" & pids+=($!)
python3 -I "$T/../tls/wsclient.py" "$H" 80 30 > "$W/ws.txt" 2>&1 & pids+=($!)
worst=0
for i in $(seq 1 10); do sleep 2; ms=$(status_ms); [ "$ms" -gt "$worst" ] && worst=$ms; done
[ "$worst" -lt $((1500 * F)) ] && ok "web page requests stay fast during the load (worst /api/status $worst ms)" || bad "slow web during the load: $worst ms"
for p in "${pids[@]}"; do wait "$p"; done
for i in 1 2 3; do grep -q "^PASS" "$W/c$i.txt" && ok "$(head -1 "$W/c$i.txt")" || bad "$(cat "$W/c$i.txt")"; done
"${PY[@]}" ffprobe "$W/ff2.mp4" 28 && ok "ffmpeg HLS client under load" || bad "ffmpeg HLS client under load"
fr=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$W/rtsp.mkv" 2>/dev/null)
[ "${fr:-0}" -ge 380 ] && ok "RTSP client alongside: $fr frames in 30 s" || bad "RTSP client: ${fr:-none} frames ($(head -2 "$W/rtsp.err"))"
v=$(sed -n 's/.*video=\([0-9]*\).*/\1/p' "$W/ws.txt")
[ "${v:-0}" -ge 380 ] && ok "WebSocket viewer alongside: $(cat "$W/ws.txt")" || bad "WebSocket viewer: $(cat "$W/ws.txt")"

echo "== hls.js in Google Chrome (cross-origin page: needs the CORS header)"
if [ -n "${HLSJS:-}" ] && [ -n "${PW_DIR:-}" ] && [ -x "${CHROME:-/usr/bin/google-chrome-stable}" ]; then
  r=$(HOME=$W timeout $((90 * F)) node "$T/hlsjs_test.js" "$URL" 30 2>&1 | tail -1)
  [[ "$r" == PASS* ]] && ok "$r" || bad "$r"
else echo "SKIP (set HLSJS=<hls.min.js> PW_DIR=<node_modules with playwright> [CHROME=...])"; fi

echo "== slow client: segment dropped from the ring while being sent"
# small TCP send buffers (this network namespace only), else the kernel takes the
# whole segment at once and the server never has to wait for this client
WMEM=$(cat /proc/sys/net/ipv4/tcp_wmem)
echo "4096 8192 16384" > /proc/sys/net/ipv4/tcp_wmem 2>/dev/null || echo "note: cannot shrink tcp_wmem"
S=$(lastseg "$URL")
( "${PY[@]}" client "$URL" 25 beside > "$W/beside.txt" 2>&1 ) &
BP=$!
"${PY[@]}" slow "$B/seg$S.m4s" 8000 > "$W/slow.txt" 2>&1 &
SP=$!
sleep 4
ms=$(status_ms); [ "$ms" -lt $((1500 * F)) ] && ok "/api/status while a slow client holds a segment: $ms ms" || bad "/api/status $ms ms with a slow client"
wait $SP; read -r _ _ got _ total _ < "$W/slow.txt"
echo "$WMEM" > /proc/sys/net/ipv4/tcp_wmem 2>/dev/null
[ "${got:-0}" -gt 0 ] && [ "${got:-0}" -lt "${total:-0}" ] && ok "slow client cut short after the ring moved on: $(cat "$W/slow.txt")" || bad "slow client: $(cat "$W/slow.txt")"
wait $BP
grep -q "^PASS" "$W/beside.txt" && ok "client beside the slow one: $(head -1 "$W/beside.txt")" || bad "$(cat "$W/beside.txt")"

echo "== HTTPS and the HTTP -> HTTPS redirect"
for i in $(seq 1 $((50 * F))); do curl -sk -o /dev/null "https://$H/" && break; sleep 0.2; done
r=$(curl -sk -m $((30 * F)) -o /dev/null -w "%{http_code} %{content_type}" "https://$H$PATH_")
expect "200 application/vnd.apple.mpegurl" "$r" "playlist over HTTPS"
curl "${A[@]}" -o "$W/ca.crt" "http://$H/api/tls/ca.crt"
HLS_CA=$W/ca.crt "${PY[@]}" client "https://$H$PATH_" 12 https && ok "live client over HTTPS (camera CA verified)" || bad "live client over HTTPS"
# ffmpeg's HLS demuxer does not pass -tls_verify/-ca_file on to segment requests: trust
# the camera CA the way a user would (system anchors; only inside this mount namespace)
mkdir -p "$W/anchors" && cp "$W/ca.crt" "$W/anchors/" &&
if mount --bind "$W/anchors" /etc/ca-certificates/trust-source/anchors 2>/dev/null; then
  timeout $((60 * F)) ffmpeg -nostdin -v error -i "https://$H$PATH_" -t 8 -c copy -y "$W/ffs.mp4" 2> "$W/ffs.err"
  "${PY[@]}" ffprobe "$W/ffs.mp4" 6 && ok "ffmpeg over HTTPS" || bad "ffmpeg over HTTPS: $(head -2 "$W/ffs.err")"
  umount /etc/ca-certificates/trust-source/anchors
else echo "SKIP ffmpeg over HTTPS (cannot add a trust anchor here)"; fi
curl "${A[@]}" -o /dev/null --data "redirect=1" "http://$H/api/tls"
loc=$(curl -s -m $((30 * F)) -o /dev/null -w "%{http_code} %{redirect_url}" "$URL")
expect "301 https://$H$PATH_" "$loc" "redirect on: HTTP playlist"
expect 200 "$(curl -skL -m $((30 * F)) -o /dev/null -w "%{http_code}" "$URL")" "redirect followed"
curl -sk --digest -u admin:admin -m 60 -H "X-Requested-With: lsc" -o /dev/null --data "redirect=0" "https://$H/api/tls"
expect 200 "$(code "$URL")" "redirect off again: HTTP playlist"

echo "== memory bound"
kill $MP 2>/dev/null; wait $MP 2>/dev/null
python3 -I - "$W/mem.jsonl" <<'EOF' && ok "$(cat "$W/mem.txt")" || bad "$(cat "$W/mem.txt")"
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip().startswith("{")]
peak = max(r["peak_bytes"] for r in rows); lim = rows[0]["limit_bytes"]
seg = max(r["bytes"] for r in rows); n = max(r["segments"] for r in rows)
msg = f"HLS memory: peak {peak / 1e6:.2f} MB (limit {lim / 1e6:.2f} MB), ring max {seg / 1e6:.2f} MB in {n} segments, {len(rows)} samples"
open(sys.argv[1].replace("mem.jsonl", "mem.txt"), "w").write(msg)
sys.exit(0 if peak <= lim and n <= 5 and seg <= 4 << 20 else 1)
EOF

echo "== idle stop (30 s without requests) and restart"
S1=$(lastseg "$URL")
read -r TF1 D1 <<< "$("${PY[@]}" tfdt "$B/seg$S1.m4s")"
SESS1=$(hls_state | jget session)
stopped=""
for i in $(seq 1 $((60 * F))); do
  sleep 1
  [ "$(hls_state | jget running)" = False ] && { stopped=$i; break; }
done
[ -n "$stopped" ] && [ "$stopped" -ge 25 ] && ok "producer stopped ${stopped} s after the last request" || bad "producer stop after ${stopped:-never} s"
st=$(hls_state)
[ "$(echo "$st" | jget segments)" = 0 ] && [ "$(echo "$st" | jget bytes)" = 0 ] && [ "$(echo "$st" | jget init)" = 0 ] && ok "ring freed: $st" || bad "ring not freed: $st"
ms=$(plfield "$URL" MEDIA-SEQUENCE)
[ -n "$ms" ] && [ "$ms" -gt "$S1" ] && ok "restart: MEDIA-SEQUENCE $ms continues after $S1" || bad "restart: MEDIA-SEQUENCE '$ms' after $S1"
[ "$(hls_state | jget session)" -gt "$SESS1" ] && ok "new producer session" || bad "no new session"
read -r TF2 _ <<< "$("${PY[@]}" tfdt "$B/seg$ms.m4s")"
[ "$TF2" -gt $((TF1 + D1)) ] && ok "timeline continues forward: tfdt $TF2 > $((TF1 + D1))" || bad "timeline: tfdt $TF2 after $((TF1 + D1))"
"${PY[@]}" client "$URL" 12 restarted && ok "playback after the restart" || bad "playback after the restart"

echo "== live.mp4: one endless fragmented MP4 (low latency, what casting uses by default)"
SLACK=0.6; [ "$F" != 1 ] && SLACK=2
TM=$(curl "${A[@]}" "http://$H/api/hls/token" | jget token); M="http://$H/hls/$TM/live.mp4"
"${PY[@]}" livemp4 "$M" 12 direct "$W/live.mp4" "$SLACK" && ok "live.mp4: keyframe first, one frame per fragment, keeps pace with real time" || bad "live.mp4 stream"
"${PY[@]}" ffprobe "$W/live.mp4" 8 && ok "ffprobe live.mp4" || bad "ffprobe live.mp4"
expect 403 "$(code "http://$H/hls/0123456789abcdef0123456789abcdef/live.mp4")" "live.mp4 with an unknown token"
curl -s -o /dev/null -m $((10 * F)) "$M" & L1=$!
curl -s -o /dev/null -m $((10 * F)) "$M" & L2=$!
for i in $(seq 1 $((10 * F))); do [ "$(hls_state | jget mp4_streams)" = 2 ] && break; sleep 0.5; done
expect 2 "$(hls_state | jget mp4_streams)" "two live.mp4 streams counted"
expect 503 "$(curl -s -o /dev/null -w "%{http_code}" -m 5 "$M")" "third live.mp4 stream refused"
wait $L1 $L2 2>/dev/null
for i in $(seq 1 $((10 * F))); do [ "$(hls_state | jget mp4_streams)" = 0 ] && break; sleep 0.5; done
expect 0 "$(hls_state | jget mp4_streams)" "streams released after the clients left"
curl -s -o /dev/null -m 60 "$M" & L1=$!
sleep 3; t0=$(date +%s)
curl "${A[@]}" -o /dev/null --data "hls.enabled=off" "http://$H/api/config"
wait $L1 2>/dev/null; t1=$(date +%s)
[ $((t1 - t0)) -le $((3 * F)) ] && ok "hls.enabled=off ends a running live.mp4 ($((t1 - t0)) s)" || bad "live.mp4 still running $((t1 - t0)) s after hls.enabled=off"
curl "${A[@]}" -o /dev/null --data "hls.enabled=on" "http://$H/api/config"
expect mp4 "$(hls_state | jget cast_format)" "hls.cast_format defaults to mp4"
expect 400 "$(code "${A[@]}" --data "hls.cast_format=webm" "http://$H/api/config")" "hls.cast_format=webm refused"

if [ "$(curl "${A[@]}" -o /dev/null -w "%{http_code}" "http://$H/api/hls/cast")" != 404 ]; then   # cast.c is optional
echo "== casting from the camera to a (fake) Chromecast: mDNS discovery, Cast v2 over TLS"
cast_wait() {   # poll GET /api/hls/cast until not busy; prints the state JSON
  local j
  for i in $(seq 1 $((60 * F))); do
    j=$(curl "${A[@]}" "http://$H/api/hls/cast"); [ "$(echo "$j" | jget busy)" = False ] && break; sleep 1
  done; echo "$j"
}
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$W/fc.key" -out "$W/fc.crt" -days 2 -subj /CN=fakecast 2>/dev/null
: > "$W/fc.log"; : > "$W/fc2.log"; : > "$W/fc3.log"; : > "$W/fc4.log"; : > "$W/fc5.log"
export LIVEMP4_SLACK=$SLACK
python3 -I "$T/fake_cast.py" 8009 "$W/fc.crt" "$W/fc.key" "$W/fc.log" --mdns "$H" --launch-delay 5 --preroll 4 --play-secs 40 > "$W/fc.out" 2>&1 & FC=$!
python3 -I "$T/fake_cast.py" 8010 "$W/fc.crt" "$W/fc.key" "$W/fc2.log" --behavior load_failed > "$W/fc2.out" 2>&1 & FC2=$!
python3 -I "$T/fake_cast.py" 8012 "$W/fc.crt" "$W/fc.key" "$W/fc3.log" --behavior mp4_refused > "$W/fc3.out" 2>&1 & FC3=$!
python3 -I "$T/fake_cast.py" 8013 "$W/fc.crt" "$W/fc.key" "$W/fc4.log" --behavior nocatchup --preroll 4 --play-secs 30 > "$W/fc4.out" 2>&1 & FC4=$!
python3 -I "$T/fake_cast.py" 8014 "$W/fc.crt" "$W/fc.key" "$W/fc5.log" --behavior noseek --preroll 4 --play-secs 30 > "$W/fc5.out" 2>&1 & FC5=$!
sleep 1
expect 202 "$(code "${A[@]}" -X POST "http://$H/api/hls/cast/discover")" "start discovery"
j=$(cast_wait)
echo "$j" | python3 -I -c '
import json, sys
d = json.load(sys.stdin); dev = {(x["host"], x["port"]): x for x in d["devices"]}
tv = dev.get((sys.argv[1], 8009)); grp = dev.get((sys.argv[1], 32187))
ok = tv and tv["name"] == "Living  Room  TV" and tv["model"] == "Chromecast" and grp and grp["name"].startswith("All speakers")
sys.exit(0 if ok and d["state"] == "idle" else 1)' "$H" && ok "discovery: $j" || bad "discovery: $j"
expect 400 "$(code "${A[@]}" --data "host=tv.local" "http://$H/api/hls/cast")" "cast to a host name (IPv4 only)"
receiver_check() {   # <fake_cast log> <mp4|hls|fallback>: what the fake device saw and did
  for i in $(seq 1 $((40 * F))); do grep -q '"ev": "played"' "$1" && break; sleep 1; done
  python3 -I - "$1" "$H" "$F" "$2" <<'EOF'
import json, re, sys
log, host, f, mode = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
ev = [json.loads(l) for l in open(log)]
loads = [e for e in ev if e["ev"] == "load"]; load = loads[-1] if loads else {}; m = load.get("media", {})
fetched = next((e for e in ev if e["ev"] == "fetched"), {}); played = next((e for e in ev if e["ev"] == "played"), {})
msgs = [(e["ns"].rsplit(".", 1)[1], e["type"]) for e in ev if e["ev"] == "msg"]
mp4 = mode == "mp4"
url_ok = re.fullmatch(r"http://%s/hls/[0-9a-f]{32}/%s" % (re.escape(host), r"live\.mp4" if mp4 else r"index\.m3u8"), m.get("contentId", "")) is not None
checks = {"tls": any(e["ev"] == "connected" for e in ev), "launch": ("receiver", "LAUNCH") in msgs,
          "pong": ("heartbeat", "PONG") in msgs, "connect app": load.get("dst") == "web-7",
          "url": url_ok, "type": m.get("contentType") == ("video/mp4" if mp4 else "application/x-mpegurl"),
          "live": m.get("streamType") == "LIVE",
          "live edge (no currentTime)": "currentTime" not in load.get("keys", ["currentTime"]),
          "fetched": fetched.get("segment", 0) > 0 and fetched.get("cors") == "*", "played": played.get("result", "").startswith("PASS")}
if mp4:
    checks["first frame soon"] = 0 <= fetched.get("first_ms", 99999) < 3500 * f      # next keyframe (GOP 2 s)
else:
    checks["fmp4"] = m.get("hlsSegmentFormat") == "fmp4" and m.get("hlsVideoSegmentFormat") == "fmp4"
if mode == "hls":
    checks["playlist ready (warm)"] = 0 <= fetched.get("playlist_ms", 99999) < 1500 * f
if mode == "fallback":
    checks["tried mp4 first"] = len(loads) == 2 and loads[0]["media"].get("contentType") == "video/mp4"
    checks["same session"] = len(loads) == 2 and loads[0]["sessionId"] == loads[1]["sessionId"]
bad = [k for k, v in checks.items() if not v]
print(("all ok: " + ", ".join(checks)) if not bad else f"failed {bad}; {played.get('result', '')[:300]}")
sys.exit(1 if bad else 0)
EOF
}
cold_start() {      # no HLS producer running
  curl "${A[@]}" -o /dev/null --data "hls.enabled=off" "http://$H/api/config"
  for i in $(seq 1 $((10 * F))); do [ "$(hls_state | jget running)" = False ] && break; sleep 1; done
  curl "${A[@]}" -o /dev/null --data "hls.enabled=on" "http://$H/api/config"
}
# 1. default: the low latency live.mp4
cold_start
expect False "$(hls_state | jget running)" "producer stopped before casting"
expect 202 "$(code "${A[@]}" --data "host=$H" "http://$H/api/hls/cast")" "cast to $H:8009 (live.mp4)"
j=$(cast_wait)
[ "$(echo "$j" | jget state)" = playing ] && [[ "$j" == *"low latency MP4"* ]] && ok "cast state: $j" || bad "cast state: $j"
r=$(receiver_check "$W/fc.log" mp4) && ok "receiver side (mp4): $r" || bad "receiver side (mp4): $r"
[[ "$j" == *"played faster for a moment"* ]] && echo "$j" | python3 -I -c 'import json, re, sys
m = re.search(r"([0-9.]+) s behind -> ([0-9.]+) s", json.load(sys.stdin)["message"]); sys.exit(0 if m and float(m[1]) > 3 and float(m[2]) <= 1.5 else 1)' \
  && ok "catch-up: from the receiver's start-up buffer to ~1 s behind by playing faster: $(echo "$j" | jget message)" || bad "catch-up: $j"
python3 -I -c 'import json, sys
p = [json.loads(l) for l in open(sys.argv[1]) if "\"played\"" in l][-1]
sys.exit(0 if p.get("rates") == [1.5, 1.2, 1.0] and not p.get("seeks") else 1)' "$W/fc.log" \
  && ok "rates 1.5 -> 1.2 -> 1.0 once really playing, no SEEK (an endless stream is not seekable)" || bad "seeks / rates: $(grep '"played"' "$W/fc.log" | tail -1)"
expect True "$(curl "${A[@]}" "http://$H/api/hls/cast" | python3 -I -c 'import json, sys
t = json.load(sys.stdin)["trace"]; ev = [e[4] for e in t]; st = [e[1] for e in t]
print(all(x in ev for x in ("connect", "load mp4", "rate 1.5", "rate 1")) and "BUFFERING" in st and "PLAYING" in st)')" "trace: connect, load, start-up BUFFERING, PLAYING, rates"
expect False "$(hls_state | jget running)" "an MP4 cast does not start the HLS producer"
CT=$(python3 -I -c 'import json, re, sys
u = [json.loads(l) for l in open(sys.argv[1])]; u = [e["media"]["contentId"] for e in u if e["ev"] == "load"][-1]
print(re.search(r"/hls/([0-9a-f]{32})/", u).group(1))' "$W/fc.log")
c=$(curl -s -o "$W/body" -w "%{http_code}" -m 3 "http://127.0.0.1/hls/$CT/live.mp4")
[ "$c" = 403 ] && grep -q "invalid or expired HLS token" "$W/body" && ok "cast URL refused from another address (bound to the device)" || bad "cast URL from another address: $c $(head -c 80 "$W/body")"
expect 200 "$(curl -s -o /dev/null -w "%{http_code}" -m 3 "http://$H/hls/$CT/live.mp4")" "cast URL works from the device's address"
expect 202 "$(code "${A[@]}" --data "host=$H" "http://$H/api/hls/cast/stop")" "stop casting"
j=$(cast_wait)
[ "$(echo "$j" | jget state)" = stopped ] && grep -q '"ev": "stop", "sessionId": "[^"]*", "matched": true' "$W/fc.log" && ok "stopped: $j" || bad "stop: $j"
expect 403 "$(curl -s -o /dev/null -w "%{http_code}" -m 3 "http://$H/hls/$CT/live.mp4")" "cast URL revoked by Stop"
# 2. hls.cast_format=hls: playlist warmed up while the receiver app starts
curl "${A[@]}" -o /dev/null --data "hls.cast_format=hls" "http://$H/api/config"
: > "$W/fc.log"; cold_start
expect False "$(hls_state | jget running)" "producer stopped before casting"
expect 202 "$(code "${A[@]}" --data "host=$H" "http://$H/api/hls/cast")" "cast to $H:8009 (HLS)"
sleep 2                                         # the fake receiver app takes 5 s to start
expect True "$(hls_state | jget running)" "producer warmed up while the receiver app starts"
j=$(cast_wait)
[ "$(echo "$j" | jget state)" = playing ] && [[ "$j" == *"(HLS)"* ]] && ok "cast state: $j" || bad "cast state: $j"
r=$(receiver_check "$W/fc.log" hls) && ok "receiver side (hls): $r" || bad "receiver side (hls): $r"
curl "${A[@]}" -o /dev/null --data "hls.cast_format=mp4" "http://$H/api/config"
# 3. a receiver that cannot play live.mp4: HLS in the same session
expect 202 "$(code "${A[@]}" --data "host=$H&port=8012" "http://$H/api/hls/cast")" "cast to a receiver that refuses MP4"
j=$(cast_wait)
[ "$(echo "$j" | jget state)" = playing ] && [[ "$j" == *"(HLS)"* ]] && ok "fell back to HLS: $j" || bad "fallback: $j"
r=$(receiver_check "$W/fc3.log" fallback) && ok "receiver side (fallback): $r" || bad "receiver side (fallback): $r"
# 4. a receiver that refuses a faster playback rate: reported, nothing else tried
expect 202 "$(code "${A[@]}" --data "host=$H&port=8013" "http://$H/api/hls/cast")" "cast to a receiver that refuses SET_PLAYBACK_RATE"
j=$(cast_wait)
[[ "$j" == *"refused to play faster"* ]] && ok "rate refused reported: $(echo "$j" | jget message)" || bad "rate refused: $j"
for i in $(seq 1 $((40 * F))); do grep -q '"ev": "played"' "$W/fc4.log" && break; sleep 1; done
nrate=$(grep -c '"type": "SET_PLAYBACK_RATE"' "$W/fc4.log"); nseek=$(grep -c '"type": "SEEK"' "$W/fc4.log")
[ "$nrate" = 1 ] && [ "$nseek" = 0 ] && ok "one rate request, no retry, no SEEK" || bad "after a refusal: $nrate rate, $nseek seek requests"
# 5. Stop while catching up: accepted at once, normal speed restored, receiver stopped
expect 202 "$(code "${A[@]}" --data "host=$H&port=8014" "http://$H/api/hls/cast")" "cast to $H:8014"
for i in $(seq 1 $((80 * F))); do grep -q '"type": "SET_PLAYBACK_RATE"' "$W/fc5.log" && break; sleep 0.5; done
expect 202 "$(code "${A[@]}" --data "host=$H&port=8014" "http://$H/api/hls/cast/stop")" "Stop while catching up is accepted"
j=$(cast_wait)
nrate=$(grep -c '"type": "SET_PLAYBACK_RATE"' "$W/fc5.log")    # not "n": the PASS/FAIL counter
[ "$(echo "$j" | jget state)" = stopped ] && [ "$nrate" = 2 ] && ok "catch-up cancelled: rate set back to 1 ($nrate rate messages), receiver stopped" || bad "cancel: $nrate rate messages, $j"
expect 202 "$(code "${A[@]}" --data "host=$H&port=8010" "http://$H/api/hls/cast")" "cast to a receiver that fails the LOAD"
j=$(cast_wait); [[ "$(echo "$j" | jget state)" == error && "$j" == *"could not play"* ]] && ok "LOAD_FAILED reported: $j" || bad "LOAD_FAILED: $j"
expect 202 "$(code "${A[@]}" --data "host=$H&port=8011" "http://$H/api/hls/cast")" "cast to a closed port"
j=$(cast_wait); [[ "$(echo "$j" | jget state)" == error && "$j" == *"cannot connect"* ]] && ok "refused connection reported: $j" || bad "closed port: $j"
curl "${A[@]}" -o /dev/null --data "redirect=1" "http://$H/api/tls"
curl -sk --digest -u admin:admin -m 60 -H "X-Requested-With: lsc" -o /dev/null --data "host=$H" "https://$H/api/hls/cast"
j=$(curl -sk --digest -u admin:admin -m 60 "https://$H/api/hls/cast"); for i in $(seq 1 10); do [[ "$j" == *'"busy":false'* ]] && break; sleep 1; j=$(curl -sk --digest -u admin:admin -m 60 "https://$H/api/hls/cast"); done
[[ "$j" == *redirect* ]] && ok "redirect on: cast refused with a reason" || bad "redirect on: $j"
curl -sk --digest -u admin:admin -m 60 -H "X-Requested-With: lsc" -o /dev/null --data "redirect=0" "https://$H/api/tls"
kill $FC $FC2 $FC3 $FC4 $FC5 2>/dev/null; wait $FC $FC2 $FC3 $FC4 $FC5 2>/dev/null
else echo "SKIP casting: cast.c not built into this firmware"; fi

echo "== switching off revokes every URL"
curl "${A[@]}" -o /dev/null --data "hls.enabled=off" "http://$H/api/config"
expect 403 "$(code "$URL")" "playlist after hls.enabled=off"
sleep 2
expect False "$(hls_state | jget running)" "producer after hls.enabled=off"
curl "${A[@]}" -o /dev/null --data "hls.enabled=on" "http://$H/api/config"
expect 403 "$(code "$URL")" "old token after off/on"
T2=$(curl "${A[@]}" "http://$H/api/hls/token" | jget token)
expect 200 "$(code "http://$H/hls/$T2/index.m3u8")" "new token"
curl "${A[@]}" -o /dev/null --data "hls.enabled=off" "http://$H/api/config"
sleep 2

echo "hls_test: $((n - fails))/$n passed, $fails failed"
exit $fails
