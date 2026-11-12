#!/bin/sh
# Guarded APP (mtd7) write of ANY known image — used both for updates and
# for rollback:   sh flash_image.sh <image>   (path relative to maint/ or absolute)
#
# Refuses unless: flash layout is the known one; the image is exactly
# 5 MiB and its md5 is listed in maint/KNOWN_IMAGES; the CURRENT APP md5 is
# also listed (so we only ever move between known-good states). Backs up
# the current APP first (mtd7_before_<md5>.bin) and restores it if the
# write or the post-write check fails. Prints one final status line:
#   FLASH_OK | FLASH_ALREADY | FLASH_RESTORED_PREVIOUS | FLASH_FAILED
D=${AKR_MAINT:-/tmp/sd/maint}
MTD=${AKR_MTD:-/dev/mtd7}
PROC_MTD=${AKR_PROC_MTD:-/proc/mtd}
KNOWN=$D/KNOWN_IMAGES
SIZE=5242880
LOG=$D/flash_image.log
msg()  { echo "$*"; echo "$*" >> $LOG; }
fail() { msg "ERROR: $*"; msg "FLASH_FAILED"; exit 1; }
known() { grep -q "^$1 " $KNOWN; }

IMG=$1
[ -n "$IMG" ] || fail "usage: flash_image.sh <image>"
case "$IMG" in /*) ;; *) IMG=$D/$IMG ;; esac

msg "=== flash_image $(date) : $IMG ==="
grep -q '^mtd7: 00500000 00001000 "APP"$' $PROC_MTD    || fail "unexpected mtd7 layout"
grep -q '^mtd6: 00040000 00001000 "CONFIG"$' $PROC_MTD || fail "unexpected mtd6 layout"
[ -f $KNOWN ] || fail "KNOWN_IMAGES list missing"
[ -x $D/flash_tool ] || chmod +x $D/flash_tool 2>/dev/null
[ -x $D/flash_tool ] || fail "flash_tool missing"
[ "$(stat -c%s "$IMG" 2>/dev/null)" = "$SIZE" ] || fail "image missing or size wrong"
NEW=$(md5sum "$IMG" | cut -c1-32)
known $NEW || fail "image md5 $NEW is not in KNOWN_IMAGES"
msg "image: $NEW ($(grep "^$NEW " $KNOWN | cut -d' ' -f2-))"

# Raw MTD char device: no page cache, and never drop caches (programs run
# from this partition while it is rewritten).
CUR=$(md5sum $MTD | cut -c1-32)
msg "current APP: $CUR ($(grep "^$CUR " $KNOWN | cut -d' ' -f2-))"
[ "$CUR" = "$NEW" ] && { msg "FLASH_ALREADY"; exit 0; }
known $CUR || fail "current APP is not a known image; refusing"

BK=$D/mtd7_before_$CUR.bin
if [ "$(md5sum $BK 2>/dev/null | cut -c1-32)" != "$CUR" ]; then
    dd if=$MTD of=$BK bs=64k 2>/dev/null; sync
    [ "$(md5sum $BK | cut -c1-32)" = "$CUR" ] || fail "backup of current APP failed"
fi
msg "current APP backed up: $BK"

msg "writing..."
if $D/flash_tool "$IMG" $MTD >> $LOG 2>&1; then
    AFTER=$(md5sum $MTD | cut -c1-32)
    msg "APP after write: $AFTER"
    [ "$AFTER" = "$NEW" ] && { sync; msg "FLASH_OK"; exit 0; }
    msg "post-write mismatch, restoring previous APP"
else
    msg "flash_tool failed, restoring previous APP"
fi
if $D/flash_tool $BK $MTD >> $LOG 2>&1 && [ "$(md5sum $MTD | cut -c1-32)" = "$CUR" ]; then
    msg "FLASH_RESTORED_PREVIOUS"; exit 2
fi
msg "RESTORE FAILED - KEEP CAMERA POWERED, use UART recovery"; msg "FLASH_FAILED"; exit 3
