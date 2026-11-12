/* NPU / SVP2 person & face detection — work in progress.
 *
 * Evidence (stock anyka_ipc, read-only disassembly):
 *   ak_npu_init      0x1dd194: open("/dev/npu", O_RDWR); ioctl(fd, 0xc0044e11, &u32)
 *   ak_svp2_create_chn 0x1dbb2c: ioctl 0xc09c4e20 with a 0x9c-byte request built
 *                     from a config block (u32 count + count*20-byte entries)
 *                     and a model descriptor {path, ...}; the model is
 *                     /usr/local/model_0x0A000000_V1.1.0.bin, attr 24 bytes
 *                     {1, 0, mode, (500-sensitivity)*2, 3, 2}.
 *   ak_svp2_process  0x1dc8c0: per frame, result = 16-byte {count, results*}
 *                     with 24-byte entries {?, left, top, right, bottom,
 *                     score, class}; labels "Body" / "Face".
 *   ak_svp2_release  0x1dcd54.
 * Only the init step is attempted so far: everything beyond it needs the
 * SVP2 request layouts verified on hardware, and a bad struct has rebooted
 * this box before (anyka driver paths). Guard: marker while initialised;
 * a boot that finds it does not touch the NPU.
 * Setting: npu.enabled (off by default). */
#include "npu.h"
#include "control.h"
#include "ctrl_params.h"
#include <pthread.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>

#define NPU_DEV     "/dev/npu"
#define NPU_INIT    0xc0044e11
#define NPU_GUARD   "/etc/config/ak_rtsp_npu_guard"

static int s_enabled;
CTRL_BOOL(p_en, s_enabled)

int npu_started(void) { return s_enabled; }

/* One guarded init attempt; npu_start() wraps it with the setting check and
 * the API exposes it for tests (POST /api/npu/test), so hardware iterations
 * do not need a reboot each time. */
int npu_test(void)
{
    if (access(NPU_GUARD, F_OK) == 0) {
        fprintf(stderr, "[npu] the previous start with it did not complete; disabled\n");
        s_enabled = 0;
        unlink(NPU_GUARD);
        return -1;
    }
    int fd = open(NPU_DEV, O_RDWR);
    if (fd < 0) {
        printf("[npu] open %s: %s\n", NPU_DEV, strerror(errno));
        return -1;
    }
    int g = open(NPU_GUARD, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (g >= 0) { fsync(g); close(g); sync(); }
    uint32_t v = 0;
    int rc = ioctl(fd, NPU_INIT, &v);
    printf("[npu] init ioctl: %s (value %u)\n", rc == 0 ? "ok" : strerror(errno), (unsigned)v);
    close(fd);
    if (g >= 0) { unlink(NPU_GUARD); sync(); }     /* the call returned: the kernel is fine */
    return rc == 0 ? 0 : -1;
}

/* SVP2 channel create, following docs/re/svp2_api.md. The 156-byte request is
 * _IOWR: whatever the driver writes back is dumped so the result-buffer
 * exchange can be identified from hardware, not guessed. */
#define NPU_CREATE_CMD 0xc09c4e20
#define NPU_MODEL_PATH "/usr/local/model_0x0A000000_V1.1.0.bin"

int npu_create(int model_type, int classify, int iou, int target)
{
    int fd = open(NPU_DEV, O_RDWR);
    if (fd < 0) { printf("[npu] open: %s\n", strerror(errno)); return -1; }
    /* init first (cheap; harmless if already done) */
    uint32_t v = 0;
    if (ioctl(fd, NPU_INIT, &v) != 0) { printf("[npu] init: %s\n", strerror(errno)); close(fd); return -1; }

    uint8_t req[156];
    memset(req, 0, sizeof req);
    *(int *)(req + 0) = 0;                      /* handle id */
    *(int *)(req + 4) = 1;                      /* model_num */
    snprintf((char *)req + 8, 128, "%s", NPU_MODEL_PATH);
    *(int *)(req + 136) = 0;                    /* env: day */
    *(int *)(req + 140) = model_type;           /* 0 HD_180P, 1 HD_360P */
    *(int *)(req + 144) = classify;             /* classify threshold 0..1000 */
    *(int *)(req + 148) = iou;                  /* IoU threshold 0..10 */
    *(int *)(req + 152) = target;               /* 2 = shape + face */

    int g = open(NPU_GUARD, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (g >= 0) { fsync(g); close(g); sync(); }
    printf("[npu] create: calling ioctl (if the log stops here, it hung)\n"); fflush(stdout);
    int rc = ioctl(fd, NPU_CREATE_CMD, req);
    printf("[npu] create: %s\n", rc == 0 ? "ok" : strerror(errno));
    if (rc == 0) {
        char hex[156 * 2 + 1];
        for (int i = 0; i < 156; i++) snprintf(hex + 2 * i, 3, "%02x", req[i]);
        printf("[npu] create reply: %s\n", hex);
    }
    close(fd);
    if (g >= 0) { unlink(NPU_GUARD); sync(); }
    return rc == 0 ? 0 : -1;
}

/* The create ioctl can block inside the driver (seen 2026-11-12): run it on a
 * detached thread so the web server and the streams are never taken down by
 * it. The guard stays set while it runs; if it never returns, the next boot
 * disables the NPU. */
static void *create_thread(void *arg)
{
    (void)arg;
    npu_create(1, 700, 3, 2);
    return NULL;
}

int npu_create_async(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 32 * 1024);
    int rc = pthread_create(&t, &a, create_thread, NULL);
    pthread_attr_destroy(&a);
    return rc;
}

int npu_start(void) { return s_enabled ? npu_test() : 0; }

void npu_init(void) { CTRL_REG("npu.enabled", p_en, CTRL_PERSIST); }
