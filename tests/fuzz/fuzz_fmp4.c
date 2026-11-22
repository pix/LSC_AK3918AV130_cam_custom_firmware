/* Fuzz harness: fmp4.c fragmented-MP4 muxer (SD recordings, web live view)
 * and record.c's recording name / day validation and listings.
 *
 * Input, byte 0 = mode:
 *   even: muxer. byte 1 = flags (bit0 audio track). Then records
 *         [kind u8][ts delta s16 LE][len u16 LE][len bytes]
 *         kind%5: 0 video key (flush before, as record.c does), 1 video P,
 *         2 audio (len/2 samples, ts = 8 kHz delta), 3 flush, 4 big time jump.
 *         The first record's bytes are the keyframe passed to fmp4_open().
 *   odd:  record.c: rest = "day\0name\0" for record_path(), record_list_json(),
 *         record_is_live(). */
#include "record.c"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void mk(const char *p) { mkdir(p, 0755); }
static void touch(const char *p) { FILE *f = fopen(p, "w"); if (f) { fputs("x", f); fclose(f); } }

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    mk(REC_ROOT); mk(REC_DIR);
    mk(REC_DIR "/2026-01-01"); mk(REC_DIR "/2026-01-02"); mk(REC_DIR "/2026-1-1");
    touch(REC_DIR "/2026-01-01/10-00-00_motion.mp4");
    touch(REC_DIR "/2026-01-01/10-00-00_motion.jpg");
    touch(REC_DIR "/2026-01-01/11-22-33_cont.mp4");
    touch(REC_DIR "/2026-01-02/00-00-00_timelapse.mp4");
    touch(REC_DIR "/2026-01-02/..bad.mp4");
    return 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void fuzz_record(const uint8_t *d, size_t n)
{
    char *s = malloc(n + 2);
    memcpy(s, d, n); s[n] = 0; s[n + 1] = 0;
    const char *day = s;
    const char *name = s + strlen(s) + 1;
    if ((size_t)(name - s) > n) name = s + n + 1;
    char out[300];
    record_path(day, name, out, sizeof out);
    record_path(day, name, out, 8);
    static char big[65536];
    record_list_json(day, big, sizeof big);
    record_list_json(day, big, 64);
    record_list_json(NULL, big, 40);
    record_is_live(day, name);
    /* a recording in progress: list must mark it */
    snprintf(g_cur, sizeof g_cur, "%.10s/%.80s", day, name);
    record_list_json(day, big, sizeof big);
    record_is_live(day, name);
    g_cur[0] = 0;
    free(s);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 2) return 0;
    if (d[0] & 1) { fuzz_record(d + 1, n - 1); return 0; }
    int audio = d[1] & 1;
    const uint8_t *p = d + 2, *end = d + n;
    char *mem = NULL; size_t memlen = 0;
    FILE *f = open_memstream(&mem, &memlen);
    if (!f) return 0;
    fmp4_t *m = NULL;
    uint64_t ts = 1000000, t8 = 0;
    while (end - p >= 5) {
        int kind = p[0] % 5;
        int16_t dt = (int16_t)le16(p + 1);
        size_t len = le16(p + 3);
        p += 5;
        if (len > (size_t)(end - p)) len = (size_t)(end - p);
        const uint8_t *b = p;
        p += len;
        if (!m) {
            m = fmp4_open(f, b, len, audio);
            if (!m) break;
            continue;
        }
        switch (kind) {
        case 0: ts += (uint64_t)(int64_t)dt; fmp4_flush(m); fmp4_video(m, b, len, 1, ts); break;
        case 1: ts += (uint64_t)(int64_t)dt; fmp4_video(m, b, len, 0, ts); break;
        case 2: {
            size_t ns = len / 2;
            int16_t *pcm = malloc(ns ? ns * 2 : 1);
            memcpy(pcm, b, ns * 2);
            t8 += (uint64_t)(int64_t)dt;
            fmp4_audio(m, pcm, ns, t8);
            free(pcm);
            break;
        }
        case 3: fmp4_flush(m); break;
        case 4: ts += (uint64_t)dt << 24; t8 += (uint64_t)dt << 20; fmp4_video(m, b, len, 0, ts); break;
        }
    }
    if (m) { (void)fmp4_video_start_ms(m); fmp4_close(m); }
    fclose(f);
    free(mem);
    return 0;
}
