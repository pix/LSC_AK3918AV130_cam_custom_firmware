CC      = zig cc -target arm-linux-musleabi -mcpu=arm926ej_s
CFLAGS  = -std=gnu99 -mfloat-abi=soft -O2 -Wall -g -ffp-contract=off \
          -I. \
          -I../smolrtsp/include \
          -I../smolrtsp/build/_deps/slice99-src \
          -I../smolrtsp/build/_deps/datatype99-src \
          -I../smolrtsp/build/_deps/interface99-src \
          -I../smolrtsp/build/_deps/metalang99-src/include \
          -DSMOLRTSP_WITH_H264 \
          -Ithird_party/mbedtls/include -DMBEDTLS_CONFIG_FILE='"tls_config.h"'
LDFLAGS = -pthread -static

TARGET     = ak_rtsp
TARGET_DBG = isp_dump
TARGET_HOOK = isp_hook.so
TARGET_TOOL = flash_tool
TARGET_DIV0 = patch_div0
.DEFAULT_GOAL := all

# Custom speaker sound ("Custom sound" button, webhook "custom"): your own
# sounds/custom.wav, else this default is downloaded once and checksum-checked
# (the Windows 3.1 "Tada", via winhistory.de). Opt out: make CUSTOM_SOUND_URL=
# Other sound: make CUSTOM_SOUND_URL=https://... CUSTOM_SOUND_SHA256=<sha256 or empty>
CUSTOM_SOUND_URL    ?= https://www.winhistory.de/more/winstart/down/owin31.wav
CUSTOM_SOUND_SHA256 ?= e78187f27ea746aee7ceda0d8daf701d8d3dc469b24c7b25b71fa32bf773b136
.PHONY: fetch-sound
fetch-sound:
	@./third_party/fetch_sound.sh "$(CUSTOM_SOUND_URL)" "$(CUSTOM_SOUND_SHA256)"

sounds_custom.c: gen_sound.py $(wildcard sounds/custom.wav) | fetch-sound
	python3 -I gen_sound.py sounds/custom.wav sounds_custom.c

web_page.c: web/index.html gen_web_page.py
	python3 -I gen_web_page.py web/index.html web_page.c

web_rec_page.c: web/recordings.html gen_web_page.py
	python3 -I gen_web_page.py web/recordings.html web_rec_page.c web_recordings_html

web_term_page.c: web/terminal.html gen_web_page.py
	python3 -I gen_web_page.py web/terminal.html web_term_page.c web_terminal_html

# -----------------------------------------------------------------------
# Build versioning
#
# build_number.txt  — persists the counter across builds (not committed)
# version_info.h    — regenerated on every make by gen_version.ps1
#
# version.o has a FORCE dependency so it always recompiles. Its recipe
# calls gen_version.ps1 (pure PowerShell, no sh required) which:
#   1. increments build_number.txt
#   2. writes version_info.h with BUILD_NUMBER / BUILD_TIMESTAMP / GIT_HASH
# -----------------------------------------------------------------------
BUILD_NUM_FILE := build_number.txt

FORCE:
.PHONY: FORCE all clean upload firmware

ASM_SRCS = arm_atomics.S
SRCS    = main.c \
          isp.c \
          vi.c \
          venc.c \
          audio.c \
          rtsp.c \
          ircut.c \
          ae.c \
          aec_custom.c \
          night.c \
          control.c \
          version.c \
          md5.c \
          auth.c \
          jpeg_enc.c \
          web.c \
          web_page.c \
          web_rec_page.c \
          web_term_page.c \
          resetkey.c \
          monotime.c \
          system.c \
          netcfg.c \
          qrwifi.c \
          sha1.c \
          live.c \
          speaker.c \
          osd.c \
          osd_hw.c \
          osd_font.c \
          tls.c \
          snapshot.c \
          mqtt.c \
          events.c \
          sounddet.c \
          tamper.c \
          sdn.c \
          sched.c \
          substream.c \
          backup.c \
          term.c \
          npu.c \
          md.c \
          fmp4.c \
          record.c \
          onvif.c \
          imgx.c \
          aec.c \
          third_party/speexdsp/mdf.c \
          third_party/speexdsp/preprocess.c \
          third_party/speexdsp/fftwrap.c \
          third_party/speexdsp/filterbank.c \
          third_party/speexdsp/kiss_fft.c \
          third_party/speexdsp/kiss_fftr.c \
          sounds_custom.c \
          third_party/quirc/quirc.c \
          third_party/quirc/decode.c \
          third_party/quirc/identify.c \
          third_party/quirc/version_db.c \
          $(wildcard ../smolrtsp/src/*.c) \
          $(wildcard ../smolrtsp/src/types/*.c) \
          $(wildcard ../smolrtsp/src/writer/*.c) \
          $(wildcard ../smolrtsp/src/transport/*.c) \
          ../smolrtsp/src/nal/h264.c
OBJS    = $(SRCS:.c=.o) $(ASM_SRCS:.S=.o)

all: $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_TOOL) $(TARGET_DIV0)

# -----------------------------------------------------------------------
# debug-dump — builds ak_rtsp with DEBUG_IOCTL_DUMP defined, so venc.c logs
# our own CREATE_ENC payload to /tmp/ak_rtsp_ioctl_dump.log in the same
# format isp_hook.c uses for stock's capture (see venc_dump.h). Not part of
# `all`/`$(TARGET)` — the normal build never defines DEBUG_IOCTL_DUMP.
# Forces a full object rebuild first (the macro changes venc.o's contents
# but make has no way to know that from timestamps alone).
# -----------------------------------------------------------------------
.PHONY: debug-dump
debug-dump:
	"$(MAKE)" clean
	"$(MAKE)" $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) CFLAGS="$(CFLAGS) -DDEBUG_IOCTL_DUMP"

# -----------------------------------------------------------------------
# aec-custom — builds ak_rtsp with AEC_CUSTOM defined, swapping ae.c's
# on-chip-tick-only AE loop for aec_custom.c's software AEC loop.
# Compile-time only, not a runtime toggle. Not part of `all`/the normal build.
# -----------------------------------------------------------------------
.PHONY: aec-custom
aec-custom:
	"$(MAKE)" clean
	"$(MAKE)" $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_TOOL) $(TARGET_DIV0) CFLAGS="$(CFLAGS) -DAEC_CUSTOM"

# -----------------------------------------------------------------------
# aec-custom-nosuspend — DIAGNOSTIC ONLY, not for shipping. Same as
# aec-custom but skips the ISP_INNER_AE_SUSPEND call, to isolate whether
# AE_SUSPEND itself (vs. our own control loop) is responsible for the much
# heavier div0 flood observed with aec-custom.
# -----------------------------------------------------------------------
.PHONY: aec-custom-nosuspend
aec-custom-nosuspend:
	"$(MAKE)" clean
	"$(MAKE)" $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_TOOL) $(TARGET_DIV0) CFLAGS="$(CFLAGS) -DAEC_CUSTOM -DAEC_SKIP_SUSPEND"

$(TARGET_HOOK): isp_hook.c
	$(CC) -std=gnu99 -mfloat-abi=soft -O1 -fPIC -shared -nostdlib -ffreestanding \
	      -mcpu=arm926ej_s -o $@ $<

$(TARGET_DBG): isp_dump.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(TARGET_TOOL): flash_tool.c arm_atomics.S
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_DIV0): patch_div0.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# mbedTLS (HTTPS): fetched + checksum-verified by third_party/fetch_mbedtls.sh,
# compiled with tls_config.h into a static library (unused parts drop out).
MBEDTLS_DIR  = third_party/mbedtls
MBEDTLS_LIB  = third_party/libmbedtls_ak.a
$(MBEDTLS_DIR)/library/ssl_tls.c:
	./third_party/fetch_mbedtls.sh
$(MBEDTLS_LIB): $(MBEDTLS_DIR)/library/ssl_tls.c tls_config.h
	rm -rf third_party/mbedtls_obj && mkdir -p third_party/mbedtls_obj
	for f in $(MBEDTLS_DIR)/library/*.c; do \
	  $(CC) -std=gnu99 -mfloat-abi=soft -O2 -I. -I$(MBEDTLS_DIR)/include \
	        -DMBEDTLS_CONFIG_FILE='"tls_config.h"' -c $$f \
	        -o third_party/mbedtls_obj/$$(basename $$f .c).o || exit 1; done
	rm -f $@ && $(AR) rcs $@ third_party/mbedtls_obj/*.o
# SpeexDSP (echo canceller): fixed point, its own config.h
SPEEX_OBJS = aec.o $(patsubst %.c,%.o,$(wildcard third_party/speexdsp/*.c))
$(SPEEX_OBJS): CFLAGS += -DHAVE_CONFIG_H -Ithird_party/speexdsp

tls.o: $(MBEDTLS_LIB)
tls.o: CFLAGS += -DTLS_BUILD_EPOCH=$(shell date +%s)L

$(TARGET): $(OBJS) $(MBEDTLS_LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# version.o — always rebuilt (FORCE). Calls gen_version.ps1 first to
# increment the counter and regenerate version_info.h, then compiles.
version.o: version.c FORCE
	./gen_version.sh
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	-pwsh -NoProfile -NonInteractive -Command "$$ErrorActionPreference='SilentlyContinue'; Get-ChildItem -Path '.' -Filter '*.o' | Remove-Item -Force; Get-ChildItem -Path '../smolrtsp' -Recurse -Filter '*.o' | Remove-Item -Force; Remove-Item -Force '$(TARGET)','$(TARGET_DBG)','$(TARGET_HOOK)','$(TARGET_TOOL)','$(TARGET_DIV0)','version_info.h'; exit 0"

# -----------------------------------------------------------------------
# firmware — builds the flashable, timestamped APP squashfs image
# (ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs, written next to build_firmware.sh
# unless FW_DIR is overridden). Rebuilds ak_rtsp natively first, then shells
# out to WSL for build_firmware.sh (mksquashfs/unsquashfs aren't available on
# Windows). Requires an app_extracted/ dump of your own camera's APP
# partition — see docs/building_your_own_firmware.md for the full walkthrough
# and what PATCHED/FW_DIR should point at.
# -----------------------------------------------------------------------
firmware: $(TARGET)
	wsl -d Ubuntu -- bash -c 'cd "$$(wslpath "$(CURDIR)")" && bash build_firmware.sh'

# Upload binary + launch script to camera over FTP.
# CAMERA_IP is this project's own dev camera on its LAN — override it for yours,
# e.g. `make upload CAMERA_IP=192.168.1.50`. The root:admin credential assumes
# you've set up the /etc/config/passwd backdoor from
# docs/flash_partition_map_and_rootfs.md with that same password.
CAMERA_IP = 192.168.50.4
FLASH_SCRIPTS = ../../tools/flash_scripts
upload: $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_TOOL) $(TARGET_DIV0)
	curl -T $(TARGET) ftp://$(CAMERA_IP)/mnt/ --user root:admin
	curl -T $(TARGET_DBG) ftp://$(CAMERA_IP)/mnt/ --user root:admin
	curl -T $(TARGET_HOOK) ftp://$(CAMERA_IP)/mnt/ --user root:admin
	curl -T $(TARGET_TOOL) ftp://$(CAMERA_IP)/mnt/ --user root:admin
	curl -T $(TARGET_DIV0) ftp://$(CAMERA_IP)/mnt/ --user root:admin
	curl -T $(FLASH_SCRIPTS)/start_rtsp.sh ftp://$(CAMERA_IP)/mnt/ --user root:admin
	curl -T $(FLASH_SCRIPTS)/install_with_flash_tool.sh ftp://$(CAMERA_IP)/mnt/ --user root:admin
	curl -T $(FLASH_SCRIPTS)/quiet_and_flash_test.sh ftp://$(CAMERA_IP)/mnt/ --user root:admin
