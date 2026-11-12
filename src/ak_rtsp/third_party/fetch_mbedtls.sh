#!/bin/sh
# Fetch the pinned mbedTLS release used for HTTPS (Apache-2.0) into
# third_party/mbedtls/ (git-ignored). Verifies the SHA-256 before unpacking.
set -eu
cd "$(dirname "$0")"
V=3.6.7
SHA=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
T=mbedtls-$V.tar.bz2
[ -d mbedtls/library ] && grep -q "\"$V\"" mbedtls/include/mbedtls/build_info.h && exit 0
[ -f "$T" ] || curl -sSfL -o "$T" "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$V/$T"
echo "$SHA  $T" | sha256sum -c -
rm -rf mbedtls mbedtls-$V
tar xjf "$T" mbedtls-$V/library mbedtls-$V/include mbedtls-$V/LICENSE
mv mbedtls-$V mbedtls
