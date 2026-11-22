#!/bin/bash
# Stability under load on a camera running this build: parallel HTTPS
# handshakes (custom / camera certificate by SNI), repeated uploads and
# removals, garbage uploads, while the RTSP stream runs. Checks that
# ak_rtsp never restarts, memory stays bounded and video keeps flowing.
# Usage: CAM_USER=.. CAM_PASS=.. CAMSH=../camsh.py bash camera_stress.sh <ip> <fixtures>
set -u
H=$1; F=$2
C=(curl -s -m 30 --digest -u "$CAM_USER:$CAM_PASS" -H "X-Requested-With: lsc")
camsh() { echo "$1" | python3 "$CAMSH" "$H" 8 -; }
state() { camsh 'p=$(pidof ak_rtsp); echo PID=$p; grep -E "VmRSS" /proc/$p/status; grep MemAvailable /proc/meminfo' | grep -E "^PID=|VmRSS|MemAvailable" | tr -s ' ' | tr '\n' ' '; }
fails=0
ok()  { echo "PASS $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

before=$(state); echo "before: $before"
pid0=$(sed -n 's/.*PID=\([0-9]*\).*/\1/p' <<<"$before")
rss0=$(sed -n 's/.*VmRSS: \([0-9]*\).*/\1/p' <<<"$before")
cat "$F/good.crt" "$F/inter.crt" "$F/ec.key" | "${C[@]}" --data-binary @- -o /dev/null "http://$H/api/tls/custom"

# video during the test
timeout 200 ffmpeg -nostdin -loglevel error -rtsp_transport tcp -i "rtsp://$CAM_USER:$CAM_PASS@$H:554/" -t 180 -f null - 2>/tmp/ff.$$ &
FF=$!

hs() { local n=$1 sni=$2 okc=0; for i in $(seq 1 "$n"); do
         openssl s_client -connect "$H:443" ${sni:+-servername "$sni"} </dev/null 2>/dev/null | grep -q "BEGIN CERTIFICATE" && okc=$((okc + 1)); done; echo "$okc"; }
t0=$(date +%s)
hs 60 cam.example.org > /tmp/h1.$$ & P1=$!; hs 60 other.lan > /tmp/h2.$$ & P2=$!; hs 60 "" > /tmp/h3.$$ & P3=$!
# uploads / removals / garbage at the same time
up_ok=0
for i in $(seq 1 25); do
  cat "$F/good.crt" "$F/inter.crt" "$F/ec.key" | "${C[@]}" --data-binary @- -o /dev/null -w "%{http_code}\n" "http://$H/api/tls/custom" | grep -q 200 && up_ok=$((up_ok + 1))
  head -c 3000 /dev/urandom | "${C[@]}" --data-binary @- -o /dev/null "http://$H/api/tls/custom"
  [ $((i % 5)) = 0 ] && "${C[@]}" -X POST -o /dev/null "http://$H/api/tls/custom/clear"
  "${C[@]}" -o /dev/null "http://$H/api/acme"
done
wait $P1 $P2 $P3
t1=$(date +%s)
h1=$(cat /tmp/h1.$$); h2=$(cat /tmp/h2.$$); h3=$(cat /tmp/h3.$$)
[ "$h1" = 60 ] && [ "$h2" = 60 ] && [ "$h3" = 60 ] && ok "180 parallel handshakes completed ($((t1 - t0)) s)" || bad "handshakes: $h1/$h2/$h3 of 60"
[ "$up_ok" = 25 ] && ok "25 uploads accepted while handshaking" || bad "uploads: $up_ok/25"
wait $FF; frc=$?
[ $frc = 0 ] && [ ! -s /tmp/ff.$$ ] && ok "RTSP stream ran 180 s without errors" || bad "ffmpeg rc=$frc: $(head -3 /tmp/ff.$$)"
after=$(state); echo "after:  $after"
pid1=$(sed -n 's/.*PID=\([0-9]*\).*/\1/p' <<<"$after")
rss1=$(sed -n 's/.*VmRSS: \([0-9]*\).*/\1/p' <<<"$after")
[ "$pid0" = "$pid1" ] && ok "ak_rtsp never restarted (pid $pid1)" || bad "ak_rtsp restarted ($pid0 -> $pid1)"
echo "RSS growth: $((rss1 - rss0)) kB over 25 uploads"
[ $((rss1 - rss0)) -lt 2048 ] && ok "memory growth under 2 MB" || bad "memory grew $((rss1 - rss0)) kB"
camsh 'cat /etc/config/ak_rtsp_lastcrash 2>/dev/null | head -3' | grep -q "signal" && bad "crash record present" || ok "no crash record"
"${C[@]}" -X POST -o /dev/null "http://$H/api/tls/custom/clear"
rm -f /tmp/h1.$$ /tmp/h2.$$ /tmp/h3.$$ /tmp/ff.$$
[ $fails = 0 ] && echo "STRESS PASS" || { echo "$fails failed"; exit 1; }
