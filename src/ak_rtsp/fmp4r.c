#include "fmp4r.h"

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* one box inside a buffer: header length, total size (both <= n) */
static int box(const uint8_t *p, size_t n, size_t *hl, size_t *sz)
{
    if (n < 8) return -1;
    uint64_t s = be32(p);
    size_t h = 8;
    if (s == 1) {
        if (n < 16) return -1;
        s = be64(p + 8);
        h = 16;
    } else if (s == 0) {
        s = n;                                   /* to the end of the parent */
    }
    if (s < h || s > n) return -1;
    *hl = h;
    *sz = (size_t)s;
    return 0;
}
#define IS(p, t) (!memcmp((p) + 4, (t), 4))

static int read_at(int fd, uint64_t off, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = pread(fd, (uint8_t *)buf + got, n - got, (off_t)(off + got));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
}

static void refresh_size(fmp4r_t *r)
{
    struct stat sb;
    if (fstat(r->fd, &sb) == 0 && sb.st_size > 0) r->fsize = (uint64_t)sb.st_size;
}

/* time in a track's timescale -> microseconds; -1 when absurd (> ~115 days) */
static int64_t to_us(uint64_t t, uint32_t scale)
{
    if (!scale || t / scale > 10000000ull) return -1;
    return (int64_t)(t / scale * 1000000ull + (t % scale) * 1000000ull / scale);
}

/* ---- moov ------------------------------------------------------------------------ */
typedef struct { uint32_t id, dur, size, flags; } trex_t;
typedef struct {
    uint32_t id, scale;
    char handler[4];
    int is_avc, is_pcm, pcm_be;
    uint32_t rate;
    uint8_t sps[256], pps[256];
    uint16_t sl, pl, w, h;
    uint8_t nal_len;
} trak_t;

static void parse_avcc(trak_t *t, const uint8_t *p, size_t n)
{
    if (n < 7) return;
    t->nal_len = (uint8_t)((p[4] & 3) + 1);
    if (t->nal_len == 3) return;                 /* not allowed by the spec */
    size_t o = 6;
    unsigned nsps = p[5] & 31;
    for (unsigned i = 0; i < nsps; i++) {
        if (o + 2 > n) return;
        size_t l = be16(p + o);
        o += 2;
        if (l > n - o) return;
        if (i == 0 && l >= 4 && l <= sizeof t->sps) { memcpy(t->sps, p + o, l); t->sl = (uint16_t)l; }
        o += l;
    }
    if (o + 1 > n) return;
    unsigned npps = p[o++];
    for (unsigned i = 0; i < npps; i++) {
        if (o + 2 > n) return;
        size_t l = be16(p + o);
        o += 2;
        if (l > n - o) return;
        if (i == 0 && l >= 1 && l <= sizeof t->pps) { memcpy(t->pps, p + o, l); t->pl = (uint16_t)l; }
        o += l;
    }
    if (t->sl && t->pl) t->is_avc = 1;
}

static void parse_stsd(trak_t *t, const uint8_t *p, size_t n)
{
    if (n < 8) return;
    size_t o = 8;                                /* version/flags, entry_count: first entry only */
    size_t hl, sz;
    if (box(p + o, n - o, &hl, &sz)) return;
    const uint8_t *e = p + o;
    if ((IS(e, "avc1") || IS(e, "avc3")) && sz >= hl + 78) {
        t->w = (uint16_t)be16(e + hl + 24);      /* visual sample entry width / height */
        t->h = (uint16_t)be16(e + hl + 26);
        for (size_t c = hl + 78; c + 8 <= sz;) {
            size_t chl, csz;
            if (box(e + c, sz - c, &chl, &csz)) break;
            if (IS(e + c, "avcC")) parse_avcc(t, e + c + chl, csz - chl);
            c += csz;
        }
    } else if ((IS(e, "ipcm") || IS(e, "sowt") || IS(e, "twos")) && sz >= hl + 28) {
        const uint8_t *a = e + hl;
        uint32_t ch = be16(a + 16), bits = be16(a + 18), rate = be32(a + 24) >> 16;
        if (ch != 1 || bits != 16 || rate < 8000 || rate > 48000) return;
        t->rate = rate;
        t->pcm_be = IS(e, "twos");
        if (IS(e, "ipcm")) {                     /* byte order in pcmC (ISO/IEC 23003-5) */
            int ok = 0;
            for (size_t c = hl + 28; c + 8 <= sz;) {
                size_t chl, csz;
                if (box(e + c, sz - c, &chl, &csz)) break;
                if (IS(e + c, "pcmC") && csz >= chl + 6 && e[c + chl + 5] == 16) {
                    t->pcm_be = !(e[c + chl + 4] & 1);
                    ok = 1;
                }
                c += csz;
            }
            if (!ok) return;
        }
        t->is_pcm = 1;
    }
}

/* walks trak / mdia / minf / stbl (fixed nesting, no recursion on user data) */
static void parse_trak(trak_t *t, const uint8_t *p, size_t n, int depth)
{
    for (size_t o = 0; o + 8 <= n;) {
        size_t hl, sz;
        if (box(p + o, n - o, &hl, &sz)) return;
        const uint8_t *b = p + o, *c = b + hl;
        size_t cl = sz - hl;
        if (IS(b, "tkhd") && cl >= 24) t->id = be32(c + (c[0] == 1 ? 20 : 12));
        else if (IS(b, "mdhd") && cl >= 24) t->scale = be32(c + (c[0] == 1 ? 20 : 12));
        else if (IS(b, "hdlr") && cl >= 12) memcpy(t->handler, c + 8, 4);
        else if (IS(b, "stsd")) parse_stsd(t, c, cl);
        else if (depth < 3 && (IS(b, "mdia") || IS(b, "minf") || IS(b, "stbl"))) parse_trak(t, c, cl, depth + 1);
        o += sz;
    }
}

static int parse_moov(fmp4r_t *r, const uint8_t *p, size_t n)
{
    trex_t trex[8];
    int ntrex = 0;
    uint32_t mscale = 0;
    uint64_t mvhd_dur = 0, mehd_dur = 0;
    trak_t v = { 0 }, a = { 0 };
    for (size_t o = 0; o + 8 <= n;) {
        size_t hl, sz;
        if (box(p + o, n - o, &hl, &sz)) break;
        const uint8_t *b = p + o, *c = b + hl;
        size_t cl = sz - hl;
        if (IS(b, "mvhd") && cl >= 32) {
            if (c[0] == 1) { mscale = be32(c + 20); mvhd_dur = be64(c + 24); }
            else { mscale = be32(c + 12); mvhd_dur = be32(c + 16); }
        } else if (IS(b, "trak")) {
            trak_t *t = malloc(sizeof *t);
            if (t) {
                memset(t, 0, sizeof *t);
                parse_trak(t, c, cl, 0);
                if (!v.id && t->id && t->scale && !memcmp(t->handler, "vide", 4) && t->is_avc) v = *t;
                else if (!a.id && t->id && t->scale && !memcmp(t->handler, "soun", 4) && t->is_pcm && t->scale == t->rate) a = *t;
                free(t);
            }
        } else if (IS(b, "mvex")) {
            for (size_t q = 0; q + 8 <= cl;) {
                size_t h2, s2;
                if (box(c + q, cl - q, &h2, &s2)) break;
                const uint8_t *d = c + q + h2;
                if (IS(c + q, "mehd") && s2 >= h2 + 8) mehd_dur = d[0] == 1 && s2 >= h2 + 12 ? be64(d + 4) : be32(d + 4);
                else if (IS(c + q, "trex") && s2 >= h2 + 24 && ntrex < 8) {
                    trex[ntrex].id = be32(d + 4);
                    trex[ntrex].dur = be32(d + 12);
                    trex[ntrex].size = be32(d + 16);
                    trex[ntrex].flags = be32(d + 20);
                    ntrex++;
                }
                q += s2;
            }
        }
        o += sz;
    }
    if (!v.id || v.id == a.id) return -1;
    r->vid = v.id; r->vscale = v.scale;
    memcpy(r->sps, v.sps, v.sl); r->sps_len = v.sl;
    memcpy(r->pps, v.pps, v.pl); r->pps_len = v.pl;
    r->nal_len = v.nal_len;
    r->width = v.w; r->height = v.h;
    if (a.id) { r->aid = a.id; r->ascale = a.scale; r->arate = a.rate; r->audio_be = (uint8_t)a.pcm_be; }
    for (int i = 0; i < ntrex; i++) {
        int k = trex[i].id == r->vid ? 0 : (r->aid && trex[i].id == r->aid) ? 1 : -1;
        if (k < 0) continue;
        r->trex_dur[k] = trex[i].dur; r->trex_size[k] = trex[i].size; r->trex_flags[k] = trex[i].flags;
    }
    uint64_t d = mehd_dur ? mehd_dur : mvhd_dur;
    int64_t us = mscale ? to_us(d, mscale) : -1;
    r->duration_us = us > 0 ? us : 0;
    return 0;
}

int fmp4r_open_fd(fmp4r_t *r, int fd)
{
    memset(r, 0, sizeof *r);
    r->fd = fd;
    r->last_idx[0] = r->last_idx[1] = -1;
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) goto fail;
    r->fsize = (uint64_t)sb.st_size;
    int have_moov = 0;
    uint64_t off = 0;
    for (int i = 0; i < 32 && off + 8 <= r->fsize; i++) {
        uint8_t h[16];
        size_t want = r->fsize - off >= 16 ? 16 : 8;
        if (read_at(fd, off, h, want)) goto fail;
        uint64_t sz = be32(h), hl = 8;
        if (sz == 1) { if (want < 16) goto fail; sz = be64(h + 8); hl = 16; }
        else if (sz == 0) sz = r->fsize - off;
        if (sz < hl || sz > r->fsize - off) goto fail;
        if (IS(h, "moov")) {
            if (have_moov || sz - hl > FMP4R_MAX_MOOV) goto fail;
            uint8_t *m = malloc((size_t)(sz - hl));
            if (!m) goto fail;
            int bad = read_at(fd, off + hl, m, (size_t)(sz - hl)) || parse_moov(r, m, (size_t)(sz - hl));
            free(m);
            if (bad) goto fail;
            have_moov = 1;
        } else if (IS(h, "moof")) {
            break;
        }
        off += sz;
    }
    if (!have_moov) goto fail;
    r->first_frag = r->next_frag = off;
    return 0;
fail:
    if (fd >= 0) close(fd);
    memset(r, 0, sizeof *r);
    r->fd = -1;
    return -1;
}

int fmp4r_open(fmp4r_t *r, const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { memset(r, 0, sizeof *r); r->fd = -1; return -1; }
    return fmp4r_open_fd(r, fd);
}

void fmp4r_close(fmp4r_t *r)
{
    if (r->fd >= 0) close(r->fd);
    free(r->s);
    free(r->buf);
    memset(r, 0, sizeof *r);
    r->fd = -1;
}

/* ---- fragments ----------------------------------------------------------------- */
static int add_sample(fmp4r_t *r, size_t *cap, const fmp4r_sample_t *s)
{
    if (r->ns >= FMP4R_MAX_SAMPLES) return -1;
    if (r->ns == *cap) {
        size_t c = *cap ? *cap * 2 : 64;
        if (c > FMP4R_MAX_SAMPLES) c = FMP4R_MAX_SAMPLES;
        fmp4r_sample_t *n = realloc(r->s, c * sizeof *n);
        if (!n) return -1;
        r->s = n;
        *cap = c;
    }
    r->s[r->ns++] = *s;
    return 0;
}

/* 0 ok, 1 incomplete (data past the end of the file), -1 broken */
static int parse_traf(fmp4r_t *r, size_t *cap, const uint8_t *p, size_t n, uint64_t moof_off)
{
    int k = -1;
    uint32_t tf = 0, ddur = 0, dsize = 0, dflags = 0;
    uint64_t base = moof_off, dts = 0;
    uint64_t next_data = 0;
    int first_trun = 1;
    for (size_t o = 0; o + 8 <= n;) {
        size_t hl, sz;
        if (box(p + o, n - o, &hl, &sz)) return -1;
        const uint8_t *b = p + o, *c = b + hl;
        size_t cl = sz - hl;
        o += sz;
        if (IS(b, "tfhd")) {
            if (cl < 8) return -1;
            tf = be24(c + 1);
            uint32_t id = be32(c + 4);
            k = id == r->vid ? 0 : (r->aid && id == r->aid) ? 1 : -1;
            if (k < 0) return 0;                 /* another track: ignored */
            ddur = r->trex_dur[k]; dsize = r->trex_size[k]; dflags = r->trex_flags[k];
            size_t q = 8;
            if (tf & 0x1) { if (q + 8 > cl) return -1; base = be64(c + q); q += 8; }
            if (tf & 0x2) q += 4;
            if (tf & 0x8) { if (q + 4 > cl) return -1; ddur = be32(c + q); q += 4; }
            if (tf & 0x10) { if (q + 4 > cl) return -1; dsize = be32(c + q); q += 4; }
            if (tf & 0x20) { if (q + 4 > cl) return -1; dflags = be32(c + q); q += 4; }
            dts = r->next_dts[k];
        } else if (k >= 0 && IS(b, "tfdt")) {
            if (cl < 8) return -1;
            dts = c[0] == 1 ? (cl >= 12 ? be64(c + 4) : 0) : be32(c + 4);
        } else if (k >= 0 && IS(b, "trun")) {
            if (cl < 8) return -1;
            uint32_t fl = be24(c + 1), cnt = be32(c + 4);
            size_t q = 8;
            int64_t doff = 0;
            int has_doff = 0;
            uint32_t first_flags = 0;
            if (fl & 0x1) { if (q + 4 > cl) return -1; doff = (int32_t)be32(c + q); q += 4; has_doff = 1; }
            if (fl & 0x4) { if (q + 4 > cl) return -1; first_flags = be32(c + q); q += 4; }
            size_t per = (size_t)(!!(fl & 0x100) + !!(fl & 0x200) + !!(fl & 0x400) + !!(fl & 0x800)) * 4;
            if (cnt > FMP4R_MAX_SAMPLES || (per && cnt > (cl - q) / per)) return -1;
            if (!per && cnt > FMP4R_MAX_SAMPLES) return -1;
            int64_t d;
            if (has_doff) d = (int64_t)base + doff;
            else d = first_trun ? (int64_t)base : (int64_t)next_data;
            if (d < 0) return -1;
            uint64_t data = (uint64_t)d;
            first_trun = 0;
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t sd = ddur, ss = dsize, sf = (i == 0 && (fl & 0x4)) ? first_flags : dflags;
                if (fl & 0x100) { sd = be32(c + q); q += 4; }
                if (fl & 0x200) { ss = be32(c + q); q += 4; }
                if (fl & 0x400) { sf = be32(c + q); q += 4; }
                if (fl & 0x800) q += 4;
                uint32_t scale = k ? r->ascale : r->vscale;
                int64_t t = to_us(dts, scale), du = to_us(sd, scale);
                if (t < 0 || du < 0) return -1;
                if (data > r->fsize || ss > r->fsize - data) return 1;     /* still being written, or cut short */
                if (ss) {
                    fmp4r_sample_t s = { data, ss, (uint32_t)(du > 10000000 ? 10000000 : du), t, (uint8_t)k,
                                         (uint8_t)(k ? 1 : !(sf & 0x10000)) };
                    if (add_sample(r, cap, &s)) return -1;
                }
                data += ss;
                dts += sd;
            }
            next_data = data;
        }
    }
    if (k >= 0) r->next_dts[k] = dts;
    return 0;
}

static int cmp_sample(const void *a, const void *b)
{
    const fmp4r_sample_t *x = a, *y = b;
    if (x->t_us != y->t_us) return x->t_us < y->t_us ? -1 : 1;
    if (x->audio != y->audio) return x->audio < y->audio ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}

/* header of the box at off: 0 ok (type, size within the file), -1 none / incomplete */
static int box_hdr(fmp4r_t *r, uint64_t off, uint8_t type[4], uint64_t *size)
{
    uint8_t h[16];
    for (int pass = 0; pass < 2; pass++) {
        if (off + 8 <= r->fsize) {
            size_t want = r->fsize - off >= 16 ? 16 : 8;
            if (read_at(r->fd, off, h, want)) return -1;
            uint64_t sz = be32(h), hl = 8;
            if (sz == 1) { if (want < 16) return -1; sz = be64(h + 8); hl = 16; }
            else if (sz == 0) sz = r->fsize - off;
            if (sz >= hl && sz <= r->fsize - off) { memcpy(type, h + 4, 4); *size = sz; return 0; }
            if (sz < hl) return -1;
        }
        refresh_size(r);                         /* a file being written grows */
    }
    return -1;
}

int fmp4r_next(fmp4r_t *r)
{
    r->ns = 0;
    r->last_idx[0] = r->last_idx[1] = -1;
    if (r->fd < 0) return 0;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t type[4];
        uint64_t sz, off = r->next_frag;
        if (box_hdr(r, off, type, &sz)) return 0;
        if (memcmp(type, "moof", 4) || sz > FMP4R_MAX_MOOF) { r->next_frag = off + sz; continue; }
        uint8_t *nb = realloc(r->buf, (size_t)sz);
        if (!nb) return 0;
        r->buf = nb;
        if (read_at(r->fd, off, r->buf, (size_t)sz)) return 0;
        size_t hl = be32(r->buf) == 1 ? 16 : 8;
        size_t cap = 0;
        free(r->s);                              /* sized to this fragment */
        r->s = NULL;
        uint64_t keep_dts[2] = { r->next_dts[0], r->next_dts[1] };
        int bad = 0;
        for (size_t o = hl; o + 8 <= sz;) {
            size_t bhl, bsz;
            if (box(r->buf + o, (size_t)sz - o, &bhl, &bsz)) break;
            if (IS(r->buf + o, "traf")) {
                int rc = parse_traf(r, &cap, r->buf + o + bhl, bsz - bhl, off);
                if (rc == 1) { r->ns = 0; r->next_dts[0] = keep_dts[0]; r->next_dts[1] = keep_dts[1]; return 0; }
                if (rc < 0) { bad = 1; break; }
            }
            o += bsz;
        }
        r->next_frag = off + sz;
        if (bad) { r->ns = 0; return 0; }        /* broken fragment: the file ends here */
        if (!r->ns) continue;
        qsort(r->s, r->ns, sizeof *r->s, cmp_sample);
        for (size_t i = 0; i < r->ns; i++) r->last_idx[r->s[i].audio] = (int)i;
        return 1;
    }
    return 0;
}

void fmp4r_seek(fmp4r_t *r, int64_t t_us)
{
    uint64_t best = r->first_frag;
    uint64_t dts_best[2] = { 0, 0 };
    r->next_frag = r->first_frag;
    r->next_dts[0] = r->next_dts[1] = 0;
    for (int i = 0; i < 200000; i++) {
        uint64_t at = r->next_frag, d0 = r->next_dts[0], d1 = r->next_dts[1];
        if (fmp4r_next(r) != 1) break;
        int64_t ft = r->s[0].t_us;
        for (size_t j = 0; j < r->ns; j++) if (!r->s[j].audio) { ft = r->s[j].t_us; break; }
        if (ft > t_us) break;
        best = at; dts_best[0] = d0; dts_best[1] = d1;
    }
    r->next_frag = best;
    r->next_dts[0] = dts_best[0]; r->next_dts[1] = dts_best[1];
    r->ns = 0;
    r->last_idx[0] = r->last_idx[1] = -1;
}

int fmp4r_more(fmp4r_t *r)
{
    uint64_t off = r->next_frag;
    for (int i = 0; i < 8; i++) {
        uint8_t type[4];
        uint64_t sz;
        if (box_hdr(r, off, type, &sz)) return 0;
        if (!memcmp(type, "moof", 4)) return sz <= FMP4R_MAX_MOOF;
        off += sz;
    }
    return 0;
}

int64_t fmp4r_scan_end(fmp4r_t *r)
{
    uint64_t keep = r->next_frag, kd0 = r->next_dts[0], kd1 = r->next_dts[1];
    r->next_frag = r->first_frag;
    r->next_dts[0] = r->next_dts[1] = 0;
    int64_t end = 0;
    for (int i = 0; i < 200000 && fmp4r_next(r) == 1; i++)
        for (size_t j = 0; j < r->ns; j++) {
            int64_t e = r->s[j].t_us + r->s[j].dur_us;
            if (!r->s[j].audio && e > end) end = e;
        }
    r->next_frag = keep;
    r->next_dts[0] = kd0; r->next_dts[1] = kd1;
    r->ns = 0;
    r->last_idx[0] = r->last_idx[1] = -1;
    return end;
}

int fmp4r_read(const fmp4r_t *r, const fmp4r_sample_t *s, uint8_t *buf, size_t cap)
{
    if (r->fd < 0 || s->size > cap || s->off > r->fsize || s->size > r->fsize - s->off) return -1;
    if (read_at(r->fd, s->off, buf, s->size)) return -1;
    return (int)s->size;
}
