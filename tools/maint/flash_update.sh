#!/bin/sh
# Guarded APP (mtd7) update: refuses unless the flash layout is the known
# one, the new image is intact, and the CURRENT APP is a known image
# (wizard build or stock). Backs up current APP to the SD card first and
# restores it automatically if the write fails.
D=/tmp/sd/maint
IMG=$D/app_new.squashfs
NEW_MD5=fa862e137d53fa5f05caeaab2ad071a3
KNOWN_CURRENT="1f7c7f254cc92b5955b3ba5d6fa4e6ab 45f7b0ea5a8a92bc2defe9c1e4769a2a"
SIZE=5242880
LOG=$D/flash_update.log
msg() { echo "$*"; echo "$*" >> $LOG; }
fail() { msg "ERROR: $*"; msg "UPDATE_FLASH_FAILED"; exit 1; }

msg "=== flash_update $(date) ==="
grep -q '^mtd7: 00500000 00001000 "APP"$' /proc/mtd || fail "unexpected mtd7 layout"
grep -q '^mtd6: 00040000 00001000 "CONFIG"$' /proc/mtd || fail "unexpected mtd6 layout"
[ -x $D/flash_tool ] || chmod +x $D/flash_tool 2>/dev/null
[ -x $D/flash_tool ] || fail "flash_tool missing"
[ "$(stat -c%s $IMG 2>/dev/null)" = "$SIZE" ] || fail "image size wrong"
[ "$(md5sum $IMG | cut -c1-32)" = "$NEW_MD5" ] || fail "image checksum wrong (SD copy corrupt?)"

# Read the raw MTD char device: no page cache involved, and no cache drop
# (dropping caches would force programs running from the APP partition to
# re-read code from flash while it is being rewritten).
CUR=$(md5sum /dev/mtd7 | cut -c1-32)
msg "current APP md5: $CUR"
if [ "$CUR" = "$NEW_MD5" ]; then msg "already up to date"; msg "UPDATE_FLASH_SUCCESS"; exit 0; fi
case " $KNOWN_CURRENT " in *" $CUR "*) ;; *) fail "current APP is not a known image; refusing" ;; esac

dd if=/dev/mtd7 of=$D/mtd7_before_update.bin bs=64k 2>/dev/null
sync
[ "$(md5sum $D/mtd7_before_update.bin | cut -c1-32)" = "$CUR" ] || fail "backup of current APP failed"
msg "current APP backed up to $D/mtd7_before_update.bin"

msg "writing new APP..."
if $D/flash_tool $IMG /dev/mtd7 >> $LOG 2>&1; then
    AFTER=$(md5sum /dev/mtd7 | cut -c1-32)
    msg "APP md5 after write: $AFTER"
    [ "$AFTER" = "$NEW_MD5" ] && { sync; msg "UPDATE_FLASH_SUCCESS"; exit 0; }
    msg "post-write checksum mismatch, restoring previous APP"
else
    msg "flash_tool failed, restoring previous APP"
fi
if $D/flash_tool $D/mtd7_before_update.bin /dev/mtd7 >> $LOG 2>&1; then
    msg "UPDATE_RESTORED_PREVIOUS (camera still runs the old firmware)"; exit 2
fi
msg "RESTORE FAILED - KEEP CAMERA POWERED"; msg "UPDATE_FLASH_FAILED"; exit 3
