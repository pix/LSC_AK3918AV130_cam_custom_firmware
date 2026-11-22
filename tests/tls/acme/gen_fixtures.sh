#!/bin/sh
# Certificates and keys for test_custom_cert.c / test_acme.c (openssl 3.4+).
# Usage: gen_fixtures.sh <dir>
set -eu
F=$1
mkdir -p "$F"
cd "$F"
o() { openssl "$@" 2>/dev/null; }
past=$(date -u -d '-40 days' +%Y%m%d%H%M%SZ); past2=$(date -u -d '-10 days' +%Y%m%d%H%M%SZ)
future=$(date -u -d '+10 days' +%Y%m%d%H%M%SZ); future2=$(date -u -d '+90 days' +%Y%m%d%H%M%SZ)
soon=$(date -u -d '+10 days' +%Y%m%d%H%M%SZ); yesterday=$(date -u -d '-1 days' +%Y%m%d%H%M%SZ)

o req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout root.key -out root.crt -days 3650 \
  -subj "/CN=Test Root" -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign
o req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout inter.key -out inter.csr -subj "/CN=Test Intermediate"
printf "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n" > inter.ext
o x509 -req -in inter.csr -CA root.crt -CAkey root.key -out inter.crt -days 3000 -extfile inter.ext

SRV="basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n"
# leaf <name> <keyfile> <extensions> [not_before not_after]
leaf() {
  o req -new -key "$2" -subj "/CN=$1" -out "$1.csr"
  printf "%b" "$3" > "$1.ext"
  if [ $# -ge 5 ]; then o x509 -req -in "$1.csr" -CA inter.crt -CAkey inter.key -out "$1.crt" -extfile "$1.ext" -not_before "$4" -not_after "$5"
  else o x509 -req -in "$1.csr" -CA inter.crt -CAkey inter.key -out "$1.crt" -extfile "$1.ext" -days 60; fi
}
key_ec() { o genpkey -algorithm EC -pkeyopt ec_paramgen_curve:"$2" -out "$1"; }
key_rsa() { o genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:"$2" -out "$1"; }

key_ec ec.key P-256;      leaf good "ec.key" "${SRV}subjectAltName=DNS:cam.example.org,DNS:*.wild.example.org\n"
key_ec p384.key P-384;    leaf p384 "p384.key" "${SRV}subjectAltName=DNS:p384.example.org\n"
key_rsa rsa.key 2048;     leaf rsa "rsa.key" "basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:rsa.example.org\n"
key_rsa rsa4096.key 4096; leaf rsa4096 "rsa4096.key" "${SRV}subjectAltName=DNS:big.example.org\n"
key_rsa rsa1024.key 1024;leaf rsa1024 "rsa1024.key" "${SRV}subjectAltName=DNS:small.example.org\n"
key_ec p521.key P-521;    leaf p521 "p521.key" "${SRV}subjectAltName=DNS:p521.example.org\n"
key_ec e1.key P-256;      leaf expired "e1.key" "${SRV}subjectAltName=DNS:old.example.org\n" "$past" "$past2"
key_ec e2.key P-256;      leaf future "e2.key" "${SRV}subjectAltName=DNS:new.example.org\n" "$future" "$future2"
key_ec e3.key P-256;      leaf soon "e3.key" "${SRV}subjectAltName=DNS:cam.example.org,DNS:*.wild.example.org\n" "$yesterday" "$soon"
key_ec e4.key P-256;      leaf nosan "e4.key" "$SRV"
key_ec e5.key P-256;      leaf ipsan "e5.key" "${SRV}subjectAltName=IP:192.168.1.5\n"
key_ec e6.key P-256;      leaf client "e6.key" "basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\nsubjectAltName=DNS:client.example.org\n"
key_ec e7.key P-256;      leaf noeku "e7.key" "basicConstraints=CA:FALSE\nsubjectAltName=DNS:noeku.example.org\n"
key_rsa e8.key 2048;      leaf kuenc "e8.key" "basicConstraints=CA:FALSE\nkeyUsage=critical,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:kuenc.example.org\n"
key_ec e9.key P-256;      leaf isca "e9.key" "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyCertSign\nsubjectAltName=DNS:ca.example.org\n"
key_ec e10.key P-256;     leaf upper "e10.key" "${SRV}subjectAltName=DNS:Cam.Upper.EXAMPLE.org,DNS:bad_name.example.org,DNS:*.*.example.org\n"
many=""; for i in $(seq 1 20); do many="$many${many:+,}DNS:n$i.example.org"; done
key_ec e11.key P-256;     leaf many "e11.key" "${SRV}subjectAltName=$many\n"
key_ec other.key P-256
o pkey -in ec.key -aes256 -passout pass:secret -out ec_enc.key
o pkey -in rsa.key -traditional -out rsa_trad.key
o pkey -in rsa.key -traditional -aes256 -passout pass:secret -out rsa_trad_enc.key
o ecparam -genkey -name prime256v1 -out sec1.key          # "EC PARAMETERS" + "EC PRIVATE KEY"
leaf sec1 "sec1.key" "${SRV}subjectAltName=DNS:sec1.example.org\n"
# a CA certificate for the "extra ACME CA" upload, and a non-CA one
cp root.crt extra_ca.crt
echo ok
