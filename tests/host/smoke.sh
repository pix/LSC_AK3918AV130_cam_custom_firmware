#!/bin/bash
# Quick checks against host_server (run inside run_sandboxed.sh).
H=${CAM:-127.0.0.1}; A=(--digest -u admin:admin -m 20 -s)
fails=0
chk() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
chk "page 401 without login" '[ "$(curl -s -o /dev/null -w %{http_code} http://$H/)" = 401 ]'
chk "page 200 with login" '[ "$(curl "${A[@]}" -o /dev/null -w %{http_code} http://$H/)" = 200 ]'
chk "status JSON" 'curl "${A[@]}" http://$H/api/status | python3 -c "import json,sys; json.load(sys.stdin)"'
chk "snapshot JPEG" 'curl "${A[@]}" http://$H/snapshot.jpg | head -c 3 | od -An -tx1 | grep -q "ff d8 ff"'
for i in $(seq 1 50); do curl -sk -o /dev/null https://$H/ && break; sleep 0.2; done
chk "HTTPS up" '[ "$(curl -sk "${A[@]}" -o /dev/null -w %{http_code} https://$H/api/tls)" = 200 ]'
chk "RTSP 5 s (TCP)" 'timeout 30 ffmpeg -nostdin -loglevel error -rtsp_transport tcp -i rtsp://admin:admin@$H:554/ -t 5 -f null - 2>&1 | grep -qv . ; [ ${PIPESTATUS[0]} = 0 ]'
echo "smoke: $fails failed"
exit $fails
