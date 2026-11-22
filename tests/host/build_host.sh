#!/bin/bash
# Build the camera firmware's non-hardware code for the PC.
#   build_host.sh <out dir> [asan|valgrind|fuzz]
# asan: gcc -O1 ASan + UBSan; valgrind: gcc -O0 (run under valgrind); fuzz: clang
# objects with -fsanitize=fuzzer-no-link,address,undefined for libFuzzer harnesses.
# Produces <out>/lib.a (modules + smolrtsp + mbedTLS), <out>/host_server.
# Hardware (video, encoder, ISP, audio, IR cut, LEDs) comes from hw_stubs.c.
# Dangerous calls are wrapped at link time (wraps.c): system/popen/reboot/mount/exec*
# only log, nothing on the PC is touched; paths point into <out>/root.
set -eu
OUT=$(mkdir -p "$1" && cd "$1" && pwd); MODE=${2:-asan}
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/../.." && pwd)
S=$R/src/ak_rtsp; [ -d "$S" ] || S=$R/firmware/src/ak_rtsp
M=$S/third_party/mbedtls
SM=$S/../smolrtsp
ROOT=$OUT/root
mkdir -p "$ROOT/cfg" "$ROOT/sd/maint" "$OUT/obj" "$OUT/mb" "$OUT/smol"

case $MODE in
  asan)     CC=gcc;   OPT=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -DAKR_STACK_SCALE=8) ;;
  valgrind) CC=gcc;   OPT=(-O0 -g) ;;
  fuzz)     CC=clang; OPT=(-O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined -DAKR_STACK_SCALE=8) ;;
  *) echo "mode: asan | valgrind | fuzz"; exit 1 ;;
esac
SMOLI=(-I"$SM/include" -I"$SM/build/_deps/slice99-src" -I"$SM/build/_deps/datatype99-src"
       -I"$SM/build/_deps/interface99-src" -I"$SM/build/_deps/metalang99-src/include" -DSMOLRTSP_WITH_H264)
# every path the firmware writes, inside the work directory
PATHS=(-DTLS_DIR="\"$ROOT/cfg\"" -DCFG_DIR="\"$ROOT/cfg\"" -DREC_ROOT="\"$ROOT/sd\""
       -DSETTINGS_FILE="\"$ROOT/cfg/ak_rtsp_settings\"" -DEXT_SETTINGS_FILE="\"$ROOT/cfg/ak_rtsp_ext\""
       -DSD_LOG_FLAG="\"$ROOT/sd/maint/ak_rtsp.log.enable\"" -DSD_LOG_FILE="\"$ROOT/sd/maint/ak_rtsp.log\""
       -DLAST_CRASH_FILE="\"$ROOT/cfg/ak_rtsp_lastcrash\"" -DHOOK_TOKEN_FILE="\"$ROOT/cfg/ak_rtsp_hook\""
       -DAUTH_FILE="\"$ROOT/cfg/ak_rtsp_auth\"" -DAUTH_ONVIF_FILE="\"$ROOT/cfg/ak_rtsp_onvif_pw\""
       -DOSD_FILE="\"$ROOT/cfg/ak_rtsp_osd\"" -DNET_STA_CONF="\"$ROOT/cfg/wpa_supplicant.conf\""
       -DNET_STA_CONF_NAME="\"wpa_supplicant.conf\"" -DNET_AP_FILE="\"$ROOT/cfg/ak_rtsp_ap\""
       -DNET_TMP="\"$ROOT/net\"" -DNETCFG_SIM -DQRWIFI_NO_CAMERA -DAKR_HOST)
CFL=(-std=gnu99 -w "${OPT[@]}" -iquote "$S" -I"$M/include" -DMBEDTLS_CONFIG_FILE='"tls_config.h"' "${SMOLI[@]}" "${PATHS[@]}")
mkdir -p "$ROOT/net"

# mbedTLS (cached per mode)
if [ ! -f "$OUT/mb/.done" ]; then
  ls "$M"/library/*.c | xargs -P"$(nproc)" -I{} sh -c '"$0" "$@" -I"'"$M"'/library" -c {} -o "'"$OUT"'/mb/$(basename {} .c).o"' "$CC" "${CFL[@]}"
  touch "$OUT/mb/.done"
fi
# SmolRTSP
for f in "$SM"/src/*.c "$SM"/src/types/*.c "$SM"/src/writer/*.c "$SM"/src/transport/*.c "$SM"/src/nal/h264.c; do
  echo "$f"
done | xargs -P"$(nproc)" -I{} sh -c 'o="'"$OUT"'/smol/$(echo {} | md5sum | cut -c1-8)_$(basename {} .c).o"; "$0" "$@" -c {} -o "$o"' "$CC" "${CFL[@]}"

# generated pages / roots
(cd "$S" && python3 -I gen_web_page.py web/index.html "$OUT/web_page.c" \
         && python3 -I gen_web_page.py web/recordings.html "$OUT/web_rec_page.c" web_recordings_html \
         && python3 -I gen_web_page.py web/terminal.html "$OUT/web_term_page.c" web_terminal_html \
         && python3 -I gen_web_page.py acme_roots.pem "$OUT/acme_roots.c" acme_roots_pem)

MODS="web auth control system events notify mqtt onvif onvif_g recidx fmp4r replay record fmp4 hls cast backup tls acme term sched sounddet md5 sha1
      jpeg_enc monotime netcfg apframe live snapshot rtsp aec sdn osd osd_font version resetkey qrwifi speaker md_logic"
{ for m in $MODS; do echo "$S/$m.c"; done; echo "$OUT/web_page.c"; echo "$OUT/web_rec_page.c"; echo "$OUT/web_term_page.c";
  echo "$OUT/acme_roots.c"; echo "$T/hw_stubs.c"; echo "$T/wraps.c";
  ls "$S"/third_party/quirc/*.c; } |
  xargs -P"$(nproc)" -I{} sh -c '"$0" "$@" -c {} -o "'"$OUT"'/obj/$(basename {} .c).o"' "$CC" "${CFL[@]}" -I"$T" >/dev/null
# custom sound (generated, not in git): the real one if built, else silence
if [ -f "$S/sounds_custom.c" ]; then cp "$S/sounds_custom.c" "$OUT/sounds_custom.c"
else printf '#include <stddef.h>\n#include <stdint.h>\nconst size_t speaker_custom_len = 8;\nconst int16_t speaker_custom_pcm[8];\n' > "$OUT/sounds_custom.c"; fi
"$CC" "${CFL[@]}" -c "$OUT/sounds_custom.c" -o "$OUT/obj/sounds_custom.o"
# aec.c + SpeexDSP: as in the Makefile (config.h)
{ echo "$S/aec.c"; ls "$S"/third_party/speexdsp/*.c; } |
  xargs -P"$(nproc)" -I{} sh -c '"$0" "$@" -c {} -o "'"$OUT"'/obj/$(basename {} .c).o"' "$CC" "${CFL[@]}" -DHAVE_CONFIG_H -I"$S/third_party/speexdsp" >/dev/null
rm -f "$OUT/lib.a"
ar rcs "$OUT/lib.a" "$OUT"/obj/*.o "$OUT"/smol/*.o "$OUT"/mb/*.o
WRAPS=(-Wl,--wrap=system -Wl,--wrap=popen -Wl,--wrap=reboot -Wl,--wrap=mount -Wl,--wrap=umount
       -Wl,--wrap=execle -Wl,--wrap=execve -Wl,--wrap=execl -Wl,--wrap=execvp -Wl,--wrap=settimeofday
       -Wl,--wrap=sethostname -Wl,--wrap=pthread_attr_setstacksize)
printf '%s\n' "${WRAPS[@]}" > "$OUT/wraps.flags"
printf '%s\n' "${CFL[@]}" > "$OUT/cflags"
echo "$CC" > "$OUT/cc"
if [ -f "$T/host_server.c" ]; then
  "$CC" "${CFL[@]}" -I"$T" "$T/host_server.c" "$OUT/lib.a" "${WRAPS[@]}" -lpthread -lm -o "$OUT/host_server"
fi
echo "built $MODE in $OUT"
