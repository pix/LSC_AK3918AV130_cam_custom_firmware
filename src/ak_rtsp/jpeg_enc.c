/* Baseline JPEG encoder — see jpeg_enc.h.
 * DCT: IJG "islow" (Loeffler-Ligtenberg-Moschytz) fixed-point algorithm,
 * output scaled by 8, compensated during quantization. */
#include "jpeg_enc.h"
#include <string.h>

static const uint8_t zigzag[64] = {  /* zigzag index -> natural index */
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t std_lum_q[64] = {  /* natural order */
    16, 11, 10, 16, 24, 40, 51, 61,   12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,   14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,   24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,   72, 92, 95, 98,112,100,103, 99,
};
static const uint8_t std_chr_q[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,   18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,   47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,   99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,   99, 99, 99, 99, 99, 99, 99, 99,
};

static const uint8_t dc_lum_bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t dc_chr_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t dc_vals[12]     = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t ac_lum_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t ac_lum_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa,
};
static const uint8_t ac_chr_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t ac_chr_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa,
};

typedef struct { uint16_t code[256]; uint8_t size[256]; } huff_t;

typedef struct {
    uint8_t *out;
    int      cap, len, overflow;
    uint32_t acc;   /* bit accumulator, MSB-first */
    int      nacc;  /* bits in acc */
} bw_t;

static void build_huff(huff_t *h, const uint8_t bits[16], const uint8_t *vals)
{
    memset(h, 0, sizeof(*h));
    unsigned code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++) {
            h->code[vals[k]] = (uint16_t)code;
            h->size[vals[k]] = (uint8_t)len;
            code++; k++;
        }
        code <<= 1;
    }
}

static void put_byte(bw_t *b, uint8_t v)
{
    if (b->len >= b->cap) { b->overflow = 1; return; }
    b->out[b->len++] = v;
}

static void put_bytes(bw_t *b, const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) put_byte(b, p[i]);
}

static void put_u16(bw_t *b, unsigned v)
{
    put_byte(b, (uint8_t)(v >> 8));
    put_byte(b, (uint8_t)v);
}

/* Entropy-coded bits with 0xFF byte stuffing. nbits <= 16. */
static void put_bits(bw_t *b, unsigned value, int nbits)
{
    b->acc = (b->acc << nbits) | (value & ((1u << nbits) - 1));
    b->nacc += nbits;
    while (b->nacc >= 8) {
        uint8_t byte = (uint8_t)(b->acc >> (b->nacc - 8));
        put_byte(b, byte);
        if (byte == 0xFF) put_byte(b, 0x00);
        b->nacc -= 8;
    }
}

static void flush_bits(bw_t *b)
{
    if (b->nacc > 0) put_bits(b, 0x7F, 8 - b->nacc);  /* pad with 1s */
    b->acc = 0; b->nacc = 0;
}

/* ---- Forward DCT (IJG jfdctint.c, islow) --------------------------------- */
#define CONST_BITS 13
#define PASS1_BITS 2
#define DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))
#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172

static void fdct_islow(int32_t *d)
{
    int32_t t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13, z1, z2, z3, z4, z5;
    int32_t *p;

    for (p = d; p < d + 64; p += 8) {  /* rows */
        t0 = p[0] + p[7]; t7 = p[0] - p[7];
        t1 = p[1] + p[6]; t6 = p[1] - p[6];
        t2 = p[2] + p[5]; t5 = p[2] - p[5];
        t3 = p[3] + p[4]; t4 = p[3] - p[4];

        t10 = t0 + t3; t13 = t0 - t3;
        t11 = t1 + t2; t12 = t1 - t2;

        p[0] = (t10 + t11) * (1 << PASS1_BITS);
        p[4] = (t10 - t11) * (1 << PASS1_BITS);
        z1 = (t12 + t13) * FIX_0_541196100;
        p[2] = DESCALE(z1 + t13 * FIX_0_765366865, CONST_BITS - PASS1_BITS);
        p[6] = DESCALE(z1 - t12 * FIX_1_847759065, CONST_BITS - PASS1_BITS);

        z1 = t4 + t7; z2 = t5 + t6; z3 = t4 + t6; z4 = t5 + t7;
        z5 = (z3 + z4) * FIX_1_175875602;
        t4 *= FIX_0_298631336; t5 *= FIX_2_053119869;
        t6 *= FIX_3_072711026; t7 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;

        p[7] = DESCALE(t4 + z1 + z3, CONST_BITS - PASS1_BITS);
        p[5] = DESCALE(t5 + z2 + z4, CONST_BITS - PASS1_BITS);
        p[3] = DESCALE(t6 + z2 + z3, CONST_BITS - PASS1_BITS);
        p[1] = DESCALE(t7 + z1 + z4, CONST_BITS - PASS1_BITS);
    }

    for (p = d; p < d + 8; p++) {  /* columns */
        t0 = p[0]  + p[56]; t7 = p[0]  - p[56];
        t1 = p[8]  + p[48]; t6 = p[8]  - p[48];
        t2 = p[16] + p[40]; t5 = p[16] - p[40];
        t3 = p[24] + p[32]; t4 = p[24] - p[32];

        t10 = t0 + t3; t13 = t0 - t3;
        t11 = t1 + t2; t12 = t1 - t2;

        p[0]  = DESCALE(t10 + t11, PASS1_BITS);
        p[32] = DESCALE(t10 - t11, PASS1_BITS);
        z1 = (t12 + t13) * FIX_0_541196100;
        p[16] = DESCALE(z1 + t13 * FIX_0_765366865, CONST_BITS + PASS1_BITS);
        p[48] = DESCALE(z1 - t12 * FIX_1_847759065, CONST_BITS + PASS1_BITS);

        z1 = t4 + t7; z2 = t5 + t6; z3 = t4 + t6; z4 = t5 + t7;
        z5 = (z3 + z4) * FIX_1_175875602;
        t4 *= FIX_0_298631336; t5 *= FIX_2_053119869;
        t6 *= FIX_3_072711026; t7 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;

        p[56] = DESCALE(t4 + z1 + z3, CONST_BITS + PASS1_BITS);
        p[40] = DESCALE(t5 + z2 + z4, CONST_BITS + PASS1_BITS);
        p[24] = DESCALE(t6 + z2 + z3, CONST_BITS + PASS1_BITS);
        p[8]  = DESCALE(t7 + z1 + z4, CONST_BITS + PASS1_BITS);
    }
}

/* ---- Block encoding ------------------------------------------------------- */
static int bit_length(unsigned v)
{
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

/* blk: 64 level-shifted samples (natural order). qt: quant table (natural
 * order, already clamped 1..255). Updates *pred (previous DC). */
static void encode_block(bw_t *b, int32_t *blk, const uint8_t *qt, int *pred,
                         const huff_t *dc, const huff_t *ac)
{
    int q[64];

    fdct_islow(blk);
    for (int i = 0; i < 64; i++) {
        int n = zigzag[i];
        int32_t div = (int32_t)qt[n] * 8;   /* DCT output is scaled by 8 */
        int32_t c = blk[n];
        q[i] = c >= 0 ? (int)((c + div / 2) / div) : -(int)((-c + div / 2) / div);
    }

    int diff = q[0] - *pred;
    *pred = q[0];
    unsigned mag = (unsigned)(diff < 0 ? -diff : diff);
    int nb = bit_length(mag);
    put_bits(b, dc->code[nb], dc->size[nb]);
    if (nb) put_bits(b, (unsigned)(diff < 0 ? diff - 1 : diff), nb);

    int run = 0;
    for (int i = 1; i < 64; i++) {
        int v = q[i];
        if (v == 0) { run++; continue; }
        while (run > 15) {
            put_bits(b, ac->code[0xF0], ac->size[0xF0]);  /* ZRL */
            run -= 16;
        }
        mag = (unsigned)(v < 0 ? -v : v);
        nb = bit_length(mag);
        int sym = (run << 4) | nb;
        put_bits(b, ac->code[sym], ac->size[sym]);
        put_bits(b, (unsigned)(v < 0 ? v - 1 : v), nb);
        run = 0;
    }
    if (run > 0) put_bits(b, ac->code[0x00], ac->size[0x00]);  /* EOB */
}

static void scale_qtable(uint8_t *dst, const uint8_t *src, int quality)
{
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) {
        int v = (src[i] * scale + 50) / 100;
        dst[i] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
    }
}

static void put_dht(bw_t *b, int cls_id, const uint8_t bits[16], const uint8_t *vals, int nvals)
{
    put_byte(b, (uint8_t)cls_id);
    put_bytes(b, bits, 16);
    put_bytes(b, vals, nvals);
}

int jpeg_encode_nv12(const uint8_t *nv12, int w, int h, int quality,
                     uint8_t *out, int out_cap)
{
    if (!nv12 || !out || out_cap <= 0 || w <= 0 || h <= 0 ||
        (w & 1) || (h & 1) || w > 65535 || h > 65535)
        return -1;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    uint8_t qlum[64], qchr[64];
    scale_qtable(qlum, std_lum_q, quality);
    scale_qtable(qchr, std_chr_q, quality);

    huff_t dcl, acl, dcc, acc;
    build_huff(&dcl, dc_lum_bits, dc_vals);
    build_huff(&acl, ac_lum_bits, ac_lum_vals);
    build_huff(&dcc, dc_chr_bits, dc_vals);
    build_huff(&acc, ac_chr_bits, ac_chr_vals);

    bw_t b = { out, out_cap, 0, 0, 0, 0 };

    static const uint8_t soi_app0[] = {
        0xFF, 0xD8,
        0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01,
        0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    };
    put_bytes(&b, soi_app0, sizeof(soi_app0));

    /* DQT: two 8-bit tables, zigzag order */
    put_u16(&b, 0xFFDB); put_u16(&b, 2 + 2 * 65);
    put_byte(&b, 0x00);
    for (int i = 0; i < 64; i++) put_byte(&b, qlum[zigzag[i]]);
    put_byte(&b, 0x01);
    for (int i = 0; i < 64; i++) put_byte(&b, qchr[zigzag[i]]);

    /* SOF0: 3 components, Y 2x2, Cb/Cr 1x1 */
    put_u16(&b, 0xFFC0); put_u16(&b, 17);
    put_byte(&b, 8); put_u16(&b, (unsigned)h); put_u16(&b, (unsigned)w);
    put_byte(&b, 3);
    put_byte(&b, 1); put_byte(&b, 0x22); put_byte(&b, 0);
    put_byte(&b, 2); put_byte(&b, 0x11); put_byte(&b, 1);
    put_byte(&b, 3); put_byte(&b, 0x11); put_byte(&b, 1);

    /* DHT: all four standard tables in one segment */
    put_u16(&b, 0xFFC4);
    put_u16(&b, 2 + (17 + 12) * 2 + (17 + 162) * 2);
    put_dht(&b, 0x00, dc_lum_bits, dc_vals, 12);
    put_dht(&b, 0x10, ac_lum_bits, ac_lum_vals, 162);
    put_dht(&b, 0x01, dc_chr_bits, dc_vals, 12);
    put_dht(&b, 0x11, ac_chr_bits, ac_chr_vals, 162);

    /* SOS */
    static const uint8_t sos[] = {
        0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11,
        0x00, 0x3F, 0x00,
    };
    put_bytes(&b, sos, sizeof(sos));

    const uint8_t *yp = nv12;
    const uint8_t *uv = nv12 + (size_t)w * h;
    int pred_y = 0, pred_cb = 0, pred_cr = 0;
    int32_t blk[64];
    const int cx_max = w / 2 - 1, cy_max = h / 2 - 1;

    for (int my = 0; my < h; my += 16) {
        for (int mx = 0; mx < w; mx += 16) {
            for (int by = 0; by < 16; by += 8) {
                for (int bx = 0; bx < 16; bx += 8) {
                    /* Edge MCUs: replicate the last row/column (clamp). */
                    for (int y = 0; y < 8; y++) {
                        int sy = my + by + y; if (sy > h - 1) sy = h - 1;
                        const uint8_t *row = yp + (size_t)sy * w;
                        for (int x = 0; x < 8; x++) {
                            int sx = mx + bx + x; if (sx > w - 1) sx = w - 1;
                            blk[y * 8 + x] = (int32_t)row[sx] - 128;
                        }
                    }
                    encode_block(&b, blk, qlum, &pred_y, &dcl, &acl);
                }
            }
            for (int comp = 0; comp < 2; comp++) {
                for (int y = 0; y < 8; y++) {
                    int sy = my / 2 + y; if (sy > cy_max) sy = cy_max;
                    const uint8_t *row = uv + (size_t)sy * w + comp;
                    for (int x = 0; x < 8; x++) {
                        int sx = mx / 2 + x; if (sx > cx_max) sx = cx_max;
                        blk[y * 8 + x] = (int32_t)row[sx * 2] - 128;
                    }
                }
                if (comp == 0) encode_block(&b, blk, qchr, &pred_cb, &dcc, &acc);
                else           encode_block(&b, blk, qchr, &pred_cr, &dcc, &acc);
            }
            if (b.overflow) return -1;
        }
    }

    flush_bits(&b);
    put_u16(&b, 0xFFD9);
    return b.overflow ? -1 : b.len;
}
