/* Host browser test: web server + live module, frames replayed from files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "auth.h"
#include "control.h"
#include "web.h"
#include "live.h"
#include "speaker.h"
#include "osd.h"
#include <math.h>
#include <pthread.h>
static void *tone(void *a) { (void)a; static short pcm[256]; double ph = 0;
    for (;;) { for (int i = 0; i < 256; i++) { pcm[i] = (short)(8000 * sin(ph)); ph += 2 * M_PI * 440 / 8000; } live_push_audio(pcm, 256); usleep(32000); } return NULL; }
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
int main(int argc, char **argv)
{
    control_start(); auth_init(); osd_init(); web_start();
    pthread_t tt; pthread_create(&tt, NULL, tone, NULL);
    speaker_set_ready(1);
    char *names[4096]; int n = 0;
    DIR *d = opendir(argv[1]); struct dirent *e;
    while (d && (e = readdir(d)) && n < 4096) if (strstr(e->d_name, ".h264")) names[n++] = strdup(e->d_name);
    qsort(names, n, sizeof(char *), cmp);
    printf("replaying %d frames\n", n);
    static unsigned char buf[600000]; unsigned long long ts = 1000;
    for (;;) for (int i = 0; i < n; i++) {
        char p[512]; snprintf(p, sizeof p, "%s/%s", argv[1], names[i]);
        FILE *f = fopen(p, "rb"); size_t len = f ? fread(buf, 1, sizeof buf, f) : 0; if (f) fclose(f);
        int key = 0; for (size_t k = 0; k + 4 < len; k++) if (buf[k] == 0 && buf[k+1] == 0 && buf[k+2] == 1 && (buf[k+3] & 31) == 5) { key = 1; break; }
        live_push_frame(buf, len, key, ts); ts += 66; usleep(66000);
        { static int k; size_t q = speaker_queued(); if (q && (k++ % 15) == 0) { printf("speaker_queued=%zu\n", q); fflush(stdout); }
          int16_t tmp[512]; speaker_fill(tmp, 512); }   /* drain like the DAC would */
    }
}
