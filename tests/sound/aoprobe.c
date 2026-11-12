/* aoprobe: standalone speaker experiment for the camera (SD test mode, with
 * /tmp/sd/maint/no_playback so ak_rtsp leaves /dev/pcmC0D0p free).
 * Usage: aoprobe [source] [gain|-1] [spk 0/1] [seconds] [tone|chime]
 * Sequence = stock order: set_param, set_source, (set_gain), start, speaker. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>
struct pars { uint32_t format, rate, channels, bits, period_size, period_count, reserved; };
static void io(int fd, const char *n, unsigned long cmd, void *arg) {
    int r = ioctl(fd, cmd, arg);
    printf("%-22s -> %s\n", n, r == 0 ? "ok" : strerror(errno));
}
static void info(void) {
    FILE *f = fopen("/sys/devices/platform/soc/8000000.adc_dac/pcm/playback_info", "r"); char l[128];
    while (f && fgets(l, sizeof l, f)) if (strstr(l, "isr") || strstr(l, "flag") || strstr(l, "gain")) printf("  %s", l);
    if (f) fclose(f);
}
int main(int argc, char **argv) {
    int src = argc > 1 ? atoi(argv[1]) : 4, gain = argc > 2 ? atoi(argv[2]) : -1, spk = argc > 3 ? atoi(argv[3]) : 1;
    int secs = argc > 4 ? atoi(argv[4]) : 3; const char *snd = argc > 5 ? argv[5] : "tone";
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open("/dev/pcmC0D0p", O_WRONLY);
    if (fd < 0) { printf("open: %s (is ak_rtsp holding it? create /tmp/sd/maint/no_playback and reboot)\n", strerror(errno)); return 1; }
    struct pars p = { 0, 8000, 1, 16, 512, 16, 0 };
    io(fd, "set_param", 0x401c50e0, &p);
    int32_t v = -1; ioctl(fd, 0x80045010, &v); printf("source before: %d\n", v);
    v = src;  io(fd, "set_source", 0x40045010, &v);
    v = -1; ioctl(fd, 0x80045010, &v); printf("source after:  %d\n", v);
    v = -1; ioctl(fd, 0x80045030, &v); printf("gain before:   %d\n", v);
    if (gain >= 0) { v = gain; io(fd, "set_gain", 0x40045030, &v); v = -1; ioctl(fd, 0x80045030, &v); printf("gain after:    %d\n", v); }
    io(fd, "start_playing", 0x50e1, (void *)0);
    v = spk; io(fd, "enable_speaker", 0x40045081, &v);
    info();
    int16_t buf[256]; long n = 0, total = (long)secs * 8000;
    while (n < total) {
        for (int i = 0; i < 256; i++, n++) {
            double t = (double)n / 8000, f = 440;
            if (!strcmp(snd, "chime")) f = (n % 8000 < 2000) ? 523.25 : (n % 8000 < 4000) ? 659.25 : (n % 8000 < 6000) ? 783.99 : 1046.5;
            buf[i] = (int16_t)(sin(2 * M_PI * f * t) * 16000);
        }
        if (write(fd, buf, sizeof buf) < 0) { printf("write: %s\n", strerror(errno)); break; }
    }
    info();
    v = 0; io(fd, "speaker off", 0x40045081, &v);
    close(fd);
    return 0;
}
