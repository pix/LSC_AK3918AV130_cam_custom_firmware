#!/bin/bash
# multitest.sh <host> <ca.crt>: several RTSP/RTSPS sessions + a web viewer at once
H=$1; CA=$2; D=${TMPDIR:-/tmp}/multitest; mkdir -p $D; rm -f $D/*.mkv $D/*.err
U=admin:admin; SECS=20; HERE=$(dirname "$0")
run(){ timeout $((SECS+25)) ffmpeg -v warning -rtsp_transport $3 ${4:+-tls_verify 1 -ca_file $CA} -i "$2" -t $SECS -c copy -y $D/$1.mkv 2>$D/$1.err; }
run tcp  "rtsp://$U@$H:554/"  tcp &
run udp  "rtsp://$U@$H:554/"  udp &
run tls  "rtsps://$U@$H:322/" tcp tls &
run tls2 "rtsps://$U@$H:322/" tcp tls &
sleep 4
python3 -I $HERE/wsclient.py $H 443 8 $CA > $D/ws.txt 2>&1 &
# 5th RTSP session must be refused while 4 run
timeout 10 ffmpeg -v error -rtsp_transport tcp -i "rtsp://$U@$H:554/" -t 2 -f null - > $D/fifth.txt 2>&1; echo "5th session: exit=$? $(head -c 120 $D/fifth.txt | tr '\n' ' ')"
echo 'top -b -n2 -d2 | grep -E "^CPU|ak_rtsp" | tail -2' | python3 ${CAMSH:-camsh.py} $H 10 - | grep -E "^CPU|ak_rtsp" | tail -2
wait
for n in tcp udp tls tls2; do
  f=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $D/$n.mkv 2>/dev/null)
  du=$(ffprobe -v error -show_entries format=duration -of csv=p=0 $D/$n.mkv 2>/dev/null)
  st=$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 $D/$n.mkv 2>/dev/null | tr '\n' '+')
  echo "$n: duration=$du frames=$f streams=$st warnings=$(grep -vc 'Timestamps are unset' $D/$n.err)"
done
cat $D/ws.txt
timeout 20 ffmpeg -v error -rtsp_transport udp -ca_file $CA -tls_verify 1 -i "rtsps://$U@$H:322/" -t 2 -f null - 2>&1 | head -1 | sed 's/^/rtsps+udp: /'
