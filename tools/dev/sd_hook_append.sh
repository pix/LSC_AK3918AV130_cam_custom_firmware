# Append to the decloud SD card's hack.sh (it runs at every boot, before
# ak_rtsp starts). Gives two development aids, both inert when unused:
#  1. SD test binaries: if /tmp/sd/maint/USE_TEST_BINARY names an executable
#     in /tmp/sd/maint/, it is bind-mounted over /usr/bin/ak_rtsp for this
#     boot (tools/dev/deploy_test.sh sets it). Delete or rename the file to go
#     back to the flashed build; pulling the SD card does the same.
#  2. Persistent kernel log: dmesg is appended to /tmp/sd/maint/dmesg.log
#     every 2 s (the kernel log is lost on the reboot that follows a crash).
AKR_BIN=/tmp/sd/maint/$(cat /tmp/sd/maint/USE_TEST_BINARY 2>/dev/null)
if [ -f /tmp/sd/maint/USE_TEST_BINARY ] && [ -f "$AKR_BIN" ] && [ -x "$AKR_BIN" ]; then mount --bind "$AKR_BIN" /usr/bin/ak_rtsp && echo "[akr] $AKR_BIN bound over /usr/bin/ak_rtsp"; fi
setsid sh -c 'echo "=== boot $(date)" >> /tmp/sd/maint/dmesg.log; while true; do dmesg -c >> /tmp/sd/maint/dmesg.log 2>/dev/null; sync; sleep 2; done' </dev/null >/dev/null 2>&1 &
