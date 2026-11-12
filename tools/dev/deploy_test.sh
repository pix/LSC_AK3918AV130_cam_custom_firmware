#!/bin/bash
# deploy_test.sh <local binary> <name>: try a build on the camera WITHOUT flashing.
# Pushes the binary to the SD card (/tmp/sd/maint/<name>), selects it with
# USE_TEST_BINARY (hack.sh bind-mounts it over /usr/bin/ak_rtsp at boot),
# reboots and waits for the web page. Undo: rename USE_TEST_BINARY on the SD.
# Needs the maintenance telnetd on port 24 (decloud SD card) and busybox nc.
# Environment: CAMERA_IP (required), CAMERA_USER / CAMERA_PASS (default admin/admin).
set -u
T=$(dirname "$0"); H=${CAMERA_IP:?set CAMERA_IP}
U=${CAMERA_USER:-admin}; P=${CAMERA_PASS:-admin}
[ $# -eq 2 ] || { echo "usage: $0 <binary> <name>"; exit 2; }
echo "pkill -x nc; rm -f /tmp/sd/maint/$2; setsid sh -c \"nc -l -p 9100 > /tmp/sd/maint/$2\" </dev/null >/dev/null 2>&1 &" | python3 "$T/camsh.py" "$H" 5 - >/dev/null
sleep 1
python3 -I -c 'import socket,sys
s=socket.create_connection((sys.argv[2],9100)); s.sendall(open(sys.argv[1],"rb").read()); s.shutdown(socket.SHUT_WR); s.close()' "$1" "$H"
sleep 2
want=$(md5sum < "$1" | cut -d" " -f1)
got=$(echo "md5sum /tmp/sd/maint/$2" | python3 "$T/camsh.py" "$H" 8 - | grep -o "^[0-9a-f]\{32\}")
[ "$want" = "$got" ] || { echo "PUSH FAILED ($want vs $got)"; exit 1; }
echo "pushed $2 $want"
echo "chmod +x /tmp/sd/maint/$2; echo $2 > /tmp/sd/maint/USE_TEST_BINARY; rm -f /tmp/sd/maint/ak_rtsp.log; sync; reboot" | python3 "$T/camsh.py" "$H" 5 - >/dev/null
sleep 45
for i in $(seq 1 25); do
  c=$(curl -s -m 3 -o /dev/null -w "%{http_code}" -u "$U:$P" --digest "http://$H/api/osd")
  [ "$c" = 200 ] && { echo "web up"; exit 0; }
  sleep 3
done
echo "web NOT up"; exit 1
