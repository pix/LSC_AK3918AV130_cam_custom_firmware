#!/bin/bash
# Push notification checks; runs INSIDE tests/host/run_sandboxed.sh (see run.sh).
#   suite.sh <build dir> <work dir> [asan|valgrind|plain]
# Mock servers (mock_push.py) listen on 127.0.0.1; "push.test" resolves to it
# from a private hosts file. Events are simulated through host_server's FIFO
# (<build>/root/cfg/sim_events), so they take the real path through events.c.
set -u
B=$1; W=$2; MODE=${3:-asan}; SLOW=1; [ "$MODE" = valgrind ] && SLOW=3
T=$(cd "$(dirname "$0")" && pwd)
H=${CAM:-127.0.0.1}
A=(--digest -u admin:admin -m 30 -s -H "X-Requested-With: lsc")
FIFO=$B/root/cfg/sim_events
P=$W/pki
fails=0; n=0
ok()  { n=$((n + 1)); echo "PASS $*"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "FAIL $*"; }
chk() { local d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }

# ---- isolation: names only from our hosts file, never systemd-resolved / DNS ----
printf '127.0.0.1 localhost push.test\n' > "$W/hosts"
printf 'hosts: files\n' > "$W/nsswitch.conf"
mount --bind "$W/hosts" /etc/hosts && mount --bind "$W/nsswitch.conf" /etc/nsswitch.conf || { echo "FAIL hosts setup"; exit 1; }
[ -d /run/systemd/resolve ] && mount -t tmpfs tmpfs /run/systemd/resolve
getent hosts push.test | grep -q 127.0.0.1 || { echo "FAIL push.test does not resolve"; exit 1; }
if getent hosts ntfy.sh >/dev/null 2>&1 || getent hosts api.telegram.org >/dev/null 2>&1; then echo "FAIL real names resolve"; exit 1; fi
ip route | grep -q default && { echo "FAIL the sandbox has a default route"; exit 1; }

# ---- helpers ----
mock() {   # name port [args]: start a stand-in server, wait until it listens
  local name=$1 port=$2; shift 2
  rm -rf "$W/m_$name"
  python3 -I "$T/mock_push.py" --port "$port" --out "$W/m_$name" "$@" > "$W/m_$name.log" 2>&1 &
  echo $! > "$W/m_$name.pid"
  for i in $(seq 1 50); do grep -q listening "$W/m_$name.log" 2>/dev/null && return 0; sleep 0.1; done
  echo "mock $name did not start: $(cat "$W/m_$name.log")"; return 1
}
tlsmock() { local name=$1 port=$2; shift 2; mock "$name" "$port" --tls "$P/srv.crt" "$P/srv.key" "$@"; }
unmock() { kill "$(cat "$W/m_$1.pid")" 2>/dev/null; wait "$(cat "$W/m_$1.pid")" 2>/dev/null; }
# q <mock> <python expression over R = list of request records>
q() { python3 -I -c 'import json,os,sys
f=sys.argv[1]; R=[json.loads(l) for l in open(f)] if os.path.exists(f) else []
ok=False
try: ok=bool(eval(sys.argv[2]))
except Exception as e: print("   (", type(e).__name__, e, ")")
if not ok: print("   requests:", json.dumps([{k:r.get(k) for k in ("method","path","status","headers","fields","payload","json","files","title","message","error")} for r in R])[:1500])
sys.exit(0 if ok else 1)' "$W/m_$1/requests.jsonl" "$2"; }
nreq() { [ -f "$W/m_$1/requests.jsonl" ] && wc -l < "$W/m_$1/requests.jsonl" || echo 0; }
saved() { python3 -I -c 'import json,sys
R=[json.loads(l) for l in open(sys.argv[1])]
print(R[int(sys.argv[2])]["files"][0]["saved"])' "$W/m_$1/requests.jsonl" "$2"; }
cfg() {    # name=value ... through POST /api/config
  local a=() kv c
  for kv in "$@"; do a+=(--data-urlencode "$kv"); done
  c=$(curl "${A[@]}" -o "$W/cfg.out" -w '%{http_code}' "${a[@]}" "http://$H/api/config")
  [ "$c" = 200 ] || { echo "   config $* -> $c $(cat "$W/cfg.out")"; return 1; }
}
cfg_bad() { local c; c=$(curl "${A[@]}" -o "$W/cfg.out" -w '%{http_code}' --data-urlencode "$1" "http://$H/api/config"); [ "$c" = 400 ]; }
st() { curl "${A[@]}" "http://$H/api/notify" > "$W/st.json"; }
sj() { st; python3 -I -c 'import json,sys
s=json.load(open(sys.argv[1])); sys.exit(0 if eval(sys.argv[2]) else 1)' "$W/st.json" "$1"; }
sv() { python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$W/st.json" "$1"; }
wait_for() { local t=$(( $1 * SLOW )) i; for i in $(seq 1 $((t * 4))); do sj "$2" && return 0; sleep 0.25; done; echo "   timeout waiting for $2: $(head -c 900 "$W/st.json")"; return 1; }
idle() { wait_for "${1:-90}" 's["queued"]==0'; }
ev() { echo "$1 1 ${2:-0}" > "$FIFO"; sleep 0.3; echo "$1 0" > "$FIFO"; sleep 0.2; }
jpeg_ok() { [ -s "$1" ] && ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$1" 2>/dev/null | grep -qx mjpeg &&
            ffmpeg -nostdin -v error -i "$1" -f null - > "$W/ff.err" 2>&1 && [ ! -s "$W/ff.err" ]; }
clip_ok() {  # file min_s max_s: H.264, decodes without errors, duration in range
  [ -s "$1" ] || return 1
  local info; info=$(ffprobe -v error -show_entries stream=codec_name:format=duration -of csv=p=0 "$1" 2>&1 | tr '\n' ' ')
  echo "   clip $(stat -c %s "$1") bytes: $info"
  echo "$info" | grep -q h264 || return 1
  # decode every frame. Under valgrind the simulated encoder and the clip reader fall behind (frames
  # a few ms apart, the hub skips ahead to newer keyframes): the null muxer then notes colliding output
  # timestamps - an output-side note, not a decoding error, so those lines are ignored.
  ffmpeg -nostdin -v error -i "$1" -fps_mode passthrough -f null - 2>&1 | grep -v -e 'non monotonically increasing dts to muxer' -e 'Last message repeated' > "$W/ff.err"
  [ -s "$W/ff.err" ] && { head -3 "$W/ff.err"; return 1; }
  local lo=$2; [ "$MODE" = valgrind ] && lo=1.5    # valgrind: see above; the duration is checked by the asan / plain runs
  python3 -I -c 'import sys; d=float(sys.argv[1].split()[-1]); sys.exit(0 if float(sys.argv[2])<=d<=float(sys.argv[3]) else 1)' "$info" "$lo" "$3"
}
counter() { st; sv "$1"; }
fresh() {  # defaults + a new rate-limit window (disabling resets it)
  cfg notify.enabled=off notify.cooldown=0 notify.max=100 notify.window_min=60 notify.clip=off notify.clip_s=4 notify.snapshot=on \
      notify.events=motion,sound,tamper notify.title= notify.image_url= notify.priority=5 notify.user= notify.token= notify.url= && st
}
SNAP0=0
sent_wait() { wait_for "${2:-60}" "s[\"sent\"]>=$1"; }

curl -s -o /dev/null -m 10 "http://$H/" || { echo "FAIL server not reachable"; exit 1; }

echo "== settings and API"
chk "GET /api/notify is JSON, off by default" sj 's["enabled"] is False and s["service"]=="ntfy" and s["sent"]==0'
chk "invalid service refused" cfg_bad notify.service=email
chk "non-http URL refused" cfg_bad notify.url=ftp://x/y
chk "URL with spaces refused" cfg_bad "notify.url=https://a b/c"
chk "unknown event kind refused" cfg_bad notify.events=motion,bogus
chk "clip_s 11 refused" cfg_bad notify.clip_s=11
chk "clip_s 2 refused" cfg_bad notify.clip_s=2
chk "max 0 refused" cfg_bad notify.max=0
chk "window 0 refused" cfg_bad notify.window_min=0
chk "priority 11 refused" cfg_bad notify.priority=11
chk "picture URL with brackets refused" cfg_bad "notify.image_url=https://x/a(b)"
chk "token with a space refused" cfg_bad "notify.token=a b"
chk "user with a space refused" cfg_bad "notify.user=a b"
c=$(curl "${A[@]}" -o "$W/t.out" -w '%{http_code}' -X POST "http://$H/api/notify/test")
grep -q "topic URL" "$W/t.out" || c="$c (no hint)"
chk "test without a URL -> 409 with a hint ($c: $(cat "$W/t.out"))" [ "$c" = 409 ]
c=$(curl --digest -u admin:admin -s -o /dev/null -w '%{http_code}' -X POST "http://$H/api/notify/test")
chk "test without X-Requested-With -> 403" [ "$c" = 403 ]
c=$(curl -s -o /dev/null -w '%{http_code}' "http://$H/api/notify")
chk "status needs a login (401)" [ "$c" = 401 ]
c=$(curl "${A[@]}" -o /dev/null -w '%{http_code}' --data-binary @"$P/ca.crt" "http://$H/api/acme/ca")
chk "test CA uploaded as the extra CA" [ "$c" = 200 ]

echo "== ntfy (HTTPS, access token, test button)"
tlsmock nt 8443
cfg notify.service=ntfy notify.url=https://push.test:8443/camtopic notify.token=tk_goodtoken
tt=$(curl "${A[@]}" -o "$W/t.out" -w '%{http_code} %{time_total}' -X POST "http://$H/api/notify/test")
chk "test queued: 202, answered at once ($tt)" python3 -I -c 'import sys; c,t=sys.argv[1].split(); sys.exit(0 if c=="202" and float(t)<float(sys.argv[2]) else 1)' "$tt" "$SLOW"
chk "test delivered" sent_wait 1
chk "ntfy: PUT /camtopic, Bearer token, Filename, image/jpeg, title, tags" q nt 'len(R)==1 and R[0]["method"]=="PUT" and R[0]["path"]=="/camtopic" and R[0]["headers"]["Authorization"]=="Bearer tk_goodtoken" and R[0]["headers"]["Filename"]=="snapshot.jpg" and R[0]["headers"]["Content-Type"]=="image/jpeg" and R[0]["title"].endswith(": test") and R[0]["headers"]["Tags"]=="white_check_mark" and R[0]["message"].startswith("Test notification from")'
chk "ntfy: attachment is a JPEG that decodes" jpeg_ok "$(saved nt 0)"
curl "${A[@]}" "http://$H/api/config?prefix=notify." > "$W/c.json"
chk "URL and token are secrets in /api/config" python3 -I -c 'import json,sys; c=json.load(open(sys.argv[1])); sys.exit(0 if c["notify.url"]=="(set)" and c["notify.token"]=="(set)" and c["notify.service"]=="ntfy" and c["notify.enabled"]=="off" else 1)' "$W/c.json"
chk "status: server host only (no topic), state ok" sj 's["server"]=="push.test" and "camtopic" not in json.dumps(s) and s["state"]=="ok" and s["last_ok"]!=""'

echo "== ntfy events"
cfg notify.events=motion notify.cooldown=0 notify.enabled=on
ev motion
chk "motion event delivered" sent_wait 2
chk "ntfy: motion title and tag" q nt 'len(R)==2 and R[1]["title"].endswith(": motion") and R[1]["headers"]["Tags"]=="rotating_light" and R[1]["message"].startswith("Motion detected at")'
ev sound -30; sleep 2
chk "sound not selected in notify.events: nothing sent" q nt 'len(R)==2'
cfg notify.token=cam:secret "notify.title=Garten Tür"
ev motion
chk "Basic auth + non-ASCII title delivered" sent_wait 3
chk "ntfy: Basic user:password, RFC 2047 title decodes to UTF-8" q nt 'len(R)==3 and R[2]["headers"]["Authorization"].startswith("Basic ") and R[2]["title"]=="Garten Tür: motion" and R[2]["headers"]["Title"].startswith("=?UTF-8?B?")'
cfg notify.token=tk_wrong notify.title=
f0=$(counter failed)
ev motion
chk "bad token: failure reported" wait_for 30 "s[\"failed\"]>=$((f0 + 1)) and 'HTTP 401: unauthorized' in s['error'] and s['state']=='error'"
chk "bad token: no retry (one request)" q nt 'len(R)==4 and R[3]["status"]==401'
mock ntp 8080
cfg notify.url=http://127.0.0.1:8080/open notify.token= notify.snapshot=off
s0=$(counter sent)
ev motion
chk "plain HTTP, no token, no picture: delivered" sent_wait $((s0 + 1))
chk "ntfy: text-only POST (no Filename), message in the body" q ntp 'len(R)==1 and R[0]["method"]=="POST" and "Filename" not in R[0]["headers"] and "Authorization" not in R[0]["headers"] and R[0]["message"].startswith("Motion detected")'
unmock ntp

echo "== Telegram (HTTPS, chunked answers)"
tlsmock tg 8444 --chunked
fresh
cfg notify.service=telegram notify.url=https://push.test:8444 notify.token=123456:ABC-good_token notify.user=4242 notify.enabled=on
s0=$(counter sent)
ev motion
chk "Telegram: delivered" sent_wait $((s0 + 1))
chk "Telegram: sendPhoto, chat_id, caption, one photo field (image/jpeg)" q tg 'len(R)==1 and R[0]["path"]=="/bot123456:ABC-good_token/sendPhoto" and R[0]["fields"]["chat_id"]=="4242" and "motion" in R[0]["fields"]["caption"] and [f["name"] for f in R[0]["files"]]==["photo"] and R[0]["files"][0]["ctype"]=="image/jpeg"'
chk "Telegram: photo decodes" jpeg_ok "$(saved tg 0)"
cfg notify.snapshot=off
ev sound -33
chk "Telegram: text only -> sendMessage" wait_for 30 "s[\"sent\"]>=$((s0 + 2))"
chk "Telegram: sendMessage text has the sound level" q tg 'len(R)==2 and R[1]["path"].endswith("/sendMessage") and "-33 dBFS" in R[1]["fields"]["text"] and not R[1]["files"]'
f0=$(counter failed)
cfg notify.token=123456:ABC-wrong
ev motion
chk "Telegram: bad token -> 'Unauthorized', no retry" wait_for 30 "s[\"failed\"]>=$((f0 + 1)) and 'Unauthorized' in s['error']"
cfg notify.token=123456:ABC-good_token notify.user=999
ev motion
chk "Telegram: unknown chat -> 'chat not found'" wait_for 30 "s[\"failed\"]>=$((f0 + 2)) and 'chat not found' in s['error']"
chk "Telegram: 4 requests in all (no retries on 4xx)" q tg 'len(R)==4'
unmock tg

echo "== Pushover"
tlsmock po 8445
fresh
cfg notify.service=pushover notify.url=https://push.test:8445 notify.token=azGoodAppToken notify.user=uGoodUserKey notify.clip=on notify.enabled=on
s0=$(counter sent)
ev sound -30
chk "Pushover: delivered" sent_wait $((s0 + 1))
idle
chk "Pushover: token, user, title, message, attachment (no video even with clips on)" q po 'len(R)==1 and R[0]["path"]=="/1/messages.json" and R[0]["fields"]["token"]=="azGoodAppToken" and R[0]["fields"]["user"]=="uGoodUserKey" and R[0]["fields"]["title"].endswith(": sound") and "-30 dBFS" in R[0]["fields"]["message"] and [f["name"] for f in R[0]["files"]]==["attachment"] and R[0]["files"][0]["ctype"]=="image/jpeg"'
chk "Pushover: attachment decodes" jpeg_ok "$(saved po 0)"
f0=$(counter failed)
cfg notify.user=uWrongKey
ev tamper
chk "Pushover: bad user key -> its error text" wait_for 30 "s[\"failed\"]>=$((f0 + 1)) and 'user identifier' in s['error']"
unmock po

echo "== Discord (webhook, bot, 429)"
tlsmock dc 8446
fresh
cfg notify.service=discord notify.url=https://push.test:8446/api/webhooks/1234/goodhook notify.enabled=on
s0=$(counter sent)
ev motion
chk "Discord webhook: delivered" sent_wait $((s0 + 1))
chk "Discord webhook: ?wait=true, payload_json content/username/no mentions, files[0] JPEG" q dc 'len(R)==1 and R[0]["path"]=="/api/webhooks/1234/goodhook" and R[0]["query"]=="wait=true" and R[0]["payload"]["content"].startswith("**") and "motion" in R[0]["payload"]["content"] and R[0]["payload"]["username"] and R[0]["payload"]["allowed_mentions"]=={"parse":[]} and [f["name"] for f in R[0]["files"]]==["files[0]"] and R[0]["files"][0]["filename"]=="snapshot.jpg"'
chk "Discord: attachment decodes" jpeg_ok "$(saved dc 0)"
cfg notify.url=https://push.test:8446 notify.token=goodbot notify.user=987654321
ev motion
chk "Discord bot: delivered" sent_wait $((s0 + 2))
chk "Discord bot: channel messages URL, 'Bot' authorization, no username" q dc 'len(R)==2 and R[1]["path"]=="/api/v10/channels/987654321/messages" and R[1]["headers"]["Authorization"]=="Bot goodbot" and "username" not in R[1]["payload"]'
f0=$(counter failed)
cfg notify.token= notify.url=https://push.test:8446/api/webhooks/1234/badhook
ev motion
chk "Discord: bad webhook token -> 'Invalid Webhook Token'" wait_for 30 "s[\"failed\"]>=$((f0 + 1)) and 'Invalid Webhook Token' in s['error']"
cfg notify.token=wrongbot notify.url=https://push.test:8446 notify.user=987654321
ev motion
chk "Discord: bad bot token -> 401" wait_for 30 "s[\"failed\"]>=$((f0 + 2)) and '401' in s['error']"
cfg notify.user=abc
c=$(curl "${A[@]}" -o "$W/t.out" -w '%{http_code}' -X POST "http://$H/api/notify/test")
chk "Discord bot: a non-numeric channel id is refused before sending ($c $(cat "$W/t.out"))" [ "$c" = 409 ]
unmock dc
tlsmock d429 8447 --rate429 2
cfg notify.token= notify.user= notify.url=https://push.test:8447/api/webhooks/1234/goodhook
s0=$(counter sent)
ev motion
chk "Discord 429 twice: delivered on the third attempt" sent_wait $((s0 + 1)) 60
chk "Discord 429: waited retry_after (>= 0.8 s) between attempts" q d429 'len(R)==3 and [r["status"] for r in R]==[429,429,200] and R[1]["t"]-R[0]["t"]>=0.8 and R[2]["t"]-R[1]["t"]>=0.8'
unmock d429

echo "== Gotify (HTTP and HTTPS)"
mock go 8081
fresh
cfg notify.service=gotify notify.url=http://127.0.0.1:8081 notify.token=AgoodGotify notify.priority=8 notify.clip=on notify.enabled=on
s0=$(counter sent)
ev motion
chk "Gotify: delivered" sent_wait $((s0 + 1))
idle
chk "Gotify: POST /message, X-Gotify-Key, JSON title/message/priority 8, no attachment, no clip" q go 'len(R)==1 and R[0]["path"]=="/message" and R[0]["headers"]["X-Gotify-Key"]=="AgoodGotify" and R[0]["json"]["priority"]==8 and R[0]["json"]["title"].endswith(": motion") and "extras" not in R[0]["json"] and not R[0]["files"]'
cfg notify.image_url=https://cam.example.org/snap.jpg
ev motion
chk "Gotify + picture URL: delivered" sent_wait $((s0 + 2))
chk "Gotify: Markdown image + text/markdown extras" q go 'len(R)==2 and "![snapshot](https://cam.example.org/snap.jpg)" in R[1]["json"]["message"] and R[1]["json"]["extras"]["client::display"]["contentType"]=="text/markdown"'
f0=$(counter failed)
cfg notify.token=Abad
ev motion
chk "Gotify: bad key -> its errorDescription" wait_for 30 "s[\"failed\"]>=$((f0 + 1)) and 'valid access token' in s['error']"
unmock go
tlsmock go2 8448
cfg notify.url=https://push.test:8448 notify.token=AgoodGotify notify.image_url=
s0=$(counter sent)
ev motion
chk "Gotify over HTTPS (extra CA): delivered" sent_wait $((s0 + 1))
unmock go2

echo "== video clips"
tlsmock ntc 8449
fresh
cfg notify.service=ntfy notify.url=https://push.test:8449/camtopic notify.token=tk_goodtoken notify.clip=on notify.clip_s=4 notify.enabled=on
c0=$(counter clips)
ev motion
chk "ntfy clip: sent" wait_for 60 "s[\"clips\"]>=$((c0 + 1))"
chk "ntfy clip: picture first, then clip.mp4 (video/mp4, low priority, movie_camera)" q ntc 'len(R)==2 and R[0]["headers"]["Filename"]=="snapshot.jpg" and R[1]["headers"]["Filename"]=="clip.mp4" and R[1]["headers"]["Content-Type"]=="video/mp4" and R[1]["headers"]["Priority"]=="2" and R[1]["headers"]["Tags"]=="movie_camera"'
chk "ntfy clip: H.264, decodes, ~4 s" clip_ok "$(saved ntc 1)" 3.0 5.0
cfg notify.clip_s=10
ev motion
chk "10 s clip: sent" wait_for 60 "s[\"clips\"]>=$((c0 + 2))"
chk "10 s clip at ~2 Mbit/s: stopped at the 2 MB cap" sj '"size cap" in s["clip_note"] and s["last_clip_bytes"]<=2*1024*1024'
chk "capped clip still decodes (3-10 s)" clip_ok "$(saved ntc 3)" 3.0 10.0
unmock ntc
tlsmock tgc 8450
cfg notify.service=telegram notify.url=https://push.test:8450 notify.token=123456:ABC-good_token notify.user=4242 notify.clip_s=4
ev motion
chk "Telegram clip: sent" wait_for 60 "s[\"clips\"]>=$((c0 + 3))"
chk "Telegram clip: sendPhoto, then sendVideo (silent, streaming) with clip.mp4" q tgc 'len(R)==2 and R[0]["path"].endswith("/sendPhoto") and R[1]["path"].endswith("/sendVideo") and R[1]["fields"]["disable_notification"]=="true" and R[1]["fields"]["supports_streaming"]=="true" and R[1]["files"][0]["filename"]=="clip.mp4" and R[1]["files"][0]["ctype"]=="video/mp4"'
chk "Telegram clip: H.264, decodes, ~4 s" clip_ok "$(saved tgc 1)" 3.0 5.0
unmock tgc
tlsmock dcc 8451
cfg notify.service=discord notify.url=https://push.test:8451/api/webhooks/1234/goodhook notify.token= notify.user=
ev motion
chk "Discord clip: sent" wait_for 60 "s[\"clips\"]>=$((c0 + 4))"
chk "Discord clip: second message with files[0]=clip.mp4, silent flag" q dcc 'len(R)==2 and R[1]["files"][0]["filename"]=="clip.mp4" and R[1]["payload"]["flags"]==4096'
chk "Discord clip: H.264, decodes, ~4 s" clip_ok "$(saved dcc 1)" 3.0 5.0
unmock dcc
tlsmock n413 8452 --max-attach 300000
cfg notify.service=ntfy notify.url=https://push.test:8452/camtopic notify.token=tk_goodtoken
cf0=$(counter clip_failed); s0=$(counter sent)
ev motion
chk "ntfy 413 on the clip: reported, the picture was sent" wait_for 60 "s[\"clip_failed\"]>=$((cf0 + 1)) and 'too large' in s['clip_note'] and s['sent']>=$((s0 + 1))"
chk "ntfy 413: picture 200, clip 413" q n413 'len(R)==2 and R[0]["status"]==200 and R[1]["status"]==413'
ev motion
chk "next event: the too-large clip is not uploaded again" wait_for 60 "s[\"clip_failed\"]>=$((cf0 + 2))"
chk "ntfy 413: only the picture went out the second time" q n413 'len(R)==3 and R[2]["headers"]["Filename"]=="snapshot.jpg"'
unmock n413

echo "== burst with clips on (bounded queue, one clip at a time)"
tlsmock bur 8453
fresh
cfg notify.service=ntfy notify.url=https://push.test:8453/camtopic notify.token=tk_goodtoken notify.clip=on notify.clip_s=3 notify.enabled=on
dq0=$(counter dropped_queue); s0=$(counter sent)
SP=$(pgrep -x host_server | head -1)
kb() { [ -n "$SP" ] && sed -n "s/^$1:[[:space:]]*\([0-9]*\) kB/\1/p" "/proc/$SP/status"; }
[ -n "$SP" ] && echo 5 > "/proc/$SP/clear_refs" 2>/dev/null    # reset the peak (VmHWM) to the current RSS
rss0=$(kb VmRSS); hwm0=$(kb VmHWM)
# VmHWM misses memory given back with madvise (glibc arenas): also sample RSS every 0.1 s
( while [ -n "$SP" ] && [ ! -f "$W/rss.stop" ]; do kb VmRSS; sleep 0.1; done > "$W/rss.samples" ) &
for i in $(seq 1 10); do echo "motion 1" > "$FIFO"; sleep 0.05; echo "motion 0" > "$FIFO"; sleep 0.05; done
chk "burst: at most 4 queued + 1 in progress, the rest dropped and counted" wait_for 10 "s[\"dropped_queue\"]>=$((dq0 + 4))"
chk "burst: queue drains" idle 180
touch "$W/rss.stop"; wait $!
rss1=$(kb VmRSS); hwm1=$(kb VmHWM)
smax=$(sort -n "$W/rss.samples" 2>/dev/null | tail -1); [ -n "$smax" ] && [ -n "$hwm1" ] && [ "$smax" -gt "$hwm1" ] && hwm1=$smax
[ -n "$SP" ] && echo "   host_server memory: RSS ${rss0} -> ${rss1} kB, peak ${hwm0} -> ${hwm1} kB ($(wc -l < "$W/rss.samples") samples; clips already sent before: allocator warm)"
if [ "$MODE" = plain ]; then    # ASan's quarantine / valgrind keep freed memory: only meaningful natively
  mem_ok=0; [ $((rss1 - rss0)) -lt 6144 ] && [ $((hwm1 - rss0)) -lt 8192 ] && mem_ok=1
  chk "burst with clips: RSS grows < 6 MB, peak < 8 MB above the start" [ $mem_ok = 1 ]
fi
chk "burst: every accepted event delivered with its clip" q bur 'len(R)>=4 and len(R)%2==0 and all(r["status"]==200 for r in R) and [r["headers"]["Filename"] for r in R]==["snapshot.jpg","clip.mp4"]*(len(R)//2)'
unmock bur

echo "== cooldown"
tlsmock cd 8454
fresh
cfg notify.service=ntfy notify.url=https://push.test:8454/camtopic notify.token=tk_goodtoken notify.cooldown=30 notify.enabled=on
dc0=$(counter dropped_cooldown)
ev motion; ev motion; ev motion
sleep 2; idle
chk "cooldown: 3 motion events -> 1 notification" q cd 'len(R)==1'
chk "cooldown: 2 counted" sj "s['dropped_cooldown']==$((dc0 + 2))"
ev sound -20
sleep 1; idle
chk "cooldown is per event kind: sound still notifies" q cd 'len(R)==2 and R[1]["title"].endswith(": sound")'
unmock cd

echo "== rate limit: notify.max per notify.window_min, then one summary"
tlsmock rl 8455
fresh
cfg notify.service=ntfy notify.url=https://push.test:8455/camtopic notify.token=tk_goodtoken notify.max=3 notify.window_min=1 notify.enabled=on
t0=$(date +%s)
for i in $(seq 1 7); do ev motion; done
sleep 1; idle
chk "burst of 7 -> exactly 3 deliveries" q rl 'len(R)==3'
chk "4 suppressed, window shows 3/3" sj 's["suppressed"]==4 and s["window_sent"]==3 and s["window_max"]==3 and s["window_left_s"]>0'
chk "summary after the window (~60 s)" wait_for 90 's["summaries"]>=1 and s["suppressed"]==0'
idle
chk "summary: one text message '4 notifications suppressed'" q rl 'len(R)==4 and R[3]["method"]=="POST" and "Filename" not in R[3]["headers"] and R[3]["message"].startswith("4 notifications suppressed") and R[3]["headers"]["Tags"]=="hourglass"'
echo "   window took $(( $(date +%s) - t0 )) s"
ev motion
chk "new window: notifications flow again" wait_for 30 "s['window_sent']==1"
unmock rl

echo "== failures: server down, 500 then OK, untrusted certificate, slow server + queue overflow"
fresh
cfg notify.service=ntfy notify.url=https://push.test:9/camtopic notify.token=tk_goodtoken notify.enabled=on
f0=$(counter failed); t0=$(date +%s)
ev motion
chk "server down: failure after bounded retries" wait_for 60 "s[\"failed\"]>=$((f0 + 1)) and 'cannot connect' in s['error']"
el=$(( $(date +%s) - t0 ))
chk "server down: 2 retries logged, took ${el} s (>= 12)" sj "any('retry 2/2' in l for l in s['log']) and $el >= 12"
tlsmock f500 8456 --fail500 2
cfg notify.url=https://push.test:8456/camtopic
s0=$(counter sent)
ev motion
chk "HTTP 500 twice: delivered on the third attempt" sent_wait $((s0 + 1)) 60
chk "500, 500, 200" q f500 '[r["status"] for r in R]==[500,500,200]'
unmock f500
c=$(curl "${A[@]}" -o /dev/null -w '%{http_code}' --data-binary "" "http://$H/api/acme/ca")
tlsmock unt 8457
cfg notify.url=https://push.test:8457/camtopic
f0=$(counter failed)
ev motion
chk "without the extra CA: certificate rejected, no retry" wait_for 30 "s[\"failed\"]>=$((f0 + 1)) and 'not signed by a trusted CA' in s['error']"
chk "untrusted: nothing reached the server" q unt 'len(R)==0'
unmock unt
curl "${A[@]}" -o /dev/null --data-binary @"$P/ca.crt" "http://$H/api/acme/ca"
tlsmock slow 8458 --delay 35
tlsmock fast 8459
cfg notify.url=https://push.test:8458/camtopic
dq0=$(counter dropped_queue)
for i in $(seq 1 8); do ev motion; done
chk "slow server: 1 sending + 4 queued, 3 dropped" wait_for 10 "s['queued']==5 and s['dropped_queue']==$((dq0 + 3))"
tt=$(curl "${A[@]}" -o /dev/null -w '%{time_total}' "http://$H/api/status")
chk "web server answers at once while sending ($tt s)" python3 -I -c 'import sys; sys.exit(0 if float(sys.argv[1])<float(sys.argv[2]) else 1)' "$tt" "$SLOW"
c=$(curl "${A[@]}" -o "$W/t.out" -w '%{http_code}' -X POST "http://$H/api/notify/test")
grep -q "queue is full" "$W/t.out" || c="$c (no reason)"
chk "test button with a full queue -> 409 'queue is full' ($c)" [ "$c" = 409 ]
f0=$(counter failed)
cfg notify.url=https://push.test:8459/camtopic
chk "slow server: 'no answer within 30 s'" wait_for 60 "s['failed']>=$((f0 + 1)) and any('no answer within 30 s' in l for l in s['log'])"
chk "the queued events then go to the working server" idle 120
chk "4 queued events delivered" q fast 'len(R)==4'
sleep 6
chk "slow server got exactly one request (no retry after a timeout)" q slow 'len(R)==1'
unmock slow; unmock fast

echo "== off"
tlsmock off 8460
fresh
cfg notify.service=ntfy notify.url=https://push.test:8460/camtopic notify.token=tk_goodtoken notify.enabled=on notify.events=sound
ev motion; sleep 2
chk "event kind not selected: nothing sent" q off 'len(R)==0'
cfg notify.events=motion,sound,tamper notify.enabled=off
ev motion; ev sound -10; sleep 2
chk "notify.enabled=off: nothing sent" q off 'len(R)==0'
unmock off

echo "== line crossing / loitering events (md_logic.c)"
tlsmock lin 8461
fresh
cfg notify.service=ntfy notify.url=https://push.test:8461/camtopic notify.token=tk_goodtoken notify.cooldown=0 notify.events=line,loiter notify.enabled=on
s0=$(counter sent)
ev line 2
chk "line crossing delivered" wait_for 30 "s['sent']>=$((s0 + 1))"
chk "ntfy: line title and direction" q lin 'len(R)==1 and R[0]["title"].endswith(": line crossed") and "toward the right" in R[0]["message"]'
ev loiter 3
chk "loitering delivered" wait_for 30 "s['sent']>=$((s0 + 2))"
chk "ntfy: loitering title and zone" q lin 'len(R)==2 and R[1]["title"].endswith(": loitering") and "zone 3" in R[1]["message"]'
ev motion; sleep 2
chk "motion not selected: nothing more" q lin 'len(R)==2'
c=$(curl "${A[@]}" -o /dev/null -w "%{http_code}" --data "notify.events=line,bogus" "http://$H/api/config")
chk "unknown kind refused ($c)" [ "$c" = 400 ]
cfg notify.enabled=off
unmock lin

chk "server still serves the page" [ "$(curl -s -o /dev/null -m 10 -w %{http_code} "http://$H/")" = 401 ]
st; echo "   final status: $(head -c 600 "$W/st.json")"
echo "notify suite: $n checks, $fails failed"
exit $fails
