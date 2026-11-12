#!/bin/sh
# Fetch the default custom sound into sounds/custom.wav (git-ignored), checked
# against a SHA-256. Usage: fetch_sound.sh <url> <sha256>  (from the Makefile:
# CUSTOM_SOUND_URL / CUSTOM_SOUND_SHA256). Best effort: never fails the build.
#  - sounds/custom.wav already there (your own sound): left alone.
#  - empty URL: nothing fetched (the custom sound is then empty).
#  - empty SHA-256: the download is used unchecked.
cd "$(dirname "$0")/.." || exit 0
URL=${1:-}; SHA=${2:-}; OUT=sounds/custom.wav
[ -f "$OUT" ] && exit 0
[ -n "$URL" ] || exit 0
mkdir -p sounds
TMP=sounds/.custom.wav.part
rm -f "$TMP"
if command -v curl >/dev/null 2>&1; then curl -sSfL --max-time 30 -o "$TMP" "$URL" || rm -f "$TMP"
elif command -v wget >/dev/null 2>&1; then wget -q -T 30 -O "$TMP" "$URL" || rm -f "$TMP"
fi
if [ ! -s "$TMP" ]; then echo "custom sound: download failed ($URL), building without it"; rm -f "$TMP"; exit 0; fi
if [ -n "$SHA" ] && ! echo "$SHA  $TMP" | sha256sum -c - >/dev/null 2>&1; then
    echo "custom sound: SHA-256 mismatch for $URL, building without it"; rm -f "$TMP"; exit 0
fi
mv "$TMP" "$OUT" && echo "custom sound: fetched $URL"
exit 0
