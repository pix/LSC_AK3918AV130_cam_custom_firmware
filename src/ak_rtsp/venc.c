#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stddef.h>

#include "ak_rtsp.h"
#include "ctrl_params.h"
#include <unistd.h>
#include "ak_ioctls.h"
#include "venc.h"
#include "monotime.h"
#include "night.h"
#include <stdlib.h>

/* DEBUG_IOCTL_DUMP (opt-in, off by default — see Makefile `debug-dump`
 * target): dumps our own VENC_IOCTL_CREATE_ENC payload to
 * /tmp/ak_rtsp_ioctl_dump.log in the exact same format isp_hook.c uses for
 * stock anyka_ipc's capture (ExtractedData/isp_hook_n.log), via the shared
 * venc_dump.h — so the two can be diffed directly instead of eyeballed, and
 * so a struct-packing/offset bug would show up as an obvious mismatch
 * before ever touching hardware. Not compiled into the normal build: this
 * links a real libc so it uses plain write(2), unlike isp_hook.c's raw
 * syscalls (that file must stay libc-free to LD_PRELOAD onto anyka_ipc
 * regardless of which libc it's linked against). */
#ifdef DEBUG_IOCTL_DUMP
#define AKDUMP_WRITE(fd, ptr, len) write((fd), (ptr), (len))
#include "venc_dump.h"
#endif

/* -------------------------------------------------------------------------
 * Reset stale venc channel state left by a previously killed anyka_ipc.
 *
 * When anyka_ipc is SIGKILLed while streaming, venc_chn_dev_close calls
 * venc_destroy_chn but that function returns early (error 0x300000e) because
 * chn_attr[2] (stream-active flag) is still 1. This leaves:
 *   chn_dev+0x28 = valid (stale) chn_attr pointer (vfree never called)
 *   chn_dev+0x30 = 1  (encoder-created flag, NOT reset)
 *
 * Consequence: our subsequent CREATE_ENC sees chn_dev+0x30=1 and silently
 * returns 0 without creating the encoder. Later ioctls dereference the NULL
 * chn_attr (set to NULL by a previous successful close) and crash.
 *
 * Fix: open the device, call DEACTIVATE to zero out chn_attr[2], then close.
 * The close re-triggers venc_destroy_chn; now chn_attr[2]==0 → takes the
 * success path → vfree(chn_attr) + chn_dev+0x30=0 + chn_dev+0x34=1.
 * A subsequent open gets a fully clean channel.
 *
 * Safe to call unconditionally: if the channel is already clean (no stale
 * anyka_ipc state), DEACTIVATE returns an error (ignored) and close is a
 * no-op with respect to chn_dev+0x30.
 * ------------------------------------------------------------------------- */
void venc_reset_stale_channel(void)
{
    int fd = open(DEV_VENC_CHN0, O_RDWR | O_NONBLOCK);
    if (fd < 0) return;

    uint32_t chn_id = 0;
    int ret = ioctl(fd, VENC_IOCTL_DEACTIVATE, &chn_id);
    printf("[ak_rtsp] venc_reset_stale: DEACTIVATE ret=%d errno=%d\n", ret, errno);

    close(fd);
    printf("[ak_rtsp] venc channel reset done\n");
}

/* -------------------------------------------------------------------------
 * Query the kernel for exact DMA block sizes the AL encoder needs.
 *
 * ioctl(g_venc_fd, VENC_IOCTL_QUERY_DMA, buf) on the GLOBAL /dev/venc fd.
 * This mirrors ak_venc_open_ex → venc_get_req_dma_block @ 0x001d0c88.
 *
 * Input struct (84 bytes = uint32_t[21]):
 *   offset  0 (uint32): profile_idc  (77=Main)
 *   offset  4 (uint32): chroma_mode  (1 = YUV420)
 *   offset  8 (uint16): width
 *   offset 10 (uint16): height
 *   offset 12 (uint16): max_fps = 60
 *   offset 14 (uint16): smart_mode  (0 = off)
 *   offset 28 (uint8):  1  (hardcoded in venc_get_req_dma_block, always)
 *   offset 29 (uint8):  bEnableMMA = 1
 *   offset 32 (uint8):  MVVRange_factor = 40
 *
 * Output (kernel overwrites the same struct):
 *   buf[0]    = N  (number of DMA blocks required)
 *   buf[1..N] = block sizes in bytes
 *
 * SIDE EFFECT: this call also caches the H264 encoder config (profile, width,
 * height) in kernel channel state. CREATE_ENC reads from that cached state
 * and ignores the codec_type_code field in venc_create_enc_req — so calling
 * this BEFORE CREATE_POOL/CREATE_ENC is required.
 *
 * Ring pool size = Σ ((block_size + 31) & ~31) over all N blocks.
 * For H264 Main 1920×1080 with MMA: 8 blocks, total = 4120384 (ENC_RING_SIZE).
 * ------------------------------------------------------------------------- */
/* video.profile: H.264 Main (77, stock) or High (100, CABAC + 8x8, a bit
 * better at the same bitrate). Read straight from the saved settings because
 * the encoder is created before control_load_settings(). Needs a restart. */
static int s_vprofile;                    /* 0 = main, 1 = high */
static int vp_get(char *o, size_t c) { snprintf(o, c, "%s", s_vprofile ? "high" : "main"); return 0; }
static int vp_set(const char *v, const char **e)
{
    if (!strcmp(v, "main")) s_vprofile = 0;
    else if (!strcmp(v, "high")) s_vprofile = 1;
    else { *e = "must be main or high (takes effect after a restart)"; return -1; }
    return 0;
}
static uint32_t venc_profile(void) { return s_vprofile ? H264_PROFILE_IDC_HIGH : H264_PROFILE_IDC_MAIN; }

static int g_smart_used;                  /* smart mode the encoder runs (stream settings below) */
static void venc_guard_decide(void);

static uint32_t venc_query_ring_pool_size(void)
{
    uint32_t buf[21];
    memset(buf, 0, sizeof(buf));

    buf[0] = venc_profile();                 /* profile_idc */
    buf[1] = 1;                               /* chroma_mode = YUV420 */
    ((uint16_t *)buf)[4] = ENC_WIDTH;         /* width  */
    ((uint16_t *)buf)[5] = ENC_HEIGHT;        /* height */
    ((uint16_t *)buf)[6] = 60;                /* max_fps = 60 */
    /* smart_mode: must match CREATE_ENC. LTR (1) and skip-frame (3) need two
     * more reference buffers (AKV_Encoder_Get_Dma_Memory_Info: (mode & ~2)
     * == 1); a pool sized without them is the dma_pool_alloc_obj crash. */
    ((uint16_t *)buf)[7] = (uint16_t)g_smart_used;
    ((uint8_t  *)buf)[28] = 1;               /* hardcoded 1 */
    ((uint8_t  *)buf)[29] = 1;               /* bEnableMMA = 1 */
    ((uint8_t  *)buf)[32] = 40;              /* MVVRange_factor = 40 */

    if (ioctl(g_venc_fd, VENC_IOCTL_QUERY_DMA, buf) < 0) {
        perror("VENC_IOCTL_QUERY_DMA");
        printf("[ak_rtsp] QUERY_DMA failed; falling back to ENC_RING_SIZE=%u\n", ENC_RING_SIZE);
        return ENC_RING_SIZE;
    }

    uint32_t count = buf[0];
    printf("[ak_rtsp] QUERY_DMA: %u DMA blocks required\n", count);
    if (count == 0 || count > 20) {
        printf("[ak_rtsp] QUERY_DMA: unexpected count %u; falling back to ENC_RING_SIZE\n", count);
        return ENC_RING_SIZE;
    }

    uint32_t pool_size = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t sz      = buf[1 + i];
        uint32_t aligned = (sz + 31u) & ~31u;
        printf("[ak_rtsp]   block[%u] = %u bytes (aligned %u)\n", i, sz, aligned);
        pool_size += aligned;
    }
    printf("[ak_rtsp] QUERY_DMA ring pool size = %u bytes\n", pool_size);
    return pool_size;
}

/* -------------------------------------------------------------------------
 * Create venc DMA pool + mmap the bitstream buffer.
 *
 * Order matches ak_venc_open_ex exactly:
 *   1. QUERY_DMA → kernel caches H264 config + returns ring pool size
 *   2. CREATE_POOL ring (size from query)
 *   3. CREATE_POOL ext (size = ENC_VENC_SIZE bitstream ring)
 *   4. mmap ext pool into userspace
 * ------------------------------------------------------------------------- */
int venc_create_pool(void)
{
    printf("[ak_rtsp] sizeof(venc_dma_pool_desc)=%d\n", (int)sizeof(struct venc_dma_pool_desc));

    char vp[8] = "";                          /* saved before settings are replayed */
    if (control_saved_get("video.profile", vp, sizeof vp) == 0) s_vprofile = !strcmp(vp, "high");
    if (s_vprofile) printf("[venc] H.264 High profile\n");
    venc_guard_decide();                      /* before QUERY_DMA: it sizes the pool for the smart mode */

    uint32_t ring_size = venc_query_ring_pool_size();

    struct venc_dma_pool_desc pool;

    memset(&pool, 0, sizeof(pool));
    pool.create      = 1;
    pool.is_external = 0;
    pool.pool_size   = ring_size;
    if (ioctl(g_chn0_fd, VENC_IOCTL_CREATE_POOL, &pool) < 0) {
        perror("VENC_IOCTL_CREATE_POOL ring");
        return -1;
    }

    memset(&pool, 0, sizeof(pool));
    pool.create      = 1;
    pool.is_external = 1;
    pool.pool_size   = ENC_VENC_SIZE;
    if (ioctl(g_chn0_fd, VENC_IOCTL_CREATE_POOL, &pool) < 0) {
        perror("VENC_IOCTL_CREATE_POOL ext");
        return -1;
    }

    /* phys_base  (+0x10) = kernel virtual address of the DMA region (0xc...).
     * phys_base2 (+0x14) = true physical address of the DMA region  (0x81...).
     *
     * GET_STREAM returns frame_offset as a KERNEL VIRTUAL address inside the
     * ring buffer, so frame pointer arithmetic must use phys_base (kv) as the
     * base, not phys_base2 (physical):
     *
     *   data_virt = g_venc_virt + (frame_offset - g_venc_phys)
     *             = mmap_base   + (kv_offset    - kv_base)
     *
     * Confirmed from run log: frame_offset=0xc62120a0, kv_base=0xc61f6000
     *   → ring offset = 0x1c0a0 = 115,872 bytes  (within 777,600-byte ring ✓)
     * Using phys_base2 (0x81ad0000) gives a wild pointer → segfault. */
    g_venc_phys = pool.phys_base;   /* kernel virtual base — used for frame_offset arithmetic */

    g_venc_virt = mmap(NULL, ENC_VENC_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, g_chn0_fd, 0);
    if (g_venc_virt == MAP_FAILED) {
        perror("mmap venc");
        return -1;
    }
    printf("[ak_rtsp] venc DMA virt=%p kv_base=0x%08x phys=0x%08x\n",
           g_venc_virt, g_venc_phys, pool.phys_base2);
    return 0;
}

/* -------------------------------------------------------------------------
 * Create H.264 encoder.
 *
 * IMPORTANT: codec_type_code MUST be H264_PROFILE_IDC_MAIN (77), not 0x42.
 * Passing 0x42 accidentally triggers the H265 code path inside
 * AL_SchedulerCpu_CreateChannel → over-allocates DMA → ring pool crash.
 * The encoder takes profile/width/height from the kernel channel state cached
 * by VENC_IOCTL_QUERY_DMA, not from this struct.
 * ------------------------------------------------------------------------- */
/* ---- stream settings (applied when the encoder is created, i.e. at boot) ----
 * Crash guard: the encoder has crashed the kernel before on bad rate-control
 * values, so non-default values are bracketed by a marker on CONFIG; if the
 * camera did not get 150 frames out after the last start, the next start
 * ignores them and uses the defaults (venc_startup_ok clears the marker). */
#define VENC_GUARD "/etc/config/ak_rtsp_venc_guard"
static int s_kbps = 1536, s_gop = ENC_GOP, g_guarded;
CTRL_INT(p_kbps, s_kbps, 256, 4096)
CTRL_INT(p_gop, s_gop, 10, 150)
static char g_venc_note[96];

/* video.smart: Anyka "smart encoding" (SDK smart_mode). gop = variable GOP:
 * while the scene is static the keyframe interval stretches to
 * SMART_GOPLEN frames (fewer I-frames, the bulk of a static scene's
 * bitrate); ltr = long-term reference (P-frames can reference an old good
 * frame instead of a new I-frame). The static/moving decision is the
 * driver's own (akv_stream_parse_mdinfo -> Stream_state, threshold
 * SMART_STATIC). Values: Anyka's akipc app and the LSC factory config.
 * Experimental, off by default; covered by the crash guard. Restart. */
#define SMART_GOPLEN  300                 /* frames (20 s at 15 fps) */
#define SMART_QUALITY 100
#define SMART_STATIC  550
static int s_smart;                       /* setting: 0 off, 1 ltr, 2 gop (driver values) */
static int sm_get(char *o, size_t c) { snprintf(o, c, "%s", s_smart == 1 ? "ltr" : s_smart == 2 ? "gop" : "off"); return 0; }
static int sm_set(const char *v, const char **e)
{
    if (!strcmp(v, "off")) s_smart = 0;
    else if (!strcmp(v, "ltr")) s_smart = 1;
    else if (!strcmp(v, "gop")) s_smart = 2;
    else { *e = "must be off, gop or ltr (takes effect after a restart)"; return -1; }
    return 0;
}
int venc_smart_now(void) { return g_smart_used; }

/* video.idle_fps: adaptive frame rate (night.c), off | 10 | 5 | 3. Applies
 * immediately (no restart). */
static int s_idle_fps;
static int if_get(char *o, size_t c)
{
    if (s_idle_fps) snprintf(o, c, "%d", s_idle_fps); else snprintf(o, c, "off");
    return 0;
}
static int if_set(const char *v, const char **e)
{
    int f = !strcmp(v, "off") ? 0 : atoi(v);
    if (f != 0 && f != 10 && f != 5 && f != 3) { *e = "must be off, 10, 5 or 3"; return -1; }
    s_idle_fps = f;
    night_set_idle_fps(f);
    return 0;
}

/* video.idle_wake: what brings the full frame rate back: motion in the
 * motion zones (same as recordings/events) or anywhere in the picture. */
static int s_wake_all;
static int iw_get(char *o, size_t c) { snprintf(o, c, "%s", s_wake_all ? "all" : "zones"); return 0; }
static int iw_set(const char *v, const char **e)
{
    if (!strcmp(v, "zones")) s_wake_all = 0;
    else if (!strcmp(v, "all")) s_wake_all = 1;
    else { *e = "must be zones or all"; return -1; }
    night_set_idle_wake_all(s_wake_all);
    return 0;
}

void venc_register_params(void)
{
    CTRL_REG("venc.kbps", p_kbps, CTRL_PERSIST);
    CTRL_REG("venc.gop", p_gop, CTRL_PERSIST);
    CTRL_REG("video.profile", vp, CTRL_PERSIST);
    CTRL_REG("video.smart", sm, CTRL_PERSIST);
    CTRL_REG("video.idle_fps", if, CTRL_PERSIST);
    CTRL_REG("video.idle_wake", iw, CTRL_PERSIST);
}
const char *venc_note(void) { return g_venc_note; }
static int g_kbps_used = 1536, g_gop_used = ENC_GOP;
int venc_kbps_now(void) { return g_kbps_used; }
int venc_gop_now(void) { return g_gop_used; }
void venc_startup_ok(void)
{
    if (g_guarded) { unlink(VENC_GUARD); sync(); g_guarded = 0; }
}

/* Decide once, before QUERY_DMA, which stream settings this start uses
 * (module settings are replayed before the encoder is created). */
static void venc_guard_decide(void)
{
    g_kbps_used = s_kbps; g_gop_used = s_gop; g_smart_used = s_smart;
    if (s_kbps != 1536 || s_gop != ENC_GOP || s_smart) {
        if (access(VENC_GUARD, F_OK) == 0) {
            snprintf(g_venc_note, sizeof g_venc_note, "custom stream settings skipped after an incomplete start; using defaults");
            fprintf(stderr, "[venc] %s\n", g_venc_note);
            g_kbps_used = 1536; g_gop_used = ENC_GOP; g_smart_used = 0;
            unlink(VENC_GUARD);        /* defaults are known-good; retry custom values next boot */
        } else {
            int gfd = open(VENC_GUARD, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (gfd >= 0) { fsync(gfd); close(gfd); sync(); }
            g_guarded = 1;
        }
    }
    if (g_smart_used) printf("[venc] smart encoding: %s (GOP up to %d, static threshold %d)\n",
                             g_smart_used == 1 ? "ltr" : "gop", SMART_GOPLEN, SMART_STATIC);
}

/* Ask the encoder for a keyframe now (stock ak_venc_request_idr: ioctl
 * 0x400456f3 -> AKV_Encoder_Set_EncIFrame). Used when a viewer, RTSP client
 * or recording starts, so long smart GOPs never make anyone wait. At most
 * one per second. */
void venc_request_idr(void)
{
    static int64_t last;
    int64_t now = mono_ms();
    if (!g_smart_used || g_chn0_fd < 0 || now - last < 1000) return;
    last = now;
    uint32_t chn = 0;
    if (ioctl(g_chn0_fd, VENC_IOCTL_REQUEST_IDR, &chn) < 0) perror("[venc] request IDR");
}

int venc_create_encoder(void)
{
    int kbps = g_kbps_used, gop = g_gop_used;
    struct venc_create_enc_req req;
    memset(&req, 0, sizeof(req));

    req.chn_id               = 0;
    req.enc.codec_type_code  = venc_profile();
    req.enc.chroma_mode      = 1;    /* CHROMA_4_2_0 */
    req.enc.width            = ENC_WIDTH;
    req.enc.height           = ENC_HEIGHT;
    /* CORRECTED (2026-07-04, isp_hook.c CREATE_ENC byte-exact capture —
     * ExtractedData/isp_hook_n.log, "CREATE_ENC #1 chn_id=0"): stock uses
     * VENC_RC_VBRP (5), NOT VENC_RC_AVBR (0). This was the actual root cause
     * of the earlier "qp_or_kbps=99 is initqp" reasoning below being wrong —
     * that was inferred indirectly from anyka_ipc's own debug log text
     * ("initqp:99, minqp:28, maxqp:43"), which doesn't say which wire field
     * each value lands in for which rc_mode. Under the WRONG assumed mode
     * (AVBR), qp_or_kbps carries initqp; under the REAL mode (VBR+) it
     * carries minqp instead, and qp2/max_kbps_vbrp/initqp_vbrp are real,
     * separate fields — previously left unset (0). The raw hex dump
     * decodes stock's byte-exact values directly: minqp=28, maxqp=43,
     * max_kbps=1536, initqp=99 — the exact _ht_hw_settings.ini values, just
     * in the fields VBR+ actually uses. This does not touch the `rc`
     * (0x40-0x5f) sub-struct at all — still the hardware-confirmed-safe
     * values below, per the crash history below. */
    req.enc.rc_mode          = VENC_RC_VBRP;
    req.enc.fps              = ENC_FPS;
    g_kbps_used = kbps; g_gop_used = gop;
    req.enc.goplen           = (uint16_t)gop;
    req.enc.max_fps          = 0;    /* stock sends 0 for both channels (isp_hook_n.log) */
    req.enc.enc_level        = 40;   /* H.264 Level 4.0 */
    req.enc.qp_or_kbps       = 28;   /* VBR+: minqp (stock isp_hook_n.log capture) */
    req.enc.qp2              = 43;   /* VBR+: maxqp */
    req.enc.max_kbps_vbrp    = (uint32_t)kbps; /* VBR+: max_kbps (stock 1536) */
    req.enc.initqp_vbrp      = 99;   /* VBR+: initqp — the "99" belongs here, not qp_or_kbps */
    req.smart.smart_mode     = (int16_t)g_smart_used;   /* same value QUERY_DMA sized the pool for */
    req.smart.smart_goplen   = g_smart_used ? SMART_GOPLEN : 100;
    req.smart.smart_quality  = g_smart_used ? SMART_QUALITY : 50;
    req.smart.smart_static_value = g_smart_used ? SMART_STATIC : 0;
    /* REVERTED (2026-07-04) — hardware-tested and CRASHES THE KERNEL.
     * A Ghidra trace of venc_check_and_format_rc_param (0x1d0a58) and its
     * caller ht_video_codec_start_encode (0xbef60) appeared to show stock
     * sending an all-zero rc struct (cub_size/minqp/maxqp/delta/I_P_B_pic_size
     * all 0, only rc_flag=1). Tried it on hardware with UART attached this
     * time — confirmed kernel Oops, not a graceful failure:
     *   dma_pool_alloc_obj+0x440 [ak_venc_adapter]
     *     <- venc_dma_pool_malloc
     *     <- AL_SchedulerCpu_CreateChannel   (called from CREATE_ENC)
     *     <- AL_RC_MulDivR                    ("rate control mul-div-round")
     * AL_* is Allegro DVT/Envive H.264 IP naming — AL_RC_MulDivR is doing a
     * scaled (a*b)/c division as part of rate-control channel setup, and it
     * divides by zero. This is direct proof the kernel DOES read/use these
     * rc fields (not a struct-offset misread as first suspected) — almost
     * certainly maxqp-minqp==0 (both zero) used as a QP-range divisor.
     * Reverted to the values confirmed safe on hardware (they stream, just
     * with the macroblocking artifact this was trying to fix). Do NOT set
     * minqp==maxqp (or both zero) here again. The full crash dump and
     * follow-up plan (byte-exact runtime capture via the LD_PRELOAD hook
     * instead of further guessing from Ghidra) is what led to the values
     * below. */
    /* TRIED AND FAILED AGAIN (2026-07-04): setting rc.flag=0/rc.flag2=0/
     * rc.srd_threshold=0 (MMA/MVVRange disabled, minqp/maxqp left at the
     * known-safe 28/43) CRASHES THE KERNEL — same NULL-deref bug as the
     * all-zero rc struct attempt above, confirmed via UART:
     *   dma_pool_alloc_obj+0x440 [ak_venc_adapter]
     *     <- venc_dma_pool_malloc <- AL_SchedulerCpu_CreateChannel
     * A Ghidra trace through AKV_Encoder_Open_Ext (0x2eb7c) beforehand
     * appeared to show flag/flag2/srd_threshold are independently gated
     * ("if nonzero, use; else skip") and don't feed any division — that
     * trace was real but incomplete; it examined the wrong function
     * relative to the actual crash site. This is now the THIRD time
     * (across multiple sessions) that a plausible-looking Ghidra reasoning
     * chain about this rc struct has been contradicted by hardware. A full
     * disassembly-level pass ("RC struct div/0 — full disassembly-level
     * root cause") found the panic is actually a NULL pointer dereference
     * in a DMA-pool free-list search (dma_pool_alloc_obj), not a literal
     * integer division — and the causal link from these specific ioctl
     * fields to that pool exhaustion could NOT be conclusively closed this
     * round. Do not retry
     * flag/flag2/srd_threshold changes without new evidence (e.g. the
     * isp_hook.c CREATE_ENC byte capture) — Ghidra reasoning alone has now
     * failed twice for this struct. Reverted to hardware-confirmed-safe. */
    req.rc_flag              = 1;
    req.rc.cub_size          = 0;
    req.rc.minqp             = 28;
    req.rc.maxqp             = 43;
    req.rc.srd_threshold     = 128;
    req.rc.flag              = 1;   /* bEnableMMA = TRUE */
    req.rc.flag2             = 40;  /* MVVRange_factor = 40 */

    /* Same log block format as anyka_ipc's own ht_video_codec.c:749-760
     * ("===========channel %d venc param===========" / width / height /
     * fps / goplen / target_kbps / max_kbps / minqp / maxqp / initqp /
     * enc_level / "===...==="), so our own debug log is directly
     * line-for-line comparable against anyka_ipc_hooked.log without having
     * to mentally map field names each time. target_kbps and max_kbps are
     * the same field in our VBR+ payload (req.enc.max_kbps_vbrp) — stock's
     * two separate ini values (target_kbps/max_kbps) are also both 1536,
     * so printing the one field twice matches stock's printed values
     * exactly even though our wire struct only carries one of them. */
    printf("===========channel %d venc param===========\n", req.chn_id);
    printf("width:%d\n",        req.enc.width);
    printf("height:%d\n",       req.enc.height);
    printf("fps:%d\n",          req.enc.fps);
    printf("goplen:%d\n",       req.enc.goplen);
    printf("target_kbps:%d\n",  req.enc.max_kbps_vbrp);
    printf("max_kbps:%d\n",     req.enc.max_kbps_vbrp);
    printf("minqp:%d\n",        req.enc.qp_or_kbps);
    printf("maxqp:%d\n",        req.enc.qp2);
    printf("initqp:%d\n",       req.enc.initqp_vbrp);
    printf("enc_level:%d\n",    req.enc.enc_level);
    printf("============================================\n");

    printf("[ak_rtsp] calling CREATE_ENC...\n");
    int ret = ioctl(g_chn0_fd, VENC_IOCTL_CREATE_ENC, &req);
#ifdef DEBUG_IOCTL_DUMP
    {
        int f = open("/tmp/ak_rtsp_ioctl_dump.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            akdump_create_enc(f, "AK_RTSP", &req, (uint32_t)ret);
            close(f);
        }
    }
#endif
    if (ret < 0) {
        perror("VENC_IOCTL_CREATE_ENC");
        return -1;
    }
    printf("[ak_rtsp] CREATE_ENC OK\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Bind venc channel 0 to VI channel 0 (kernel encode mode).
 *
 * encode_mode=1: kernel handles ISP→VENC DMA internally, no userspace
 * involvement needed per frame.
 *
 * Reversed from ak_venc_bind_vi_chn @ 0x001d65c0.
 * IMPORTANT: must be called BEFORE CREATE_POOL/CREATE_ENC (see
 * smolrtsp_integration.md §C for the ordering constraint explanation).
 * ------------------------------------------------------------------------- */
int venc_bind_vi(void)
{
    struct venc_bind_vi_req bind;
    memset(&bind, 0, sizeof(bind));
    bind.venc_chn_id   = 0;
    bind.vi_chn_packed = 0;  /* (dev_idx=0 << 8) | vi_chn=0 */
    bind.param0        = ENC_WIDTH;
    bind.param1        = ENC_HEIGHT;
    bind.encode_mode   = 1;  /* kernel mode */

    printf("[ak_rtsp] calling BIND_VI_CHN...\n");
    if (ioctl(g_chn0_fd, VENC_IOCTL_BIND_VI_CHN, &bind) < 0) {
        perror("VENC_IOCTL_BIND_VI_CHN");
        return -1;
    }
    printf("[ak_rtsp] BIND_VI_CHN OK\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Activate the encoder — starts frame production.
 * Reversed from ak_venc_bind_activate @ 0x001d68a8.
 * ------------------------------------------------------------------------- */
int venc_activate(void)
{
    uint32_t chn_id = 0;
    printf("[ak_rtsp] calling ACTIVATE...\n");
    if (ioctl(g_chn0_fd, VENC_IOCTL_ACTIVATE, &chn_id) < 0) {
        perror("VENC_IOCTL_ACTIVATE");
        return -1;
    }
    printf("[ak_rtsp] ACTIVATE OK\n");
    return 0;
}
