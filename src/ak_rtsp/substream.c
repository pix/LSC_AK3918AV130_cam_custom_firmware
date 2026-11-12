/* Second H.264 stream, 640x360 15 fps, encoder channel 1 kernel-bound to VI
 * channel 1 (see docs/re/re_spec.md section 6; stock encodes this one in user
 * mode, the kernel bind is our variant).
 * Values follow stock's [sub_video_0]: VBR+, 512 kbps, gop 30, minqp 20,
 * maxqp 40, initqp 90, level 50, and *no* rate-control block (rc_flag 0)
 * with MMA/MVV off in QUERY_DMA, so the two stay consistent.
 *
 * Setting video.sub (off by default, needs a restart). Crash guard: a marker
 * is kept until 300 frames came out; a start that does not get there leaves
 * it, and the next start turns the feature off (ak_rtsp_sub_off) instead of
 * trying again. */
#include "substream.h"
#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "monotime.h"
#include "ctrl_params.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define SUB_GUARD "/etc/config/ak_rtsp_sub_guard"
#define SUB_OFF   "/etc/config/ak_rtsp_sub_off"
#define SUB_W 640
#define SUB_H 360
#define SUB_FPS 15
#define SUB_EXT 131072              /* stock: align32(max(0x20000, max_kbps*256)) */
#define SUB_MAXLEN 0x40000
#define VENC_SET_CHN_STATE 0x406056c7

static int s_enabled;
CTRL_BOOL(p_en, s_enabled)
static int g_fd = -1, g_guarded;
static uint8_t *g_virt;
static uint32_t g_kv;
static char g_note[96];
static volatile unsigned g_frames, g_keys;
static volatile int g_state;        /* 0 off, 1 running, -1 failed / skipped */

/* ---- tiny frame ring for consumers --------------------------------------- */
#define RING 48
#define RING_BYTES (512 * 1024)
static struct { uint32_t off, len; uint64_t ts; uint32_t seq; uint8_t key; } g_ring[RING];
static uint8_t g_data[RING_BYTES];
static uint32_t g_wpos, g_seq;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

static void push(const uint8_t *d, uint32_t len, int key, uint64_t ts)
{
    if (len > RING_BYTES / 4) return;
    pthread_mutex_lock(&g_lock);
    if (g_wpos + len > RING_BYTES) g_wpos = 0;
    memcpy(g_data + g_wpos, d, len);
    uint32_t i = g_seq % RING;
    g_ring[i].off = g_wpos; g_ring[i].len = len; g_ring[i].ts = ts; g_ring[i].key = (uint8_t)key; g_ring[i].seq = g_seq;
    g_wpos += len;
    g_seq++;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

/* data of a ring slot may be overwritten once RING_BYTES newer bytes were
 * written; consumers copy under the lock and skip ahead when they lag. */
int substream_next(uint32_t *cursor, uint8_t *buf, size_t cap, int *key, uint64_t *ts, int timeout_ms)
{
    pthread_mutex_lock(&g_lock);
    if (*cursor == 0 || g_seq - *cursor > RING - 4) {   /* new or lagging: start at the newest keyframe (gop 30 < RING) */
        uint32_t s = g_seq;
        *cursor = s;
        for (uint32_t k = 0; k < RING && k < g_seq; k++) if (g_ring[(s - 1 - k) % RING].key) { *cursor = s - 1 - k; break; }
    }
    if (*cursor >= g_seq) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += (long)(timeout_ms % 1000) * 1000000L; t.tv_sec += timeout_ms / 1000 + t.tv_nsec / 1000000000L; t.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&g_cond, &g_lock, &t);
        if (*cursor >= g_seq) { pthread_mutex_unlock(&g_lock); return 0; }
    }
    uint32_t i = *cursor % RING;
    int n = 0;
    if (g_ring[i].seq == *cursor && g_ring[i].len <= cap) {
        memcpy(buf, g_data + g_ring[i].off, g_ring[i].len);
        n = (int)g_ring[i].len; *key = g_ring[i].key; *ts = g_ring[i].ts;
    }
    (*cursor)++;
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* ---- encoder --------------------------------------------------------------- */
static int bring_up(void)
{
    g_fd = open("/dev/venc-chn1", O_RDWR | O_NONBLOCK);
    if (g_fd < 0) { snprintf(g_note, sizeof g_note, "no encoder channel 1 (%s)", strerror(errno)); return -1; }
    if (ioctl(g_fd, VENC_SET_CHN_STATE, 1) < 0) perror("[sub] SET_CHN_STATE (non-fatal)");

    uint32_t q[21];
    memset(q, 0, sizeof q);
    q[0] = H264_PROFILE_IDC_MAIN; q[1] = 1;
    ((uint16_t *)q)[4] = SUB_W; ((uint16_t *)q)[5] = SUB_H;
    ((uint16_t *)q)[6] = 1;                                  /* stock */
    ((uint8_t *)q)[28] = 1;                                  /* MMA / MVV stay 0 */
    if (ioctl(g_venc_fd, VENC_IOCTL_QUERY_DMA, q) < 0 || q[0] == 0 || q[0] > 20) { snprintf(g_note, sizeof g_note, "QUERY_DMA failed"); return -1; }
    uint32_t ring = 0;
    for (uint32_t i = 0; i < q[0]; i++) ring += (q[1 + i] + 31u) & ~31u;
    printf("[sub] QUERY_DMA: %u blocks, ring %u bytes\n", q[0], ring);

    struct venc_dma_pool_desc pool;
    memset(&pool, 0, sizeof pool);
    pool.create = 1; pool.is_external = 0; pool.pool_size = ring;
    if (ioctl(g_fd, VENC_IOCTL_CREATE_POOL, &pool) < 0) { snprintf(g_note, sizeof g_note, "CREATE_POOL ring: %s", strerror(errno)); return -1; }
    memset(&pool, 0, sizeof pool);
    pool.create = 1; pool.is_external = 1; pool.pool_size = SUB_EXT;
    if (ioctl(g_fd, VENC_IOCTL_CREATE_POOL, &pool) < 0) { snprintf(g_note, sizeof g_note, "CREATE_POOL ext: %s", strerror(errno)); return -1; }
    g_kv = pool.phys_base;
    g_virt = mmap(NULL, SUB_EXT, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_virt == MAP_FAILED) { g_virt = NULL; snprintf(g_note, sizeof g_note, "mmap: %s", strerror(errno)); return -1; }

    struct venc_create_enc_req r;
    memset(&r, 0, sizeof r);
    r.chn_id = 1;
    r.enc.codec_type_code = H264_PROFILE_IDC_MAIN;
    r.enc.chroma_mode = 1;
    r.enc.width = SUB_W; r.enc.height = SUB_H;
    r.enc.rc_mode = VENC_RC_VBRP;
    r.enc.fps = SUB_FPS; r.enc.goplen = 30; r.enc.max_fps = 0;
    r.enc.enc_level = 50;
    r.enc.qp_or_kbps = 20; r.enc.qp2 = 40;                    /* VBR+: minqp / maxqp */
    r.enc.max_kbps_vbrp = 512; r.enc.initqp_vbrp = 90;
    r.smart.smart_mode = 0; r.smart.smart_goplen = 100; r.smart.smart_quality = 50;
    r.rc_flag = 0;                                            /* stock sub: no rc block */
    printf("[sub] CREATE_ENC 640x360...\n");
    if (ioctl(g_fd, VENC_IOCTL_CREATE_ENC, &r) < 0) { snprintf(g_note, sizeof g_note, "CREATE_ENC: %s", strerror(errno)); return -1; }

    struct venc_bind_vi_req b;
    memset(&b, 0, sizeof b);
    b.venc_chn_id = 1; b.vi_chn_packed = 1; b.param0 = SUB_FPS; b.param1 = 200; b.encode_mode = 1;
    if (ioctl(g_fd, VENC_IOCTL_BIND_VI_CHN, &b) < 0) { snprintf(g_note, sizeof g_note, "BIND_VI_CHN: %s", strerror(errno)); return -1; }
    uint32_t chn = 1;
    if (ioctl(g_fd, VENC_IOCTL_ACTIVATE, &chn) < 0) { snprintf(g_note, sizeof g_note, "ACTIVATE: %s", strerror(errno)); return -1; }
    printf("[sub] encoder channel 1 running\n");
    return 0;
}

static void *thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct venc_stream_req q;
        memset(&q, 0, sizeof q);
        q.chn_id = 1; q.len = SUB_MAXLEN;
        if (ioctl(g_fd, VENC_IOCTL_GET_STREAM, &q) != 0) { usleep(15000); continue; }
        uint32_t off = (uint32_t)q.frame_offset - g_kv;
        if (off < SUB_EXT && (uint32_t)q.len <= SUB_EXT - off) {
            const uint8_t *d = g_virt + off;
            int key = q.frame_type_raw == FRAME_TYPE_I;
            uint64_t ts = (uint64_t)(uint32_t)q.ts_hi << 32 | (uint32_t)q.ts_lo;
            push(d, (uint32_t)q.len, key, ts);
            g_frames++; g_keys += key;
            if (g_guarded && g_frames >= 300) { unlink(SUB_GUARD); sync(); g_guarded = 0; printf("[sub] start confirmed (%u frames)\n", g_frames); }
        }
        ioctl(g_fd, VENC_IOCTL_RELEASE_STREAM, &q);
    }
    return NULL;
}

/* switching the stream on again forgets a crash-guard skip (retried at the next start) */
static int en_setx(const char *v, const char **e)
{
    int r = p_en_set(v, e);
    if (!r && s_enabled && unlink(SUB_OFF) == 0) { sync(); printf("[sub] will be retried at the next start\n"); }
    return r;
}
void substream_init(void)
{
    ctrl_param_t c = { "video.sub", p_en_get, en_setx, CTRL_PERSIST };
    control_register(&c);
}

void substream_start(void)
{
    if (!s_enabled) return;
    if (access(SUB_OFF, F_OK) == 0) { g_state = -1; snprintf(g_note, sizeof g_note, "turned off after a failed start (delete %s to retry)", SUB_OFF); return; }
    if (access(SUB_GUARD, F_OK) == 0) {
        rename(SUB_GUARD, SUB_OFF); sync();
        g_state = -1; snprintf(g_note, sizeof g_note, "the previous start with it did not complete; turned off");
        fprintf(stderr, "[sub] %s\n", g_note);
        return;
    }
    int gfd = open(SUB_GUARD, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (gfd >= 0) { fsync(gfd); close(gfd); sync(); }
    g_guarded = 1;
    if (bring_up() != 0) {
        fprintf(stderr, "[sub] %s\n", g_note);
        g_state = -1;
        unlink(SUB_GUARD); sync(); g_guarded = 0;            /* clean failure, no crash: try again next time */
        return;
    }
    g_state = 1;
    pthread_t t;
    pthread_create(&t, NULL, thread, NULL);
    pthread_detach(t);
}

int substream_running(void) { return g_state == 1; }

int substream_status_json(char *out, size_t cap)
{
    return snprintf(out, cap, "{\"enabled\":%s,\"running\":%s,\"frames\":%u,\"keyframes\":%u,\"note\":\"%s\"}",
                    s_enabled ? "true" : "false", g_state == 1 ? "true" : "false", g_frames, g_keys, g_note);
}
