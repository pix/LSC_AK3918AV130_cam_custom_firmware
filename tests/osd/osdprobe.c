/* osdprobe: standalone burned-in OSD experiment (SD test mode, ak_rtsp
 * running so the ISP is streaming). Reproduces stock anyka_ipc's
 * ak_osd_* call sequence (Ghidra: ak_osd_init / ak_osd_create_canvas /
 * FUN_00122d88 / ak_vpss_osd_set_param / pp_set_osd_ioctl):
 *   ION alloc (3 buffers) -> palette (0x4005490a) -> register region
 *   (0x40045003) -> get buffer (0x80045005) -> draw -> commit (0x40045004)
 * The ISP only DMA-*reads* the buffer, so a mistake shows garbage on the
 * picture, not memory corruption. The region is disabled before exit.
 *
 * Usage: osdprobe [chn 0|1] [x] [y] [w] [h] [seconds] [fmt 1|2] [disable 2|5]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define ISP_IOCTL_CMD   0xc0cc5616
#define ISP_INNER_OSD   0x4004496d
#define OSD_PALETTE     0x4005490a
#define OSD_REGISTER    0x40045003
#define OSD_GET_BUF     0x80045005
#define OSD_SET_BUF     0x40045004

struct isp_cmd_wrapper { int flag; int inner_cmd; void *payload; char pad[192]; } __attribute__((packed));

struct osd_blk {                 /* 0x84 bytes, layout from FUN_00122d88 */
    uint32_t cmd;                /* 0x00 */
    uint32_t chn;                /* 0x04  0=main 1=sub */
    uint32_t region;             /* 0x08 */
    uint32_t one;                /* 0x0c  always 1 */
    uint32_t size;               /* 0x10  register: 3 buffers; set: 1 buffer */
    uint16_t x, y, w, h;         /* 0x14..0x1a */
    uint32_t fmt;                /* 0x1c  0=off, 1/2 on */
    uint16_t bpp_code, pad;      /* 0x20  0=1bpp 1=2bpp 2=4bpp */
    uint32_t phys;               /* 0x24 */
    uint32_t virt;               /* 0x28 */
    uint8_t  rest[0x84 - 0x2c];
};
_Static_assert(sizeof(struct osd_blk) == 0x84, "blk");

struct ion_alloc { uint32_t len, align, heap_mask, flags, handle, phys, kaddr; };
struct ion_fd    { uint32_t handle; int fd; };

/* stock default palette (0x5c53c4): A Y U V */
static const uint32_t palette[16] = {
    0x00000000, 0xffff7f7f, 0xff007f7f, 0xff266ac0, 0xff71408a, 0xff4b554a, 0xff599540, 0xff0ec075,
    0xff34aab5, 0xff786085, 0xff2c8aa0, 0xff68d535, 0xff34aa5a, 0xff43e9ab, 0xff4b55a5, 0xff008080 };

static int g_isp;

static int osd_call(struct osd_blk *b, uint32_t cmd, const char *what)
{
    struct isp_cmd_wrapper w;
    memset(&w, 0, sizeof w);
    w.flag = 1; w.inner_cmd = ISP_INNER_OSD; w.payload = b;
    b->cmd = cmd;
    int r = ioctl(g_isp, ISP_IOCTL_CMD, &w);
    printf("%-10s -> %s  (phys=%08x virt=%08x)\n", what, r == 0 ? "ok" : strerror(errno), b->phys, b->virt);
    return r;
}

int main(int argc, char **argv)
{
    int chn = argc > 1 ? atoi(argv[1]) : 0;
    int x = argc > 2 ? atoi(argv[2]) : 64, y = argc > 3 ? atoi(argv[3]) : 64;
    int w = argc > 4 ? atoi(argv[4]) : 512, h = argc > 5 ? atoi(argv[5]) : 128;
    int secs = argc > 6 ? atoi(argv[6]) : 20, fmt = argc > 7 ? atoi(argv[7]) : 2;
    int dis = argc > 8 ? atoi(argv[8]) : 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (chn < 0 || chn > 1 || w < 16 || h < 2 || w > 1536 || h > 512 || (w & 15) || (h & 1) || (y & 1)) {
        printf("bad args (w multiple of 16, h/y even)\n"); return 1;
    }
    uint32_t one = (uint32_t)w * h / 2, total = one * 3;

    int ion = open("/dev/ion", O_RDWR);
    if (ion < 0) { printf("open ion: %s\n", strerror(errno)); return 1; }
    struct ion_alloc a = { total, 0x20, 0xff, 4, 0, 0, 0 };
    if (ioctl(ion, 0xc01c4900, &a) < 0) { printf("ION alloc: %s\n", strerror(errno)); return 1; }
    struct ion_fd sf = { a.handle, -1 };
    if (ioctl(ion, 0xc0084904, &sf) < 0) { printf("ION share: %s\n", strerror(errno)); return 1; }
    uint8_t *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, sf.fd, 0);
    if (mem == MAP_FAILED) { printf("mmap: %s\n", strerror(errno)); return 1; }
    printf("ION: len=%u handle=%u phys=%08x kaddr=%08x user=%p\n", total, a.handle, a.phys, a.kaddr, mem);
    if (!a.phys) { printf("no phys address, abort\n"); return 1; }
    memset(mem, 0, total);

    g_isp = open("/dev/isp-param-0", O_RDWR);
    if (g_isp < 0) { printf("open isp: %s\n", strerror(errno)); return 1; }

    /* palette: block[0]=cmd, 16 colours at +4, rest 0 (selector 0 = OSD table) */
    uint32_t pal[0x84 / 4];
    memset(pal, 0, sizeof pal);
    memcpy(pal + 1, palette, sizeof palette);
    if (osd_call((struct osd_blk *)pal, OSD_PALETTE, "palette")) return 1;

    struct osd_blk b;
    memset(&b, 0, sizeof b);
    b.chn = chn; b.region = 0; b.one = 1; b.size = total;
    b.x = x; b.y = y; b.w = w; b.h = h; b.fmt = fmt; b.bpp_code = 2;
    b.phys = a.phys; b.virt = (uint32_t)(uintptr_t)mem;
    if (osd_call(&b, OSD_REGISTER, "register")) return 1;

    for (int frame = 0; frame < 3; frame++) {
        struct osd_blk g;
        memset(&g, 0, sizeof g);
        g.chn = chn; g.region = 0;
        if (osd_call(&g, OSD_GET_BUF, "get_buf")) break;
        if (g.phys < a.phys || g.phys >= a.phys + total) { printf("buffer outside our ION block, abort\n"); break; }
        uint8_t *buf = mem + (g.phys - a.phys);
        /* 16 vertical colour bars (palette 0..15), each w/16 px wide, with a
         * white 4-px frame; 4bpp, two pixels per byte, row stride w/2 */
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c += 2) {
                int idx = (c * 16) / w;
                if (r < 4 || r >= h - 4 || c < 4 || c >= w - 4) idx = 1;
                buf[r * (w / 2) + c / 2] = (uint8_t)(idx << 4 | idx);
            }
        __builtin___clear_cache((char *)buf, (char *)buf + one);
        msync(mem, total, MS_SYNC);
        b.cmd = 0; b.size = one; b.phys = g.phys; b.virt = g.virt;
        if (osd_call(&b, OSD_SET_BUF, "set_buf")) break;
        usleep(200000);
    }
    printf("showing for %d s\n", secs);
    sleep(secs);

    /* disable; also blank all buffers (index 0 = transparent) in case the
     * disable is not honoured, so nothing visible is left behind */
    memset(mem, 0, total);
    __builtin___clear_cache((char *)mem, (char *)mem + total);
    b.fmt = 0;
    if (dis == 5) { b.size = one; osd_call(&b, OSD_SET_BUF, "off(set)"); }
    else { b.size = total; b.phys = a.phys; b.virt = (uint32_t)(uintptr_t)mem; osd_call(&b, OSD_REGISTER, "off(reg)"); }
    sleep(1);
    printf("done (ION buffer stays allocated until exit)\n");
    sleep(1);
    return 0;
}
