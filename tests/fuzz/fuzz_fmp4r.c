/* libFuzzer harness: the fMP4 demuxer for ONVIF replay (fmp4r.c) and, with
 * flag bit 0, a whole replay session over the fuzzed file (replay.c: PLAY
 * header parsing, seeking, the day playlist, H.264 / G.711 RTP packetizer with
 * the ONVIF extension, RTCP). SD card files can be replaced by a user, so the
 * demuxer must survive any bytes.
 *
 * Input: byte 0 = flags
 *   bit0: replay session (else: demux only, from memory through a memfd)
 *   bit1: interleaved TCP (else UDP datagrams) for the session
 *   bit2: day token (playlist: the fuzzed file + a valid one), else the file token
 * replay mode: then five NUL-terminated header values (Range, Scale, Frames,
 * Rate-Control, Require), then the file. demux mode: the file follows byte 0.
 * Seeds: FZ_WRITE_SEED=<dir> writes valid recordings (made by fmp4.c) as
 * inputs for both modes and exits. Time and sleeping are faked in replay.c
 * (mono_us / usleep macros) so pacing does not slow the fuzzer down. */
#define _GNU_SOURCE                 /* memfd_create */
#define mono_us fz_mono_us
#define usleep fz_usleep
#include "replay.c"
#undef mono_us
#undef usleep
#include "pg_fixture.h"
#include "control.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>

static int64_t g_fake_us = 1000000;
int64_t fz_mono_us(void) { return g_fake_us += 20000; }
int fz_usleep(useconds_t u) { g_fake_us += u; return 0; }

#define DAY REC_DIR "/2026-01-01"
#define FUZZ_FILE DAY "/10-00-00_cont.mp4"

static void write_file(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

static void seed(const char *dir, const char *name, const uint8_t *pre, size_t pl, const char *src)
{
    FILE *in = fopen(src, "rb");
    if (!in) return;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, in);
    fclose(in);
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite(pre, 1, pl, f);
    fwrite(buf, 1, n, f);
    fclose(f);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    mkdir(REC_ROOT, 0755); mkdir(REC_DIR, 0755); mkdir(DAY, 0755);
    pg_make_mp4(DAY "/10-30-00_motion.mp4", 40, 15, 1);         /* second file of the day playlist */
    const char *dir = getenv("FZ_WRITE_SEED");
    if (dir) {
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/.a.mp4", dir); snprintf(b, sizeof b, "%s/.b.mp4", dir);
        pg_make_mp4(a, 45, 15, 1);
        pg_make_mp4(b, 20, 10, 0);
        static const uint8_t demux[] = { 0 };
        static const uint8_t play[] = "\x01npt=0-\0\0\0no\0onvif-replay";
        static const uint8_t clock_[] = "\x07" "clock=20260101T100001Z-20260101T100002Z\0" "1.0\0\0\0onvif-replay";
        static const uint8_t intra[] = "\x03npt=0.5-2\0" "4\0intra\0\0";
        seed(dir, "demux_audio", demux, 1, a);
        seed(dir, "demux_video", demux, 1, b);
        seed(dir, "play_udp", play, sizeof play, a);
        seed(dir, "play_clock_day_tcp", clock_, sizeof clock_, a);
        seed(dir, "play_intra_tcp", intra, sizeof intra, b);
        remove(a); remove(b);
        exit(0);
    }
    signal(SIGPIPE, SIG_IGN);
    onvif_g_init();
    const char *e;
    control_set_value("onvif.replay", "on", &e);
    return 0;
}

static void demux(const uint8_t *d, size_t n)
{
    int fd = memfd_create("fz", MFD_CLOEXEC);
    if (fd < 0) return;
    if (n && write(fd, d, n) != (ssize_t)n) { close(fd); return; }
    fmp4r_t r;
    if (fmp4r_open_fd(&r, fd) != 0) return;            /* closes fd */
    static uint8_t buf[1 << 16];
    for (int frags = 0; frags < 64 && fmp4r_next(&r) == 1; frags++) {
        for (size_t i = 0; i < r.ns && i < 64; i++) (void)fmp4r_read(&r, &r.s[i], buf, sizeof buf);
        (void)fmp4r_more(&r);
    }
    fmp4r_seek(&r, 0);
    (void)fmp4r_next(&r);
    fmp4r_seek(&r, 1500000);
    (void)fmp4r_next(&r);
    fmp4r_seek(&r, INT64_MAX / 4);
    (void)fmp4r_next(&r);
    (void)fmp4r_scan_end(&r);
    (void)fmp4r_more(&r);
    fmp4r_close(&r);
}

static void steps(replay_t *r, conn_t *cn, int max)
{
    for (int i = 0; i < max; i++) if (replay_step(r, cn)) break;
}

static void session(const uint8_t *d, size_t n, uint8_t flags)
{
    const char *h[5];
    char *s = malloc(n + 6);
    memcpy(s, d, n);
    memset(s + n, 0, 6);
    size_t o = 0;
    for (int i = 0; i < 5; i++) { h[i] = s + o; o += strlen(s + o) + 1; if (o > n) o = n; }
    write_file(FUZZ_FILE, d + o, n - o);
    char sdp[2048];
    int st;
    const char *why;
    replay_t *r = replay_open(flags & 4 ? "rtsp://h/replay/2026-01-01" : "rtsp://h/replay/2026-01-01_10-00-00_cont/",
                              sdp, sizeof sdp, &st, &why);
    if (!r) { free(s); return; }
    int tcp = (flags & 2) != 0, sv[2];
    if (socketpair(AF_UNIX, (tcp ? SOCK_STREAM : SOCK_DGRAM) | SOCK_CLOEXEC, 0, sv)) abort();
    int big = 4 << 20;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    if (!tcp) fcntl(sv[0], F_SETFL, O_NONBLOCK);      /* datagrams: dropped when full, as on a network */
    shutdown(sv[1], SHUT_WR);
    conn_t *cn = tcp ? conn_plain(sv[0]) : NULL;
    replay_set_track(r, 0, tcp, 0, 1, sv[0], sv[0]);
    replay_set_track(r, 1, tcp, 2, 3, sv[0], sv[0]);
    char ro[96], info[640], so[16];
    if (replay_play(r, h[0], h[1], h[2], h[3], h[4], 5, "rtsp://h/replay/x/", ro, sizeof ro, info, sizeof info, so, sizeof so, &why) == 200)
        steps(r, cn, 400);
    replay_pause(r);
    if (replay_play(r, NULL, NULL, NULL, "no", "onvif-replay", 6, "u", ro, sizeof ro, info, sizeof info, so, sizeof so, &why) == 200)
        steps(r, cn, 50);
    if (replay_play(r, "npt=1.5-", "1", NULL, "no", NULL, 7, "u", ro, sizeof ro, info, sizeof info, so, sizeof so, &why) == 200)
        steps(r, cn, 50);
    replay_close(r);
    if (cn) conn_close(cn); else close(sv[0]);
    close(sv[1]);
    free(s);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 1 || n > (1 << 20)) return 0;
    if (d[0] & 1) session(d + 1, n - 1, d[0]);
    else demux(d + 1, n - 1);
    return 0;
}
