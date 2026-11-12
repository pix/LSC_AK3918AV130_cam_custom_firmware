/* Proof of concept on the camera: open AP for N seconds, then stop.
 * Usage: apmode_poc <ssid> <channel> <seconds> */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "apmode.h"
int main(int argc, char **argv)
{
    if (argc == 2 && argv[1][0] == '-') {   /* "-caps": print driver capabilities only */
        static char b[8192];
        int r = apmode_caps("wlan0", b, sizeof(b));
        printf("caps rc=%d\n%s", r, b);
        return r != 0;
    }
    if (argc < 4) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    int r = apmode_start("wlan0", argv[1], atoi(argv[2]));
    printf("POC start rc=%d (%s)\n", r, apmode_last_error());
    if (r) return 1;
    for (int i = 0; i < atoi(argv[3]); i++) {
        sleep(1);
        if (i % 10 == 0) printf("POC t=%ds stations=%d\n", i, apmode_station_count());
    }
    apmode_stop();
    printf("POC stopped (%s)\n", apmode_last_error());
    return 0;
}
