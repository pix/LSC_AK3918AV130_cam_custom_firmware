#!/bin/bash
H=${1:-${CAMERA_IP:?usage: $0 <camera ip> or set CAMERA_IP}}; P=${PKI:?set PKI to a dir with ca.crt/ca.key/broker.crt/broker.key/broker_wrongip.crt}
B=$(dirname "$0")/fakebroker.py; L=$P/b.log
A=(-s -m 20 -u admin:admin --digest -H X-Requested-With:lsc)
cfg(){ curl "${A[@]}" "$@" http://$H/api/config; echo; }
st(){ curl -s -m 10 -u admin:admin --digest http://$H/api/events | python3 -c "import json,sys; print('  camera says:', json.load(sys.stdin)['events']['mqtt'])"; }
broker(){ [ -f $P/b.pid ] && kill $(cat $P/b.pid) 2>/dev/null; sleep 0.5; env "$@" python3 -u -I $B 18883 25 > $L 2>&1 & echo $! > $P/b.pid; sleep 0.5; }
cfg --data-urlencode "device.name=camera-1"
curl -s -u admin:admin --digest -o $P/camera_ca.crt http://$H/api/tls/ca.crt
curl "${A[@]}" -X POST -d "what=ca" http://$H/api/mqtt/clear >/dev/null
echo "== 1: TLS + verify, no broker CA uploaded"
broker TLS_CERT=$P/broker.crt TLS_KEY=$P/broker.key
cfg --data-urlencode "mqtt.host=${BROKER_IP:?set BROKER_IP to the address of this machine}" --data-urlencode "mqtt.port=18883" --data-urlencode "mqtt.user=" --data-urlencode "mqtt.pass=" --data-urlencode "mqtt.tls=on" --data-urlencode "mqtt.tls_verify=on" --data-urlencode "mqtt.client_cert=none" --data-urlencode "mqtt.enabled=on"
sleep 6; st
echo "== 2: upload broker CA -> verified TLS"
curl "${A[@]}" --data-binary @$P/ca.crt "http://$H/api/mqtt/upload?what=ca"; echo
sleep 8; st; grep -E "TLS|CONNECT" $L
echo "== 3: broker requires client cert; camera-issued"
broker TLS_CERT=$P/broker.crt TLS_KEY=$P/broker.key TLS_CLIENT_CA=$P/camera_ca.crt
cfg --data-urlencode "mqtt.client_cert=camera"
sleep 10; st; grep -E "TLS|CONNECT|failed" $L
echo "== 4: CSR flow (custom)"
curl "${A[@]}" -X POST -o $P/cam.csr http://$H/api/mqtt/csr; openssl req -in $P/cam.csr -noout -subject
printf "extendedKeyUsage=clientAuth\n" > $P/cext.cnf
openssl x509 -req -in $P/cam.csr -CA $P/ca.crt -CAkey $P/ca.key -CAcreateserial -out $P/cam_signed.crt -days 30 -extfile $P/cext.cnf 2>/dev/null
curl "${A[@]}" --data-binary @$P/broker.crt "http://$H/api/mqtt/upload?what=cert"; echo "  (wrong cert should be refused ^)"
curl "${A[@]}" --data-binary @$P/cam_signed.crt "http://$H/api/mqtt/upload?what=cert"; echo
broker TLS_CERT=$P/broker.crt TLS_KEY=$P/broker.key TLS_CLIENT_CA=$P/ca.crt
cfg --data-urlencode "mqtt.client_cert=custom"
sleep 10; st; grep -E "TLS|CONNECT|failed" $L
echo "== 5: broker cert for a different IP"
broker TLS_CERT=$P/broker_wrongip.crt TLS_KEY=$P/broker.key
cfg --data-urlencode "mqtt.client_cert=none"
sleep 8; st
kill $(cat $P/b.pid) 2>/dev/null
cfg --data-urlencode "mqtt.enabled=off"
