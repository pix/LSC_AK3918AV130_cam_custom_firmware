#!/bin/sh
# Host test for tools/maint/flash_image.sh with a fake MTD device. Usage: sh tests/flash/run_flash_tests.sh
set -u
W=$(cd "$(dirname "$0")/../.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; cd "$T"; mkdir maint
for n in A B X; do head -c 5242880 /dev/urandom > $n.img; cp $n.img maint/; done
head -c 1000 /dev/urandom > maint/small.img
cp "$W/tools/maint/flash_image.sh" maint/
printf '%s known-A\n%s known-B\n' "$(md5sum A.img | cut -c1-32)" "$(md5sum B.img | cut -c1-32)" > maint/KNOWN_IMAGES
cat > maint/flash_tool <<'F'
#!/bin/sh
if [ -f "${AKR_FAIL_ONCE:-/nonexistent}" ]; then rm -f "$AKR_FAIL_ONCE"; head -c 4096 /dev/zero | dd of="$2" conv=notrunc 2>/dev/null; exit 1; fi
cp "$1" "$2"
F
chmod +x maint/flash_tool
printf 'mtd6: 00040000 00001000 "CONFIG"\nmtd7: 00500000 00001000 "APP"\n' > proc_mtd
cp A.img mtd7
run() { AKR_MAINT=$T/maint AKR_MTD=$T/mtd7 AKR_PROC_MTD=$T/proc_mtd sh maint/flash_image.sh "$@" 2>&1 | tail -1; }
nm() { grep "^$(md5sum mtd7 | cut -c1-32) " maint/KNOWN_IMAGES | cut -d' ' -f2 || echo OTHER; }
fails=0
t() { got=$1; want=$2; state=$3; wstate=$4; if [ "$got" = "$want" ] && [ "$state" = "$wstate" ]; then echo "PASS $5"; else echo "FAIL $5 (got $got/$state)"; fails=$((fails+1)); fi; }
A=$(md5sum A.img | cut -c1-32); B=$(md5sum B.img | cut -c1-32)
t "$(run B.img)" FLASH_OK "$(nm)" known-B "update A->B"
t "$(run B.img)" FLASH_ALREADY "$(nm)" known-B "already flashed"
t "$(run X.img)" FLASH_FAILED "$(nm)" known-B "unknown image refused"
t "$(run small.img)" FLASH_FAILED "$(nm)" known-B "wrong size refused"
t "$(run nope.img)" FLASH_FAILED "$(nm)" known-B "missing image refused"
cp X.img mtd7; t "$(run A.img)" FLASH_FAILED "$(md5sum mtd7 | cut -c1-32)" "$(md5sum X.img | cut -c1-32)" "unknown current refused"; cp B.img mtd7
printf 'mtd7: 00400000 00001000 "APP"\n' > bad; t "$(AKR_MAINT=$T/maint AKR_MTD=$T/mtd7 AKR_PROC_MTD=$T/bad sh maint/flash_image.sh A.img 2>&1 | tail -1)" FLASH_FAILED "$(nm)" known-B "wrong layout refused"
touch f1; t "$(AKR_FAIL_ONCE=$T/f1 run A.img)" FLASH_RESTORED_PREVIOUS "$(nm)" known-B "write failure auto-restores"
t "$(run A.img)" FLASH_OK "$(nm)" known-A "update B->A"
t "$(run mtd7_before_$B.bin)" FLASH_OK "$(nm)" known-B "rollback via backup"
t "$(run mtd7_before_$A.bin)" FLASH_OK "$(nm)" known-A "rollback back"
[ $fails -eq 0 ] && echo "ALL PASS" || { echo "$fails FAILED"; exit 1; }
