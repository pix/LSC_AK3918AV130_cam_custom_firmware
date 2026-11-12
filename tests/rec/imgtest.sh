#!/bin/bash
H=${1:-${CAMERA_IP:?usage: $0 <camera ip> or set CAMERA_IP}}; D=${TMPDIR:-/tmp}/imgtest; mkdir -p $D; rm -f $D/*.jpg $D/*.png
A=(-s -m 20 -u admin:admin --digest -H X-Requested-With:lsc)
cfg(){ curl "${A[@]}" "$@" http://$H/api/config; echo; }
snap(){ curl -s -m 10 -u admin:admin --digest -o $D/$1.jpg http://$H/snapshot.jpg; }
main(){ timeout 25 ffmpeg -v error -rtsp_transport tcp -i rtsp://admin:admin@$H:554/ -an -frames:v 1 -update 1 -y $D/$1.png; }
lum(){ python3 -c "
from PIL import Image,ImageStat
import sys
im=Image.open(sys.argv[1]).convert('L'); w,h=im.size
q=im.crop((0,0,w//2,h//2)); r=im.crop((w//2,h//2,w,h))
print('%-14s mean %.0f  top-left quarter %.0f (stdev %.1f)  bottom-right %.0f' % (sys.argv[1].split('/')[-1], ImageStat.Stat(im).mean[0], ImageStat.Stat(q).mean[0], ImageStat.Stat(q).stddev[0], ImageStat.Stat(r).mean[0]))" $1; }
echo "== privacy mask, solid white over the top-left quarter"
snap base; main base
cfg --data-urlencode "mask.color=#ffffff" --data-urlencode "mask.areas=0,0,959,539"
sleep 2; snap mask; main mask
lum $D/base.jpg; lum $D/mask.jpg; lum $D/base.png; lum $D/mask.png
echo "== mosaic"
cfg --data-urlencode "mask.color=mosaic"; sleep 2; snap mosaic; lum $D/mosaic.jpg
cfg --data-urlencode "mask.areas="; sleep 2; snap nomask; lum $D/nomask.jpg
echo "== WDR -50 / +50"
cfg --data-urlencode "image.wdr=-50"; sleep 3; snap wdrm
cfg --data-urlencode "image.wdr=50"; sleep 3; snap wdrp
cfg --data-urlencode "image.wdr=0"; sleep 3; snap wdr0
lum $D/wdrm.jpg; lum $D/wdr0.jpg; lum $D/wdrp.jpg
echo "== anti-flicker"
cfg --data-urlencode "image.flicker=60"; cfg --data-urlencode "image.flicker=off"; cfg --data-urlencode "image.flicker=50"; cfg --data-urlencode "image.flicker=70"
echo "== slider survives a day/night switch"
curl "${A[@]}" -d "name=isp.brightness&value=25" http://$H/api/set; echo
MODE=$(curl -s -m 5 -u admin:admin --digest http://$H/api/params | grep "^night.mode=" | cut -d= -f2); echo "mode was $MODE"
curl "${A[@]}" -d "name=night.mode&value=night" http://$H/api/set; echo; sleep 6
curl "${A[@]}" -d "name=night.mode&value=day" http://$H/api/set; echo; sleep 6
curl "${A[@]}" -d "name=night.mode&value=$MODE" http://$H/api/set; echo
curl "${A[@]}" -d "name=isp.brightness&value=0" http://$H/api/set; echo
echo 'grep -E "re-applying|imgx|ERR" /tmp/sd/maint/ak_rtsp.log | tail -5' | python3 "$(dirname "$0")/../../tools/dev/camsh.py" $H 8 - | grep -E "control|imgx|ERR"
echo "== recording on sound (threshold below room noise)"
cfg --data-urlencode "sound.threshold=-60" --data-urlencode "sound.hold=3" --data-urlencode "sound.enabled=on" --data-urlencode "record.mode=sound" --data-urlencode "record.post=3"
sleep 20
cfg --data-urlencode "sound.enabled=off" --data-urlencode "sound.threshold=-30" --data-urlencode "sound.hold=5"
sleep 10
cfg --data-urlencode "record.mode=off" --data-urlencode "record.post=10"
DAY=$(curl -s -m 5 -u admin:admin --digest http://$H/api/rec/list | python3 -c "import json,sys; print(json.load(sys.stdin)['days'][0]['day'])")
curl -s -m 5 -u admin:admin --digest "http://$H/api/rec/list?day=$DAY" | python3 -c "import json,sys; [print('  ',f['name'],f['size']) for f in json.load(sys.stdin)['files'][:4]]"
echo DONE
