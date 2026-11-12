#include "resetkey.h"
#include "auth.h"
#include "control.h"
#include "monotime.h"
#include "netcfg.h"
#include "qrwifi.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/stat.h>

#define SD_MNT "/tmp/akr_sd"

/* 32-bit ARM, kernel 4.4 ABI: struct timeval (2x32) + type/code/value.
 * Declared locally so musl's 64-bit time_t doesn't change the layout. */
struct kev {
    uint32_t sec, usec;
    uint16_t type, code;
    int32_t  value;
};
#define EV_KEY_TYPE 1

static void consume_flag(const char *name, void (*action)(void), const char *what)
{
    char path[96], done[104];
    snprintf(path, sizeof(path), SD_MNT "/%s", name);
    if (access(path, F_OK) != 0) return;
    printf("[reset] SD card file '%s' found: %s\n", name, what);
    action();
    snprintf(done, sizeof(done), "%s.done", path);
    if (rename(path, done) != 0) unlink(path);
}

static void do_settings_reset(void) { control_reset_settings(); }

void resetkey_check_sd(void)
{
    static const char *devs[] = { "/dev/mmcblk0p1", "/dev/mmcblk0" };
    mkdir(SD_MNT, 0700);
    int mounted = 0;
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]) && !mounted; i++)
        if (access(devs[i], F_OK) == 0 &&
            mount(devs[i], SD_MNT, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0)
            mounted = 1;
    if (!mounted) return;  /* no card: nothing to do */

    /* wifi.txt: apply with rollback, then keep only the SSID (password blanked). */
    FILE *wf = fopen(SD_MNT "/wifi.txt", "r");
    if (wf) {
        char text[512];
        size_t n = fread(text, 1, sizeof(text) - 1, wf);
        fclose(wf);
        text[n] = 0;
        char ssid[33] = "", psk[64] = "";
        const char *err;
        if (qrwifi_parse_file(text, ssid, psk, &err) == 0 && net_connect(ssid, psk, &err) == 0)
            printf("[reset] SD card wifi.txt: joining \"%s\"\n", ssid);
        else
            printf("[reset] SD card wifi.txt ignored: %s\n", err);
        memset(psk, 0, sizeof(psk));
        memset(text, 0, sizeof(text));
        FILE *df = fopen(SD_MNT "/wifi.txt.done", "w");
        if (df) {
            fprintf(df, "# applied by the camera; password removed for safety\nssid=%s\npassword=\n", ssid);
            fclose(df);
        }
        unlink(SD_MNT "/wifi.txt");
    }

    consume_flag("reset_auth", auth_reset, "resetting login to admin/admin");
    consume_flag("reset_settings", do_settings_reset, "deleting saved settings");

    sync();
    umount(SD_MNT);
}

static long now_ms(void)
{
    return (long)mono_ms();  /* see monotime.h */
}

static void *button_thread(void *arg)
{
    (void)arg;
    struct pollfd pf[8];
    int n = 0;
    for (int i = 0; i < 8 && n < 8; i++) {
        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/input/event%d", i);
        int fd = open(dev, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        pf[n].fd = fd;
        pf[n].events = POLLIN;
        n++;
        printf("[reset] watching %s for reset button\n", dev);
    }
    if (n == 0) {
        printf("[reset] no /dev/input/event* devices: reset button unavailable\n");
        return NULL;
    }

    long pressed_at = -1;
    int fired = 0;
    long taps[3] = { -100000, -100000, -100000 };   /* release times of short presses */
    for (;;) {
        int r = poll(pf, (nfds_t)n, 250);
        if (r < 0 && errno != EINTR) break;
        for (int i = 0; r > 0 && i < n; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            struct kev ev;
            while (read(pf[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type != EV_KEY_TYPE) continue;
                if (ev.value == 1) {          /* press */
                    pressed_at = now_ms();
                    fired = 0;
                    printf("[reset] button pressed (code %u) - hold %d s to reset login\n",
                           ev.code, RESET_HOLD_S);
                } else if (ev.value == 0) {   /* release */
                    long now = now_ms();
                    if (pressed_at >= 0 && now - pressed_at < 1000) {   /* short press */
                        taps[0] = taps[1]; taps[1] = taps[2]; taps[2] = now;
                        if (taps[2] - taps[0] <= 3000) {
                            printf("[reset] triple press -> looking for a Wi-Fi QR code for 3 min\n");
                            qrwifi_start(180, "reset button pressed 3 times");
                            taps[0] = taps[1] = taps[2] = -100000;
                        }
                    }
                    pressed_at = -1;
                }
            }
        }
        if (pressed_at >= 0 && !fired && now_ms() - pressed_at >= RESET_HOLD_S * 1000L) {
            fired = 1;
            printf("[reset] button held %d s\n", RESET_HOLD_S);
            auth_reset();
        }
    }
    for (int i = 0; i < n; i++) close(pf[i].fd);
    return NULL;
}

int resetkey_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, button_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
