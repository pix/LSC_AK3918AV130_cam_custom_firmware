/* Host test: mux camera Annex-B frames (dir of NNNN.h264) + a 440 Hz tone
 * into fragmented MP4 like the recorder does. Usage: test_fmp4 framesdir out.mp4 */
#include "fmp4.h"
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int is_key(const unsigned char *d, size_t n)
{ for (size_t i = 0; i + 4 < n; i++) if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1 && ((d[i+3] & 31) == 5 || (d[i+3] & 31) == 7)) return 1; return 0; }
int main(int argc, char **argv)
{
    char *names[2048]; int n = 0;
    DIR *dd = opendir(argv[1]); struct dirent *e;
    while (dd && (e = readdir(dd)) && n < 2048) if (strstr(e->d_name, ".h264")) names[n++] = strdup(e->d_name);
    qsort(names, n, sizeof(char *), cmp);
    FILE *out = fopen(argv[2], "wb");
    static unsigned char buf[600000];
    fmp4_t *m = NULL; uint64_t ts = 100000; double ph = 0; uint64_t t8 = 0; int frames = 0;
    for (int i = 0; i < n; i++) {
        char p[512]; snprintf(p, sizeof p, "%s/%s", argv[1], names[i]);
        FILE *f = fopen(p, "rb"); size_t len = fread(buf, 1, sizeof buf, f); fclose(f);
        int key = is_key(buf, len);
        if (!m) { if (!key) continue; m = fmp4_open(out, buf, len, 1); if (!m) { puts("no SPS/PPS"); return 1; } }
        if (key && frames) fmp4_flush(m);
        fmp4_video(m, buf, len, key, ts); frames++;
        int16_t pcm[533];                                  /* 66.7 ms of audio per frame */
        for (int k = 0; k < 533; k++) { pcm[k] = (int16_t)(8000 * sin(ph)); ph += 2 * M_PI * 440 / 8000; }
        fmp4_audio(m, pcm, 533, t8); t8 += 533;
        ts += 67;
    }
    fmp4_close(m); fclose(out);
    printf("%d frames muxed\n", frames);
    return 0;
}
