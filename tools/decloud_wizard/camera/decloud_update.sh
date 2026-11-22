#!/bin/sh
# Update an already decloud camera: invoked over the maintenance Telnet port by
# `install.sh --update`, after the wizard sent the new image and flash_tool.
#   sh decloud_update.sh <image> <image md5> <current APP md5 the wizard read> <flash_tool>
# Refuses unless the exact MTD layout matches, the image arrived intact, the
# camera runs an ak_rtsp build (not the stock APP: use the full wizard), and the
# APP is still exactly the one the wizard read and built from. Backs up the
# current APP to the SD card, writes with per-block verification (flash_tool),
# checks the whole partition, and restores the backup on any failure.

SD=/tmp/sd
LOG="$SD/logs/decloud_update.log"
TMPLOG=/tmp/decloud_update_tool.log
EXPECTSIZE=5242880
IMG="$1"; IMG_MD5="$2"; WANT_CUR="$3"; TOOL="$4"

mkdir -p "$SD/logs" "$SD/update"
echo "=== LSC decloud update: $(date) ===" | tee -a "$LOG"

msg() { echo "$*" | tee -a "$LOG"; }
fail() { msg "ERROR: $*"; msg "DECLOUD_UPDATE_FAILED"; exit 1; }

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

[ -n "$IMG" ] && [ -n "$IMG_MD5" ] && [ -n "$WANT_CUR" ] && [ -n "$TOOL" ] || fail "usage: decloud_update.sh <image> <md5> <current md5> <flash_tool>"
check_layout || fail "Unsupported flash partition layout. Nothing was written."
[ -f "$TOOL" ] || fail "flash_tool is missing."
chmod +x "$TOOL" || fail "Could not make flash_tool executable."
[ -f "$IMG" ] || fail "New firmware image is missing."
[ "$(stat -c%s "$IMG" 2>/dev/null)" = "$EXPECTSIZE" ] || fail "New firmware image has the wrong size."
[ "$(md5sum "$IMG" | awk '{print $1}')" = "$IMG_MD5" ] || fail "New firmware image is corrupt (checksum). Nothing was written."
[ "$(dd if="$IMG" bs=4 count=1 2>/dev/null)" = "hsqs" ] || fail "New firmware image is not a squashfs image."

# The camera must run an ak_rtsp build: the stock APP is updated by the full wizard only.
MNT=$(awk '$1=="/dev/mtdblock7" {print $2; exit}' /proc/mounts)
[ -n "$MNT" ] || fail "The APP partition is not mounted; cannot check what the camera runs."
[ -f "$MNT/bin/ak_rtsp" ] || fail "This camera does not run ak_rtsp (stock firmware?). Use the full wizard instead."
[ -f "$MNT/bin/anyka_ipc" ] && fail "This camera still has the stock anyka_ipc. Use the full wizard instead."

# Read the raw MTD character device: no page cache involved.
CUR=$(md5sum /dev/mtd7 | awk '{print $1}')
msg "Current APP MD5: $CUR"
msg "New image MD5:   $IMG_MD5"
[ "$CUR" = "$WANT_CUR" ] || fail "The APP changed since the wizard read it. Nothing was written."
if [ "$CUR" = "$IMG_MD5" ]; then
    msg "This exact firmware is already installed."
    msg "DECLOUD_UPDATE_SUCCESS"
    exit 0
fi

BACKUP="$SD/update/mtd7_before_$CUR.bin"
dd if=/dev/mtd7 of="$BACKUP" bs=64k 2>/dev/null
sync
[ "$(md5sum "$BACKUP" | awk '{print $1}')" = "$CUR" ] || fail "Backup of the current APP to the SD card failed. Nothing was written."
msg "Current APP backed up to $BACKUP"

msg "Writing the new APP with per-block verification..."
sync
rm -f "$TMPLOG"
"$TOOL" "$IMG" /dev/mtd7 >"$TMPLOG" 2>&1
RC=$?
cat "$TMPLOG" | tee -a "$LOG"
AFTER=$(md5sum /dev/mtd7 | awk '{print $1}')
msg "APP MD5 after write: $AFTER"
if [ "$RC" -eq 0 ] && [ "$AFTER" = "$IMG_MD5" ]; then
    echo "$IMG_MD5" > "$SD/update/UPDATE_SUCCESS"
    sync
    msg "DECLOUD_UPDATE_SUCCESS"
    exit 0
fi

msg "Write failed (code $RC). Restoring the previous APP..."
rm -f "$TMPLOG"
"$TOOL" "$BACKUP" /dev/mtd7 >"$TMPLOG" 2>&1
RRC=$?
cat "$TMPLOG" | tee -a "$LOG"
AFTER=$(md5sum /dev/mtd7 | awk '{print $1}')
if [ "$RRC" -eq 0 ] && [ "$AFTER" = "$CUR" ]; then
    sync
    msg "DECLOUD_UPDATE_RESTORED"
    exit 2
fi

msg "Restore also failed (code $RRC). KEEP THE CAMERA POWERED. Backup: $BACKUP"
msg "DECLOUD_UPDATE_FAILED"
exit 3
