#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "isp.h"
#include "ircut.h"
#include "ae.h"
#ifdef AEC_CUSTOM
#include "aec_custom.h"
#endif
#include "night.h"
#include "control.h"
#include "auth.h"
#include "web.h"
#include "resetkey.h"
#include "system.h"
#include "netcfg.h"
#include "qrwifi.h"
#include "speaker.h"
#include "osd.h"
#include "osd_hw.h"
#include "mqtt.h"
#include "events.h"
#include "sounddet.h"
#include "md.h"
#include "tamper.h"
#include "sched.h"
#include "substream.h"
#include "term.h"
#include "npu.h"
#include "record.h"
#include "onvif.h"
#include "imgx.h"
#include "snapshot.h"
#include "tls.h"
#include <sys/time.h>
#include "vi.h"
#include "venc.h"
#include "audio.h"
#include "rtsp.h"
#include "version.h"

/* -------------------------------------------------------------------------
 * Frame grab — triggered by SIGUSR1
 * Send with: kill -USR1 $(pidof ak_rtsp)
 * Saves 640×360 NV12 to /mnt/grab_NNNN.yuv; view with:
 *   ffplay -f rawvideo -video_size 640x360 -pixel_format nv12 /mnt/grab_0001.yuv
 * ------------------------------------------------------------------------- */
static volatile sig_atomic_t g_grab_request = 0;
static int                   g_grab_counter = 0;

static void sigusr1_handler(int sig) { (void)sig; g_grab_request = 1; }

/* -------------------------------------------------------------------------
 * Global state definitions (declared extern in ak_rtsp.h)
 * ------------------------------------------------------------------------- */
int g_isp_fd   = -1;
int g_subdev   = -1;
int g_vid_main = -1;
int g_vid_sub  = -1;
int g_vid_td   = -1;
int g_venc_fd  = -1;
int g_chn0_fd  = -1;

void     *g_venc_virt = MAP_FAILED;
uint32_t  g_venc_phys = 0;

void     *g_vi_bufs[VI_BUF_COUNT];
uint32_t  g_vi_buf_sizes[VI_BUF_COUNT];

void     *g_vi_sub_bufs[VI_SUB_BUF_COUNT];
uint32_t  g_vi_sub_buf_sizes[VI_SUB_BUF_COUNT];
uint32_t  g_vi_sub_mmap_offsets[VI_SUB_BUF_COUNT];

void     *g_vi_td_bufs[VI_TD_BUF_COUNT];
uint32_t  g_vi_td_buf_sizes[VI_TD_BUF_COUNT];

/* -------------------------------------------------------------------------
 * Open ISP + VI device nodes only (VENC must NOT be open during STREAMON).
 *
 * The ak_isp kernel driver triggers b2i_pp_chn_init (ISP bridge init) at
 * STREAMON time when no VENC channel is open. If /dev/venc-chn0 is already
 * open, the driver defers b2i_pp_chn_init to CREATE_ENC time instead, at
 * which point some bridge parameter is still zero → kernel division by zero.
 *
 * This matches anyka_ipc's sequence: ISP/VI open → STREAMON (triggers
 * b2i_pp_chn_init with correct slice params) → THEN open VENC devices.
 * ------------------------------------------------------------------------- */
static int open_isp_vi_devices(void)
{
    g_isp_fd   = open(DEV_ISP,        O_RDWR);
    g_subdev   = open(DEV_SUBDEV,     O_RDWR | O_NONBLOCK);
    g_vid_main = open(DEV_VIDEO_MAIN, O_RDWR | O_NONBLOCK);
    g_vid_sub  = open(DEV_VIDEO_SUB,  O_RDWR);  /* blocking: AK VI driver ignores F_SETFL, DQBUF needs blocking open */
    g_vid_td   = open(DEV_VIDEO_TD,   O_RDWR | O_NONBLOCK);

    if (g_isp_fd < 0 || g_subdev < 0 || g_vid_main < 0 || g_vid_sub < 0 || g_vid_td < 0) {
        perror("open_isp_vi_devices");
        return -1;
    }
    printf("[ak_rtsp] ISP/VI devices opened\n");
    return 0;
}

/* Open VENC device nodes — call AFTER vi_start_capture() returns (STREAMON done). */
static int open_venc_devices(void)
{
    g_venc_fd = open(DEV_VENC,      O_RDWR);
    g_chn0_fd = open(DEV_VENC_CHN0, O_RDWR | O_NONBLOCK);

    if (g_venc_fd < 0 || g_chn0_fd < 0) {
        perror("open_venc_devices");
        return -1;
    }
    printf("[ak_rtsp] VENC devices opened\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Close all device nodes and unmap DMA regions
 * ------------------------------------------------------------------------- */
static void close_devices(void)
{
    int i;
    uint32_t chn_id = 0;

    osd_hw_stop();
    ioctl(g_chn0_fd, VENC_IOCTL_DEACTIVATE, &chn_id);

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(g_vid_main, VI_IOCTL_STREAMOFF, &type);
    ioctl(g_vid_sub,  VI_IOCTL_STREAMOFF, &type);
    ioctl(g_vid_td,   VI_IOCTL_STREAMOFF, &type);

    for (i = 0; i < VI_BUF_COUNT; i++)
        if (g_vi_bufs[i] && g_vi_bufs[i] != MAP_FAILED)
            munmap(g_vi_bufs[i], g_vi_buf_sizes[i]);

    for (i = 0; i < VI_SUB_BUF_COUNT; i++)
        if (g_vi_sub_bufs[i] && g_vi_sub_bufs[i] != MAP_FAILED)
            munmap(g_vi_sub_bufs[i], g_vi_sub_buf_sizes[i]);

    for (i = 0; i < VI_TD_BUF_COUNT; i++)
        if (g_vi_td_bufs[i] && g_vi_td_bufs[i] != MAP_FAILED)
            munmap(g_vi_td_bufs[i], g_vi_td_buf_sizes[i]);

    audio_stop();

    if (g_venc_virt != MAP_FAILED) munmap(g_venc_virt, ENC_VENC_SIZE);
    if (g_chn0_fd  >= 0) close(g_chn0_fd);
    if (g_venc_fd  >= 0) close(g_venc_fd);
    if (g_vid_td   >= 0) close(g_vid_td);
    if (g_vid_sub  >= 0) close(g_vid_sub);
    if (g_vid_main >= 0) close(g_vid_main);
    if (g_subdev   >= 0) close(g_subdev);
    if (g_isp_fd   >= 0) close(g_isp_fd);
}

/* -------------------------------------------------------------------------
 * Entry point
 *
 * Prerequisites:
 *   Kernel modules loaded by sensor_driver.sh on normal boot:
 *     ak_isp.ko, sensor_gc20c3.ko, ak_venc_adapter.ko, ak_venc_bridge.ko
 *   /tmp/sensor_isp.conf must exist (sensor_driver.sh copies isp_gc20c3.conf).
 *
 * Test procedure (from SD card):
 *   sh /mnt/start_rtsp.sh
 *   Connect VLC to rtsp://CAMERA_IP:554/stream
 *
 * The start_rtsp.sh script handles:
 *   - kill anyka_ipc + watchdog
 *   - rmmod/insmod venc modules (clean kernel state)
 *   - feed /dev/watchdog in background
 *   - run this binary
 *
 * Init sequence:
 *   venc_reset_stale_channel  — clear any state left by a killed anyka_ipc
 *   open_devices              — open all /dev/* nodes
 *   isp_load_sensor_conf      — program GC20C3 I2C registers
 *   isp_pipeline_init         — program 24 ISP image-pipeline modules
 *   venc_reset_stale_channel  — clear any state left by a killed anyka_ipc
 *   open_isp_vi_devices       — open ISP + VI fds (VENC must NOT be open yet)
 *   isp_load_sensor_conf      — program GC20C3 I2C registers
 *   isp_pipeline_init         — program 24 ISP image-pipeline modules
 *   vi_set_channel_attr       — set done_mode/slice/crop/res/buf_size BEFORE REQBUFS
 *   vi_start_capture          — STREAMOFF(stale) → REQBUFS → QUERYBUF → QBUF →
 *                               STREAMON (b2i_pp_chn_init fires, no VENC open) →
 *                               STREAMOFF (ISP stops; bridge ISP-side state kept)
 *   open_venc_devices         — NOW open /dev/venc + /dev/venc-chn0
 *   SET_CHN_STATE=1           — bypass CREATE_POOL/CREATE_ENC paradox
 *   venc_create_pool          — QUERY_DMA + CREATE_POOL ring + ext + mmap
 *   venc_create_encoder       — CREATE_ENC H.264 Main 1920×1080
 *   venc_bind_vi              — BIND_VI_CHN (b2i_pp_chn_init fires again, ISP
 *                               stopped → no interrupt race → no div/0)
 *   venc_activate             — ACTIVATE
 *   vi_start_stream           — re-QBUF → STREAMON (ISP starts; both bridge
 *                               sides connected → frames flow)
 *   audio_start               — open /dev/pcmC0D0c, configure mic capture,
 *                               start its background read() thread
 *                               (non-fatal — video keeps working if this fails)
 *   accept loop               — RTSP (554) + RTSPS (322), each session in its own thread
 * ------------------------------------------------------------------------- */
/* Why the process died: a fatal signal (segfault, bus error, abort...) or a
 * supervisor kill used to leave no trace in the log, only a restart. The
 * handler is async-signal-safe (write(2) with a number formatter, no libc
 * allocation) and records the fault address and instruction pointer, which
 * can be resolved against the unstripped build. */
/* In-progress markers of the kernel-crash guards (ae.c, substream.c, venc.c,
 * osd_hw.c): keep in sync with those files. */
static const char *const k_guard_files[] = {
    "/etc/config/ak_rtsp_tnr_guard", "/etc/config/ak_rtsp_sub_guard",
    "/etc/config/ak_rtsp_venc_guard", "/etc/config/ak_rtsp_osd_guard",
};

static void log_num(char *p, size_t *n, unsigned long v)
{
    char t[24];
    int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) p[(*n)++] = t[--k];
}

static void fatal_signal(int sig, siginfo_t *si, void *uc)
{
    char b[224];
    size_t n = 0;
    const char *pre = "FATAL: signal ";
    for (const char *s = pre; *s; s++) b[n++] = *s;
    log_num(b, &n, (unsigned long)sig);
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
        const char *at = " faultaddr=0x";
        for (const char *s = at; *s; s++) b[n++] = *s;
        unsigned long a = (unsigned long)si->si_addr;
        int started = 0;
        for (int sh = 28; sh >= 0; sh -= 4) {
            unsigned d = (a >> sh) & 0xf;
            if (d || started || sh == 0) { b[n++] = "0123456789abcdef"[d]; started = 1; }
        }
        const char *pc = " pc=0x";
        for (const char *s = pc; *s; s++) b[n++] = *s;
        unsigned long ip = 0;
#if defined(__arm__)
        /* ucontext_t: uc_flags, uc_link, stack_t (12 B), then mcontext_t whose
         * sigcontext starts at +0x14: trap, err, oldmask, r0..r10, fp, ip, sp,
         * lr, pc -> ARM_pc at +0x5c. */
        if (uc) ip = *(unsigned long *)((char *)uc + 0x5c);
#endif
        started = 0;
        for (int sh = 28; sh >= 0; sh -= 4) {
            unsigned d = (ip >> sh) & 0xf;
            if (d || started || sh == 0) { b[n++] = "0123456789abcdef"[d]; started = 1; }
        }
        /* caller, so a crash inside libc can be traced to our call site */
        const char *lr = " lr=0x";
        for (const char *s = lr; *s; s++) b[n++] = *s;
        unsigned long ra = 0;
#if defined(__arm__)
        if (uc) ra = *(unsigned long *)((char *)uc + 0x58);
#endif
        started = 0;
        for (int sh = 28; sh >= 0; sh -= 4) {
            unsigned d = (ra >> sh) & 0xf;
            if (d || started || sh == 0) { b[n++] = "0123456789abcdef"[d]; started = 1; }
        }
    }
    b[n++] = '\n';
    ssize_t w = write(2, b, n);
    int lf = control_sd_log_fd();               /* SD log, when enabled */
    if (lf >= 0) w = write(lf, b, n);
    if (sig != SIGTERM && sig != SIGINT && sig != SIGHUP) {
        /* a real crash: keep the line on CONFIG, shown in the web Health panel */
        int cf = open(LAST_CRASH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (cf >= 0) {
            char t[32];
            size_t tn = 0;
            const char *at = "time=";
            for (const char *s = at; *s; s++) t[tn++] = *s;
            log_num(t, &tn, (unsigned long)time(NULL));
            t[tn++] = ' ';
            w = write(cf, t, tn);
            w = write(cf, b, n);
            fsync(cf);
            close(cf);
        }
    }
    /* We are a process dying with the kernel intact, so whatever else
     * happened, the kernel-crash guards (TNR reference frames, encoders,
     * overlay) did not catch a kernel crash: drop their in-progress markers
     * instead of letting the next start disable those features for good. */
    for (size_t i = 0; i < sizeof k_guard_files / sizeof k_guard_files[0]; i++)
        unlink(k_guard_files[i]);
    (void)w;
    _exit(128 + sig);
}

static void install_fatal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = fatal_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);      /* logged too: tells a supervisor kill from a fault */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);      /* a peer that closed first must not kill us */
}

int main(int argc, char **argv)
{
    /* Very first thing: tees stdout so the TCP tuning server's debug log
     * view gets the FULL boot log (ISP/VI/VENC init, not just ae/night's
     * own printf calls). See control.c for why this is a stdout tee rather
     * than per-callsite logging changes. */
    control_start();
    install_fatal_handlers();

    /* Login store first, then the one-shot SD-card reset files (they may
     * wipe the login or the saved settings before anything uses them). */
    auth_init();
    osd_init();
    rtsp_register_params();
    sys_register_params();
    audio_register_params();
    mqtt_init();
    events_init();
    sounddet_init();
    md_init();
    tamper_init();
    night_init();
    sched_init();
    substream_init();
    web_register_params();
    term_init();
    npu_init();
    venc_register_params();
    record_init();
    onvif_init();
    imgx_init();
    snapshot_init();
    resetkey_check_sd();
    control_load_ext_settings();        /* module settings: needed early (encoder bitrate/GOP) */

    int         no_isp_init       = 0;
    const char *grab_then_exit    = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-isp-init") == 0) {
            no_isp_init = 1;
        } else if (strcmp(argv[i], "--grab-then-exit") == 0 && i + 1 < argc) {
            grab_then_exit = argv[++i];
        }
    }

    print_build_info();
    signal(SIGUSR1, sigusr1_handler);

    venc_reset_stale_channel();

    /* Step 1: Open and configure the ISP and Video Input (VI) devices.
     * Starts capturing so the ISP-side bridge is initialized cleanly. */
    if (open_isp_vi_devices()    < 0) goto err;

    if (no_isp_init) {
        /* --no-isp-init DOES NOT WORK as a diagnostic.
         *
         * Intent was: skip isp_pipeline_init() to inherit anyka_ipc's converged
         * ISP state and compare image quality.
         *
         * Why it fails: when anyka_ipc exits, the ISP kernel driver resets ALL
         * its software-managed state (exposure attr, AE stats, etc.) to zero.
         * The hardware silicon may retain register values, but the kernel driver
         * structures are zeroed. Result: exp_type=0 (uninitialized zero = AUTO
         * mode), priv[exp]=0, luma=0 permanently, hw_exp=256 stuck, NO FRAMES.
         *
         * Do not attempt to use --no-isp-init as a diagnostic again. */
        printf("[ak_rtsp] --no-isp-init: WARNING — ISP kernel state reset on anyka_ipc exit.\n");
        printf("[ak_rtsp]   Frames will not work. See main.c comment for explanation.\n");
    } else {
        if (isp_load_sensor_conf()   < 0) goto err;
        if (isp_pipeline_init()      < 0) goto err;
    }

    /* Must run before vi_set_channel_attr()/vi_start_capture() (STREAMON) --
     * matches anyka_ipc's own strace-confirmed ordering (it issues every ISP
     * tunnel call, including sensor-fps/frame-rate, before touching any VI
     * channel at all). Doing this after STREAMON left ak_isp.ko's per-channel
     * fps cache zeroed when the first frame interrupt landed, causing a
     * kernel "Division by zero" — see ae_init_isp_params()'s comment in
     * ae.c. */
    if (ae_init_isp_params()     < 0) goto err;

    ircut_init();
    if (vi_set_channel_attr()    < 0) goto err;
    if (vi_start_capture()       < 0) goto err;

#ifdef AEC_CUSTOM
    if (aec_custom_start()       < 0) goto err;
#else
    if (ae_start()               < 0) goto err;
#endif

    /* Step 2: Open and configure VENC.
     * Since bind resolution parameters are now correctly passed, we can safely
     * set up the encoder and connect the bridge while the ISP is active. */
    if (open_venc_devices()      < 0) goto err;

    /* Override the kernel's channel state to 1.
     * Bypasses the CREATE_POOL/CREATE_ENC paradox. */
    if (ioctl(g_chn0_fd, 0x406056c7, 1) < 0)
        perror("VENC_IOCTL_SET_CHN_STATE (non-fatal)");

    if (venc_create_pool()       < 0) goto err;
    if (venc_create_encoder()    < 0) goto err;
    if (venc_bind_vi()           < 0) goto err;
    if (venc_activate()          < 0) goto err;

    night_start();  /* needs AE samples flowing, so start after ae_start() */
    osd_hw_start(); /* burned-in overlay: ISP OSD regions, needs the pipeline running */
    substream_start();   /* second encoder (video.sub, crash-guarded) */

    /* Non-fatal: video streaming must keep working even if mic capture
     * fails to init (e.g. an audio device race — a supervisor-respawned
     * anyka_ipc could still be holding it in some future scenario we
     * haven't hit yet). */
    speaker_init();   /* amplifier GPIO off before the playback stream starts */
    if (audio_start() < 0)
        printf("[ak_rtsp] audio: init failed, continuing video-only\n");

    /* Saved image/day-night settings, then the web admin page and the
     * reset-button watcher. */
    sys_time_set_tz(TIME_DEFAULT_TZ);   /* overridden by saved settings */
    control_load_settings();
    imgx_ready();                       /* WDR / privacy masks (crash-guarded) */
    sys_time_start();
    net_start();
    {   /* no saved Wi-Fi network at all: look for a Wi-Fi QR code */
        FILE *wc = fopen(NET_STA_CONF, "r");
        char buf[1024] = "";
        if (wc) { size_t n = fread(buf, 1, sizeof(buf) - 1, wc); buf[n] = 0; fclose(wc); }
        if (!strstr(buf, "network={")) qrwifi_start(600, "no Wi-Fi configured");
    }
    events_start();
    mqtt_start();
    md_start();
    tamper_start();
    sched_start();
    record_start();
    sys_led_start();
    npu_start();          /* NPU person detection (npu.enabled, guarded) */
    onvif_start();
    if (web_start() < 0)      printf("[ak_rtsp] web: failed to start admin page\n");
    if (resetkey_start() < 0) printf("[ak_rtsp] reset button watcher failed to start\n");

    /* Grab-then-exit: DQBUF blocks (O_NONBLOCK temporarily cleared in
     * vi_grab_sub_frame) until the first sub channel frame arrives. */
    if (grab_then_exit) {
        printf("[ak_rtsp] grab-then-exit: waiting for first sub channel frame...\n");
        vi_grab_sub_frame(grab_then_exit);
        close_devices();
        return 0;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); goto err; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(RTSP_SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(server_fd); goto err;
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen"); close(server_fd); goto err;
    }

    printf("[ak_rtsp] Listening on port %d...\n", RTSP_SERVER_PORT);

    /* RTSPS: same sessions, over TLS (tls.c; handshake refused until the
     * certificate is ready). Optional: plain RTSP works without it. */
    int tls_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tls_fd >= 0) {
        setsockopt(tls_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        server_addr.sin_port = htons(RTSPS_SERVER_PORT);
        if (bind(tls_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0 ||
            listen(tls_fd, 5) < 0) {
            perror("[ak_rtsp] rtsps bind/listen");
            close(tls_fd);
            tls_fd = -1;
        } else {
            printf("[ak_rtsp] RTSPS on port %d\n", RTSPS_SERVER_PORT);
        }
    }

    for (;;) {
        struct pollfd pfds[2] = { { server_fd, POLLIN, 0 }, { tls_fd, POLLIN, 0 } };

        /* While waiting for a client, drain the VENC ring so it doesn't wrap.
         * anyka_ipc polls GET_STREAM continuously even without a connected client.
         * Without draining, the ring wraps in ~3s at 2Mbps (ENC_VENC_SIZE=777KB),
         * and the next client would need up to one GOP (2s) to sync to an I-frame. */
        if (g_grab_request) {
            g_grab_request = 0;
            char path[64];
            snprintf(path, sizeof(path), "/mnt/grab_%04d.yuv", ++g_grab_counter);
            vi_grab_sub_frame(path);
        }

        /* 20 ms wait instead of 0: frames arrive every 66 ms (15 fps), so the
         * ring is still drained ~3x per frame, but the idle loop no longer
         * pins the CPU at 100% (heat, and starving other threads). */
        int p = poll(pfds, tls_fd >= 0 ? 2 : 1, 20);
        drain_venc_ring();          /* every pass: all sessions read from the live hub */
        if (p < 0 && errno != EINTR) { perror("poll"); continue; }
        if (p <= 0) continue;

        int is_tls = !(pfds[0].revents & POLLIN) && tls_fd >= 0 && (pfds[1].revents & POLLIN);
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(is_tls ? tls_fd : server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) { perror("accept"); continue; }
        rtsp_start_client(client_fd, is_tls, &client_addr, addr_len);   /* own thread (TLS handshake too) */
    }

    close(server_fd);

err:
    night_stop();
#ifdef AEC_CUSTOM
    aec_custom_stop();
#else
    ae_stop();
#endif
    control_stop();
    close_devices();
    /* The supervisor restarts us at once; give the kernel encoder/ISP half a
     * minute to settle instead of starting again on top of a half-freed
     * channel (that made every retry die even earlier: "input stream buffer
     * too small" storm from ak_venc). */
    printf("[ak_rtsp] startup failed, waiting 30 s before exit\n");
    sync();
    sleep(30);
    return 1;
}
