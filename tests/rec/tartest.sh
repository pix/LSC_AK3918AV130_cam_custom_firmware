#!/bin/bash
H=${1:-${CAMERA_IP:?usage: $0 <camera ip> or set CAMERA_IP}}; D=${TMPDIR:-/tmp}/tartest; mkdir -p $D/x
A=(-s -m 30 -u admin:admin --digest)
LIST=$(curl "${A[@]}" http://$H/api/rec/all | python3 -c "import json,sys; print('\n'.join(f['d']+'/'+f['n'] for f in json.load(sys.stdin) if f['d'] in ('2026-11-09','2026-11-10')))")
echo "$LIST" | wc -l | sed 's/^/selected /'
TOK=$(curl "${A[@]}" -H X-Requested-With:lsc -H "Content-Type: text/plain" --data-binary "$LIST" http://$H/api/rec/tar)
echo "token reply: $TOK"
T=$(echo "$TOK" | python3 -c "import json,sys; print(json.load(sys.stdin)['token'])")
curl "${A[@]}" -m 120 -o $D/all.tar -w "tar download: %{http_code} %{size_download} bytes in %{time_total}s\n" http://$H/rec-tar/$T
curl "${A[@]}" -o /dev/null -w "token reuse: %{http_code}\n" http://$H/rec-tar/$T
tar tf $D/all.tar | wc -l | sed 's/^/entries in tar /'; tar xf $D/all.tar -C $D/x && echo "tar extracts cleanly"
bad=0; for f in $(echo "$LIST" | head -6); do curl "${A[@]}" -o $D/one "http://$H/rec/$f"; cmp -s $D/one $D/x/$f || { echo "MISMATCH $f"; bad=1; }; done; [ $bad = 0 ] && echo "6 sampled files identical to individual downloads"
echo "== bulk delete the test days"
curl "${A[@]}" -H X-Requested-With:lsc -H "Content-Type: text/plain" --data-binary "$LIST" http://$H/api/rec/delete-many; echo
curl "${A[@]}" http://$H/api/rec/list; echo
