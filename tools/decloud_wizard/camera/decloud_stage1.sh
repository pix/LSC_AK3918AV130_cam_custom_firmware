#!/bin/sh
# Runs on the camera from the SD-card boot hook.
# 1) Verifies the exact 3215672.2 flash layout.
# 2) Creates a pristine raw backup of all 8 MTD partitions on the SD card.
# 3) Waits for toolkit Wi-Fi and saves it to persistent CONFIG (mtd6).

SD=/tmp/sd
LOG="$SD/logs/decloud_stage1.log"
BACKUP="$SD/original_firmware"
SUCCESS="$SD/STAGE1_SUCCESS"
STATUS="$SD/STAGE1_STATUS"

mkdir -p "$SD/logs" "$BACKUP"
exec >>"$LOG" 2>&1

echo "=== LSC decloud stage 1: $(date) ==="

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

if ! check_layout; then
    echo "ERROR: flash partition layout does not match 3215672.2."
    cat /proc/mtd
    echo "UNSUPPORTED=1" > "$STATUS"
    exit 1
fi

echo "Hardware partition layout verified."

# First make a pristine stock backup, before writing persistent Wi-Fi into mtd6.
echo "Creating pristine raw MTD backup..."
dd if=/dev/mtdblock0 of="$BACKUP/mtdblock0.bin" bs=64k
dd if=/dev/mtdblock1 of="$BACKUP/mtdblock1.bin" bs=64k
dd if=/dev/mtdblock2 of="$BACKUP/mtdblock2.bin" bs=64k
dd if=/dev/mtdblock3 of="$BACKUP/mtdblock3.bin" bs=64k
dd if=/dev/mtdblock4 of="$BACKUP/mtdblock4.bin" bs=64k
dd if=/dev/mtdblock5 of="$BACKUP/mtdblock5.bin" bs=64k
dd if=/dev/mtdblock6 of="$BACKUP/mtdblock6.bin" bs=64k
dd if=/dev/mtdblock7 of="$BACKUP/mtdblock7.bin" bs=64k
sync

check_size() {
    got=$(stat -c%s "$BACKUP/mtdblock$1.bin" 2>/dev/null)
    [ "$got" = "$2" ] || { echo "ERROR: mtdblock$1 size $got, expected $2"; exit 1; }
}
check_size 0 204800
check_size 1 4096
check_size 2 4096
check_size 3 65536
check_size 4 1572864
check_size 5 1032192
check_size 6 262144
check_size 7 5242880
(cd "$BACKUP" && md5sum mtdblock*.bin > md5sums.txt)
echo "Pristine stock backup verified."

# Wait for wifi_apply.sh to establish the station connection.
i=0
while [ "$i" -lt 90 ]; do
    if wpa_cli -i wlan0 status 2>/dev/null | grep -q '^wpa_state=COMPLETED$'; then
        echo "Wi-Fi association complete."
        break
    fi
    sleep 1
    i=$((i+1))
done

if [ "$i" -ge 90 ]; then
    echo "ERROR: Wi-Fi did not connect within 90 seconds."
    exit 1
fi

# station_connect.sh adds the network dynamically. Ask wpa_supplicant to save
# that live network into the /tmp file, then persist it on the CONFIG JFFS2.
echo "Saving live Wi-Fi configuration..."
wpa_cli -i wlan0 save_config >/tmp/decloud_wpa_save.out 2>&1 || true
sleep 1
if ! grep -q '^network={' /tmp/wpa_supplicant.conf 2>/dev/null; then
    echo "ERROR: /tmp/wpa_supplicant.conf does not contain a saved network."
    cat /tmp/decloud_wpa_save.out 2>/dev/null || true
    exit 1
fi

if ! mount | grep -q '/dev/mtdblock6 on /etc/config type jffs2 (rw'; then
    echo "ERROR: /etc/config is not the expected writable JFFS2 CONFIG partition."
    mount
    exit 1
fi

cp /tmp/wpa_supplicant.conf /etc/config/wpa_supplicant.conf
chmod 600 /etc/config/wpa_supplicant.conf
sync
if ! grep -q '^network={' /etc/config/wpa_supplicant.conf; then
    echo "ERROR: persistent Wi-Fi verification failed."
    exit 1
fi
echo "Persistent Wi-Fi configuration saved."

MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null || true)
# wpa_state=COMPLETED only means associated: the DHCP lease can come a few
# seconds later. Wait for it, or IP= stays empty and the wizard cannot
# propose the camera's address in stage 2 (change in this copy).
IP=""
i=0
while [ "$i" -lt 30 ]; do
    IP=$(ifconfig wlan0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p' | head -n1)
    if [ -z "$IP" ]; then
        IP=$(ifconfig wlan0 2>/dev/null | awk '/inet / {print $2; exit}')
    fi
    [ -n "$IP" ] && break
    sleep 1
    i=$((i+1))
done
if [ -n "$IP" ]; then
    echo "Wi-Fi address: $IP"
else
    echo "WARNING: no IP address after 30 seconds; the wizard will ask for it."
fi
{
    echo "MODEL=3215672.2"
    echo "MAC=$MAC"
    echo "IP=$IP"
    echo "MTD_LAYOUT=verified"
    echo "WIFI_PERSISTED=yes"
    echo "BACKUP=verified_pristine"
} > "$STATUS"

echo "stage1 complete" > "$SUCCESS"
sync
echo "STAGE1 SUCCESS"
