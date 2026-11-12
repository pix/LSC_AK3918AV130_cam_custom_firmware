#!/bin/bash
H=${1:-${CAMERA_IP:?usage: $0 <camera ip> or set CAMERA_IP}}; D=${TMPDIR:-/tmp}/rectest; mkdir -p $D; rm -f $D/*
A=(-s -m 20 -u admin:admin --digest -H X-Requested-With:lsc)
cfg(){ curl "${A[@]}" "$@" http://$H/api/config; echo; }
G(){ curl -s -m 20 -u admin:admin --digest "$@"; }
echo "== continuous, 1-minute files"
cfg --data-urlencode "record.mode=continuous" --data-urlencode "record.segment=1" --data-urlencode "record.audio=on"
sleep 5; G http://$H/api/rec; echo
sleep 75; G http://$H/api/rec; echo
DAY=$(G http://$H/api/rec/list | python3 -c "import json,sys; print(json.load(sys.stdin)['days'][0]['day'])")
G "http://$H/api/rec/list?day=$DAY"; echo
F=$(G "http://$H/api/rec/list?day=$DAY" | python3 -c "import json,sys; print([f['name'] for f in json.load(sys.stdin)['files'] if f['name'].endswith('cont.mp4') and not f.get('recording')][0])")
G -o $D/cont.mp4 "http://$H/rec/$DAY/$F?dl=1"; ls -la $D/cont.mp4
ffprobe -v error -show_entries stream=codec_name,codec_tag_string,width,height:format=duration -of compact $D/cont.mp4
ffmpeg -v error -i $D/cont.mp4 -f null - && echo "decodes cleanly"
ffprobe -v error -select_streams v -show_entries stream=start_time -of csv=p=0 $D/cont.mp4 | sed 's/^/video start /'; ffprobe -v error -select_streams a -show_entries stream=start_time,duration -of csv=p=0 $D/cont.mp4 | sed 's/^/audio start,dur /'
echo "range request:"; G -o /dev/null -w "%{http_code} %{size_download} bytes\n" -H "Range: bytes=1000-1999" "http://$H/rec/$DAY/$F"
echo "== motion clips with pre-roll (overlay-clock self-test)"
Z=$(printf '0%.0s' $(seq 1 128))ffffffffffffffff
cfg --data-urlencode "record.mode=motion" --data-urlencode "record.pre=3" --data-urlencode "record.post=4" --data-urlencode "record.snapshots=on" \
    --data-urlencode "md.zones=$Z" --data-urlencode "md.ignore_osd=off" --data-urlencode "md.sensitivity=100" --data-urlencode "md.hold=3" --data-urlencode "md.enabled=on"
sleep 30
cfg --data-urlencode "md.enabled=off"
sleep 12
G "http://$H/api/rec/list?day=$DAY" | python3 -c "import json,sys; [print('  ',f['name'],f['size'],'recording' if f.get('recording') else '') for f in json.load(sys.stdin)['files'][:8]]"
M=$(G "http://$H/api/rec/list?day=$DAY" | python3 -c "import json,sys; print([f['name'] for f in json.load(sys.stdin)['files'] if f['name'].endswith('motion.mp4') and not f.get('recording')][0])")
G -o $D/motion.mp4 "http://$H/rec/$DAY/$M"; ffprobe -v error -show_entries format=duration -of csv=p=0 $D/motion.mp4 | sed 's/^/motion clip seconds /'
J=$(G "http://$H/api/rec/list?day=$DAY" | python3 -c "import json,sys; print([f['name'] for f in json.load(sys.stdin)['files'] if f['name'].endswith('.jpg')][0])")
G -o $D/event.jpg "http://$H/rec/$DAY/$J"; file $D/event.jpg | cut -c1-90
echo "== delete one file"
G -X POST -H X-Requested-With:lsc -d "day=$DAY&name=$J" http://$H/api/rec/delete; echo
cfg --data-urlencode "record.mode=off" --data-urlencode "record.segment=10" --data-urlencode "record.post=10" --data-urlencode "md.zones=" --data-urlencode "md.ignore_osd=on" --data-urlencode "md.sensitivity=50" --data-urlencode "md.hold=10"
echo DONE
