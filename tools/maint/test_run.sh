#!/bin/sh
# Run the NEW ak_rtsp from the SD card WITHOUT flashing anything.
# Stops the flashed ak_rtsp (and its respawning supervisor), feeds the
# hardware watchdog ourselves, reloads the encoder/audio modules (same
# sequence as upstream tools/flash_scripts/start_rtsp.sh) and starts
# /tmp/sd/maint/ak_rtsp in the background. A reboot returns to the flashed one.
D=/tmp/sd/maint
LOG=$D/test_app.log

killall -9 watchdog           2>/dev/null
killall -9 ak_rtsp_wrapper.sh 2>/dev/null
killall -9 ak_rtsp_wrapper    2>/dev/null
killall -9 anyka_ipc          2>/dev/null
killall -9 ak_rtsp            2>/dev/null
sleep 2

WD_PIDFILE=/tmp/watchdog_feeder.pid
if [ -f "$WD_PIDFILE" ]; then
    kill -9 "$(cat "$WD_PIDFILE")" 2>/dev/null
    rm -f "$WD_PIDFILE"
fi
# setsid: detach from the telnet session, otherwise its SIGHUP on logout
# kills the feeder (-> hardware watchdog reset) and ak_rtsp.
setsid sh -c 'while true; do printf V; sleep 5; done > /dev/watchdog' </dev/null >/dev/null 2>&1 &
echo $! > "$WD_PIDFILE"

rmmod ak_venc_bridge  2>/dev/null
rmmod ak_venc_adapter 2>/dev/null
insmod /usr/modules/ak_venc_adapter.ko || { echo "TEST_RUN_FAILED insmod ak_venc_adapter"; exit 1; }
insmod /usr/modules/ak_venc_bridge.ko  || { echo "TEST_RUN_FAILED insmod ak_venc_bridge"; exit 1; }
rmmod ak_pcm 2>/dev/null
insmod /usr/modules/ak_pcm.ko 2>/dev/null
rm -f /tmp/ircut_has_reset_flag

chmod +x $D/ak_rtsp
: > $LOG
setsid $D/ak_rtsp </dev/null >> $LOG 2>&1 &
echo $! > /tmp/ak_rtsp_test.pid
sleep 8
if kill -0 "$(cat /tmp/ak_rtsp_test.pid)" 2>/dev/null; then
    echo "TEST_RUN_STARTED pid $(cat /tmp/ak_rtsp_test.pid)"
else
    echo "TEST_RUN_FAILED (process exited) - last log lines:"
    tail -n 30 $LOG
fi
