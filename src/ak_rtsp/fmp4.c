#include "fmp4.h"
#include <stdlib.h>
#include <string.h>

/* ---- growable byte buffer --------------------------------------------------- */
typedef struct { uint8_t *p; size_t n, cap; int err; } buf_t;
static void put(buf_t *b, const void *d, size_t n)
{
    if (b->err) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t *np = realloc(b->p, c);
        if (!np) { b->err = 1; return; }
        b->p = np; b->cap = c;
    }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void u8(buf_t *b, unsigned v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u16(buf_t *b, unsigned v) { uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v }; put(b, x, 2); }
static void u32(buf_t *b, uint32_t v) { uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; put(b, x, 4); }
static void u64(buf_t *b, uint64_t v) { u32(b, (uint32_t)(v >> 32)); u32(b, (uint32_t)v); }
static void zeros(buf_t *b, size_t n) { while (n--) u8(b, 0); }
static size_t box_begin(buf_t *b, const char *type) { size_t at = b->n; u32(b, 0); put(b, type, 4); return at; }
static size_t full_begin(buf_t *b, const char *type, unsigned ver, unsigned flags) { size_t at = box_begin(b, type); u32(b, ver << 24 | flags); return at; }
static void box_end(buf_t *b, size_t at)
{
    if (b->err) return;
    uint32_t sz = (uint32_t)(b->n - at);
    b->p[at] = (uint8_t)(sz >> 24); b->p[at + 1] = (uint8_t)(sz >> 16); b->p[at + 2] = (uint8_t)(sz >> 8); b->p[at + 3] = (uint8_t)sz;
}
static void patch32(buf_t *b, size_t at, uint32_t v) { b->p[at] = (uint8_t)(v >> 24); b->p[at + 1] = (uint8_t)(v >> 16); b->p[at + 2] = (uint8_t)(v >> 8); b->p[at + 3] = (uint8_t)v; }

static const uint32_t MATRIX[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 };

/* ---- NAL helpers ----------------------------------------------------------- */
typedef void (*nal_fn)(void *ctx, const uint8_t *nal, size_t len);
static void each_nal(const uint8_t *d, size_t len, nal_fn fn, void *ctx)
{
    size_t i = 0, start = (size_t)-1;
    while (i + 2 < len) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 3 < len && d[i + 2] == 0 && d[i + 3] == 1))) {
            size_t sc = d[i + 2] == 1 ? 3 : 4;
            if (start != (size_t)-1 && i > start) fn(ctx, d + start, i - start);
            i += sc; start = i; continue;
        }
        i++;
    }
    if (start != (size_t)-1 && start < len) fn(ctx, d + start, len - start);
}

typedef struct { const uint8_t *sps, *pps; size_t sl, pl; } ps_t;
static void find_ps(void *ctx, const uint8_t *n, size_t l)
{
    ps_t *p = ctx;
    int t = n[0] & 31;
    if (t == 7 && !p->sps) { p->sps = n; p->sl = l; }
    if (t == 8 && !p->pps) { p->pps = n; p->pl = l; }
}

/* width/height from the SPS (Exp-Golomb), fallback 1920x1080 */
typedef struct { const uint8_t *d; size_t n, bit; } br_t;
static int rb(br_t *r) { if (r->bit / 8 >= r->n) return 0; int v = (r->d[r->bit / 8] >> (7 - r->bit % 8)) & 1; r->bit++; return v; }
static uint32_t rbits(br_t *r, int k) { uint32_t v = 0; while (k--) v = v << 1 | (uint32_t)rb(r); return v; }
static uint32_t ue(br_t *r) { int z = 0; while (!rb(r) && z < 32) z++; return (1u << z) - 1 + rbits(r, z); }
static int32_t se(br_t *r) { uint32_t v = ue(r); return v & 1 ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2); }
static void sps_size(const uint8_t *sps, size_t len, int *w, int *h)
{
    uint8_t rbsp[256]; size_t n = 0;
    for (size_t i = 1; i < len && n < sizeof rbsp; i++) {
        if (i > 2 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0) continue;
        rbsp[n++] = sps[i];
    }
    br_t r = { rbsp, n, 0 };
    *w = 1920; *h = 1080;
    uint32_t prof = rbits(&r, 8); rbits(&r, 16); ue(&r);
    uint32_t cf = 1;
    if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 || prof == 86 || prof == 118 || prof == 128) {
        cf = ue(&r); if (cf == 3) rb(&r); ue(&r); ue(&r); rb(&r);
        if (rb(&r)) for (int i = 0; i < (cf != 3 ? 8 : 12); i++) if (rb(&r)) { int last = 8, next = 8; for (int j = 0; j < (i < 6 ? 16 : 64); j++) { if (next) next = (last + se(&r) + 256) % 256; last = next ? next : last; } }
    }
    ue(&r);
    uint32_t poc = ue(&r);
    if (poc == 0) ue(&r);
    else if (poc == 1) { rb(&r); se(&r); se(&r); uint32_t c = ue(&r); for (uint32_t i = 0; i < c && i < 256; i++) se(&r); }
    ue(&r); rb(&r);
    uint32_t pw = ue(&r) + 1, ph = ue(&r) + 1;
    int fmo = rb(&r); if (!fmo) rb(&r); rb(&r);
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (rb(&r)) { cl = ue(&r); cr = ue(&r); ct = ue(&r); cb = ue(&r); }
    int sx = (cf == 0 || cf == 3) ? 1 : 2, sy = (cf == 1 ? 2 : 1) * (2 - fmo);
    int ww = (int)(pw * 16) - sx * (int)(cl + cr), hh = (int)((2 - fmo) * ph * 16) - sy * (int)(ct + cb);
    if (ww > 0 && ww <= 4096 && hh > 0 && hh <= 4096) { *w = ww; *h = hh; }
}

/* ---- writer ----------------------------------------------------------------- */
typedef struct { uint32_t size, dur; int key; uint64_t ts; } vs_t;
struct fmp4 {
    FILE *f;
    int audio;
    uint32_t seq;
    uint64_t t0_ms, vdec;          /* first video ts; decode time of the next fragment (90 kHz) */
    buf_t vdata, adata;
    vs_t *vs; size_t nvs, cvs;
    uint32_t *as; size_t nas, cas;  /* audio sample sizes (samples of n frames) */
    uint64_t adec, anext;          /* audio: decode time of fragment start, of next sample (8 kHz) */
    int have_audio_start;
    long base, off_mvhd_dur, off_mehd;   /* header fields patched with the duration at close */
};

static void write_init(fmp4_t *m, const uint8_t *sps, size_t sl, const uint8_t *pps, size_t pl)
{
    int w, h;
    sps_size(sps, sl, &w, &h);
    buf_t b = { 0 };
    size_t a = box_begin(&b, "ftyp"); put(&b, "isom", 4); u32(&b, 0x200); put(&b, "isomiso2avc1mp41iso6", 20); box_end(&b, a);
    size_t moov = box_begin(&b, "moov");
    a = full_begin(&b, "mvhd", 0, 0); u32(&b, 0); u32(&b, 0); u32(&b, 1000); m->off_mvhd_dur = (long)b.n; u32(&b, 0); u32(&b, 0x10000); u16(&b, 0x100); zeros(&b, 10);
    for (int i = 0; i < 9; i++) u32(&b, MATRIX[i]);
    zeros(&b, 24); u32(&b, m->audio ? 3 : 2); box_end(&b, a);
    /* video track */
    size_t trak = box_begin(&b, "trak");
    a = full_begin(&b, "tkhd", 0, 3); u32(&b, 0); u32(&b, 0); u32(&b, 1); u32(&b, 0); u32(&b, 0); zeros(&b, 8); u16(&b, 0); u16(&b, 0); u16(&b, 0); u16(&b, 0);
    for (int i = 0; i < 9; i++) u32(&b, MATRIX[i]);
    u32(&b, (uint32_t)w << 16); u32(&b, (uint32_t)h << 16); box_end(&b, a);
    size_t mdia = box_begin(&b, "mdia");
    a = full_begin(&b, "mdhd", 0, 0); u32(&b, 0); u32(&b, 0); u32(&b, 90000); u32(&b, 0); u16(&b, 0x55c4); u16(&b, 0); box_end(&b, a);
    a = full_begin(&b, "hdlr", 0, 0); u32(&b, 0); put(&b, "vide", 4); zeros(&b, 12); put(&b, "VideoHandler", 13); box_end(&b, a);
    size_t minf = box_begin(&b, "minf");
    a = full_begin(&b, "vmhd", 0, 1); zeros(&b, 8); box_end(&b, a);
    size_t dinf = box_begin(&b, "dinf"); size_t dref = full_begin(&b, "dref", 0, 0); u32(&b, 1); a = full_begin(&b, "url ", 0, 1); box_end(&b, a); box_end(&b, dref); box_end(&b, dinf);
    size_t stbl = box_begin(&b, "stbl");
    size_t stsd = full_begin(&b, "stsd", 0, 0); u32(&b, 1);
    size_t avc1 = box_begin(&b, "avc1"); zeros(&b, 6); u16(&b, 1); zeros(&b, 16); u16(&b, (unsigned)w); u16(&b, (unsigned)h);
    u32(&b, 0x480000); u32(&b, 0x480000); u32(&b, 0); u16(&b, 1); zeros(&b, 32); u16(&b, 0x18); u16(&b, 0xffff);
    size_t avcc = box_begin(&b, "avcC"); u8(&b, 1); u8(&b, sps[1]); u8(&b, sps[2]); u8(&b, sps[3]); u8(&b, 0xff); u8(&b, 0xe1);
    u16(&b, (unsigned)sl); put(&b, sps, sl); u8(&b, 1); u16(&b, (unsigned)pl); put(&b, pps, pl); box_end(&b, avcc);
    box_end(&b, avc1); box_end(&b, stsd);
    a = full_begin(&b, "stts", 0, 0); u32(&b, 0); box_end(&b, a);
    a = full_begin(&b, "stsc", 0, 0); u32(&b, 0); box_end(&b, a);
    a = full_begin(&b, "stsz", 0, 0); u32(&b, 0); u32(&b, 0); box_end(&b, a);
    a = full_begin(&b, "stco", 0, 0); u32(&b, 0); box_end(&b, a);
    box_end(&b, stbl); box_end(&b, minf); box_end(&b, mdia); box_end(&b, trak);
    if (m->audio) {   /* audio track: ISO/IEC 23003-5 'ipcm', 16-bit little endian, 8 kHz mono */
        trak = box_begin(&b, "trak");
        a = full_begin(&b, "tkhd", 0, 3); u32(&b, 0); u32(&b, 0); u32(&b, 2); u32(&b, 0); u32(&b, 0); zeros(&b, 8); u16(&b, 0); u16(&b, 1); u16(&b, 0x100); u16(&b, 0);
        for (int i = 0; i < 9; i++) u32(&b, MATRIX[i]);
        u32(&b, 0); u32(&b, 0); box_end(&b, a);
        mdia = box_begin(&b, "mdia");
        a = full_begin(&b, "mdhd", 0, 0); u32(&b, 0); u32(&b, 0); u32(&b, 8000); u32(&b, 0); u16(&b, 0x55c4); u16(&b, 0); box_end(&b, a);
        a = full_begin(&b, "hdlr", 0, 0); u32(&b, 0); put(&b, "soun", 4); zeros(&b, 12); put(&b, "SoundHandler", 13); box_end(&b, a);
        minf = box_begin(&b, "minf");
        a = full_begin(&b, "smhd", 0, 0); u32(&b, 0); box_end(&b, a);
        dinf = box_begin(&b, "dinf"); dref = full_begin(&b, "dref", 0, 0); u32(&b, 1); a = full_begin(&b, "url ", 0, 1); box_end(&b, a); box_end(&b, dref); box_end(&b, dinf);
        stbl = box_begin(&b, "stbl");
        stsd = full_begin(&b, "stsd", 0, 0); u32(&b, 1);
        size_t ip = box_begin(&b, "ipcm"); zeros(&b, 6); u16(&b, 1); zeros(&b, 8); u16(&b, 1); u16(&b, 16); u16(&b, 0); u16(&b, 0); u32(&b, 8000u << 16);
        a = full_begin(&b, "pcmC", 0, 0); u8(&b, 1); u8(&b, 16); box_end(&b, a);          /* format_flags 1 = little endian */
        box_end(&b, ip); box_end(&b, stsd);
        a = full_begin(&b, "stts", 0, 0); u32(&b, 0); box_end(&b, a);
        a = full_begin(&b, "stsc", 0, 0); u32(&b, 0); box_end(&b, a);
        a = full_begin(&b, "stsz", 0, 0); u32(&b, 0); u32(&b, 0); box_end(&b, a);
        a = full_begin(&b, "stco", 0, 0); u32(&b, 0); box_end(&b, a);
        box_end(&b, stbl); box_end(&b, minf); box_end(&b, mdia); box_end(&b, trak);
    }
    size_t mvex = box_begin(&b, "mvex");
    a = full_begin(&b, "mehd", 1, 0); m->off_mehd = (long)b.n; u64(&b, 0); box_end(&b, a);   /* duration (ms) */
    a = full_begin(&b, "trex", 0, 0); u32(&b, 1); u32(&b, 1); u32(&b, 0); u32(&b, 0); u32(&b, 0); box_end(&b, a);
    if (m->audio) { a = full_begin(&b, "trex", 0, 0); u32(&b, 2); u32(&b, 1); u32(&b, 0); u32(&b, 0); u32(&b, 0); box_end(&b, a); }
    box_end(&b, mvex);
    box_end(&b, moov);
    m->base = ftell(m->f);
    if (!b.err) fwrite(b.p, 1, b.n, m->f);
    free(b.p);
}

fmp4_t *fmp4_open(FILE *f, const uint8_t *key, size_t len, int with_audio)
{
    ps_t ps = { 0 };
    each_nal(key, len, find_ps, &ps);
    if (!ps.sps || !ps.pps || ps.sl < 4) return NULL;
    fmp4_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->f = f; m->audio = with_audio; m->t0_ms = (uint64_t)-1;
    write_init(m, ps.sps, ps.sl, ps.pps, ps.pl);
    return m;
}

uint64_t fmp4_video_start_ms(const fmp4_t *m) { return m->t0_ms; }

static void add_nal(void *ctx, const uint8_t *n, size_t l)
{
    buf_t *b = ctx;
    int t = n[0] & 31;
    if (t == 7 || t == 8 || t == 9) return;          /* parameter sets live in avcC; drop AUDs */
    u32(b, (uint32_t)l); put(b, n, l);
}

int fmp4_video(fmp4_t *m, const uint8_t *d, size_t len, int key, uint64_t ts)
{
    if (m->t0_ms == (uint64_t)-1) m->t0_ms = ts;
    size_t before = m->vdata.n;
    each_nal(d, len, add_nal, &m->vdata);
    if (m->vdata.err) return -1;
    if (m->nvs == m->cvs) {
        size_t c = m->cvs ? m->cvs * 2 : 64;
        vs_t *nv = realloc(m->vs, c * sizeof *nv);
        if (!nv) return -1;
        m->vs = nv; m->cvs = c;
    }
    vs_t *s = &m->vs[m->nvs++];
    s->size = (uint32_t)(m->vdata.n - before); s->key = key; s->ts = ts; s->dur = 0;
    if (m->nvs > 1) {
        vs_t *p = &m->vs[m->nvs - 2];
        uint64_t d90 = (ts > p->ts ? ts - p->ts : 66) * 90;
        p->dur = (uint32_t)(d90 > 90000 ? 6000 : d90);
    }
    return 0;
}

int fmp4_audio(fmp4_t *m, const int16_t *pcm, size_t n, uint64_t t8)
{
    if (!m->audio || !n) return 0;
    if (!m->have_audio_start) { m->adec = m->anext = t8; m->have_audio_start = 1; }
    if (t8 + 4000 < m->anext) return 0;             /* far in the past: drop */
    if (m->nas == m->cas) {
        size_t c = m->cas ? m->cas * 2 : 128;
        uint32_t *na = realloc(m->as, c * sizeof *na);
        if (!na) return -1;
        m->as = na; m->cas = c;
    }
    m->as[m->nas++] = (uint32_t)n;
    put(&m->adata, pcm, n * 2);                     /* little endian, as captured */
    m->anext += n;
    return m->adata.err ? -1 : 0;
}

int fmp4_flush(fmp4_t *m)
{
    if (!m->nvs && !m->nas) return 0;
    /* last video sample duration: repeat the previous one */
    if (m->nvs) { vs_t *l = &m->vs[m->nvs - 1]; if (!l->dur) l->dur = m->nvs > 1 ? m->vs[m->nvs - 2].dur : 6000; if (!l->dur) l->dur = 6000; }
    buf_t b = { 0 };
    size_t moof = box_begin(&b, "moof");
    size_t a = full_begin(&b, "mfhd", 0, 0); u32(&b, ++m->seq); box_end(&b, a);
    size_t voff_at = 0, aoff_at = 0;
    if (m->nvs) {
        size_t traf = box_begin(&b, "traf");
        a = full_begin(&b, "tfhd", 0, 0x020000); u32(&b, 1); box_end(&b, a);
        a = full_begin(&b, "tfdt", 1, 0); u64(&b, m->vdec); box_end(&b, a);
        a = full_begin(&b, "trun", 0, 0x000701); u32(&b, (uint32_t)m->nvs); voff_at = b.n; u32(&b, 0);
        for (size_t i = 0; i < m->nvs; i++) { u32(&b, m->vs[i].dur); u32(&b, m->vs[i].size); u32(&b, m->vs[i].key ? 0x02000000 : 0x01010000); }
        box_end(&b, a); box_end(&b, traf);
    }
    if (m->nas) {
        size_t traf = box_begin(&b, "traf");
        a = full_begin(&b, "tfhd", 0, 0x020000); u32(&b, 2); box_end(&b, a);
        a = full_begin(&b, "tfdt", 1, 0); u64(&b, m->adec); box_end(&b, a);
        a = full_begin(&b, "trun", 0, 0x000301); u32(&b, (uint32_t)m->nas); aoff_at = b.n; u32(&b, 0);
        for (size_t i = 0; i < m->nas; i++) { u32(&b, m->as[i]); u32(&b, m->as[i] * 2); }
        box_end(&b, a); box_end(&b, traf);
    }
    box_end(&b, moof);
    size_t moof_len = b.n;
    if (voff_at) patch32(&b, voff_at, (uint32_t)(moof_len + 8));
    if (aoff_at) patch32(&b, aoff_at, (uint32_t)(moof_len + 8 + m->vdata.n));
    u32(&b, (uint32_t)(8 + m->vdata.n + m->adata.n)); put(&b, "mdat", 4);
    int ok = !b.err && fwrite(b.p, 1, b.n, m->f) == b.n;
    if (ok && m->vdata.n) ok = fwrite(m->vdata.p, 1, m->vdata.n, m->f) == m->vdata.n;
    if (ok && m->adata.n) ok = fwrite(m->adata.p, 1, m->adata.n, m->f) == m->adata.n;
    free(b.p);
    for (size_t i = 0; i < m->nvs; i++) m->vdec += m->vs[i].dur;
    m->adec = m->anext;
    m->nvs = 0; m->nas = 0; m->vdata.n = 0; m->adata.n = 0;
    return ok ? 0 : -1;
}

void fmp4_close(fmp4_t *m)
{
    if (!m) return;
    fmp4_flush(m);
    /* now the length is known: fill in mvhd/mehd so players show it */
    uint64_t ms = m->vdec / 90;
    long end = ftell(m->f);
    if (end > 0 && m->base >= 0) {
        uint8_t d4[4] = { (uint8_t)(ms >> 24), (uint8_t)(ms >> 16), (uint8_t)(ms >> 8), (uint8_t)ms };
        uint8_t d8[8] = { 0, 0, 0, 0, d4[0], d4[1], d4[2], d4[3] };
        if (fseek(m->f, m->base + m->off_mvhd_dur, SEEK_SET) == 0) fwrite(d4, 1, 4, m->f);
        if (fseek(m->f, m->base + m->off_mehd, SEEK_SET) == 0) fwrite(d8, 1, 8, m->f);
        fseek(m->f, end, SEEK_SET);
        fflush(m->f);
    }
    free(m->vdata.p); free(m->adata.p); free(m->vs); free(m->as);
    free(m);
}
