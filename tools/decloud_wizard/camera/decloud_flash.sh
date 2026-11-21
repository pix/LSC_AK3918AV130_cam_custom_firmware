#!/bin/sh
# Invoked over the login-free local Telnet port by install.sh.
# Refuses to write unless the exact MTD layout, backup and current stock APP match.

SD=/tmp/sd
LOG="$SD/logs/decloud_flash.log"
TMPLOG=/tmp/decloud_flash_tool.log
BACKUP="$SD/original_firmware/mtdblock7.bin"
TOOL="$SD/flash_tool"
EXPECTSIZE=5242880

mkdir -p "$SD/logs"

echo "=== LSC decloud flash: $(date) ===" | tee -a "$LOG"

check_layout() {
    grep -q '^mtd0: 00032000 00001000 "UBOOT"$' /proc/mtd && \
    grep -q '^mtd1: 00001000 00001000 "ENV"$' /proc/mtd && \
    grep -q '^mtd2: 00001000 00001000 "ENVBK"$' /proc/mtd && \
    grep -q '^mtd3: 00010000 00001000 "DTB"$' /proc/mtd && \
    grep -q '^mtd4: 00180000 00001000 "KERNEL"$' /proc/mtd && \
    grep -q '^mtd5: 000fc000 00001000 "ROOTFS"$' /proc/mtd && \
    grep -q '^mtd6: 00040000 00001000 "CONFIG"$' /proc/mtd && \
    grep -q '^mtd7: 00500000 00001000 "APP"$' /proc/mtd
}

msg() { echo "$*" | tee -a "$LOG"; }
fail() { msg "ERROR: $*"; msg "DECLOUD_FLASH_FAILED"; exit 1; }

check_layout || fail "Unsupported flash partition layout. Nothing was written."
[ -f "$BACKUP" ] || fail "Stock APP backup is missing."
[ "$(stat -c%s "$BACKUP" 2>/dev/null)" = "$EXPECTSIZE" ] || fail "Stock APP backup has wrong size."
[ -f "$TOOL" ] || fail "flash_tool is missing."
chmod +x "$TOOL" || fail "Could not make flash_tool executable."

IMG=""
for f in "$SD"/custom_firmware/ak_rtsp_firmware_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9].squashfs; do
    [ -f "$f" ] && IMG="$f"
done
[ -n "$IMG" ] || fail "No custom firmware image found."
[ "$(stat -c%s "$IMG" 2>/dev/null)" = "$EXPECTSIZE" ] || fail "Custom firmware image has wrong size."

# Strong same-camera/state guard: the APP currently in flash must still exactly
# match the backup made during Stage 1. If it does not, refuse to auto-flash.
STOCK_MD5=$(md5sum "$BACKUP" | awk '{print $1}')
CURRENT_MD5=$(md5sum /dev/mtdblock7 | awk '{print $1}')
msg "Stock APP MD5:   $STOCK_MD5"
msg "Current APP MD5: $CURRENT_MD5"
[ "$STOCK_MD5" = "$CURRENT_MD5" ] || fail "Current APP does not match the Stage 1 stock backup."

msg "Hardware, backup and current APP verified."
msg "Custom image: $IMG"
msg "Custom MD5: $(md5sum "$IMG" | awk '{print $1}')"
msg "Starting verified raw MTD flash..."
sync

rm -f "$TMPLOG"
"$TOOL" "$IMG" /dev/mtd7 >"$TMPLOG" 2>&1
RC=$?
cat "$TMPLOG" | tee -a "$LOG"
if [ "$RC" -eq 0 ]; then
    echo "flash success" > "$SD/FLASH_SUCCESS"
    sync
    msg "DECLOUD_FLASH_SUCCESS"
    exit 0
fi

msg "Custom flash returned code $RC. Attempting automatic stock APP restore..."
rm -f "$TMPLOG"
"$TOOL" "$BACKUP" /dev/mtd7 >"$TMPLOG" 2>&1
RRC=$?
cat "$TMPLOG" | tee -a "$LOG"
if [ "$RRC" -eq 0 ]; then
    echo "stock restored" > "$SD/FLASH_RESTORED"
    sync
    msg "DECLOUD_RESTORE_SUCCESS"
    exit 2
fi

msg "Restore also failed (code $RRC). KEEP THE CAMERA POWERED."
msg "DECLOUD_FLASH_FAILED"
exit 3
