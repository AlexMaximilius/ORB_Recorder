/*
 * png_write.h -- single-header PNG writer.
 *
 * Screenshots want PNG: lossless, universally pasteable, and it handles a
 * screenful of flat UI colour far better than GIF's 256-entry palette.
 *
 * This used to write UNCOMPRESSED deflate blocks, which is valid PNG that
 * every viewer opens, and which cost roughly ten times the bytes. The comment
 * here said "if the size ever matters, this is the place to add fixed-Huffman
 * blocks". It started to matter: a 640x460 capture came out at 1.18 MB --
 * exactly four bytes per pixel -- and these files exist to be pasted into
 * chat windows.
 *
 * So there are now three stages, each of which earns its place:
 *
 *   1. Drop the alpha channel when every pixel is opaque. Screen captures
 *      always are, and colour type 2 is a quarter smaller before a single
 *      byte is compressed.
 *   2. Filter each row adaptively -- None/Sub/Up/Average/Paeth, chosen by
 *      minimum sum of absolute differences. This is where most of the win on
 *      a screenshot comes from: a run of identical pixels filters to zeroes,
 *      and zeroes compress to almost nothing.
 *   3. LZ77 with a 32K window and lazy matching, emitted as fixed-Huffman
 *      blocks. Fixed rather than dynamic: dynamic Huffman would gain maybe
 *      another 15% and costs tree construction, package-merge and a tree
 *      encoder, all of which can be subtly wrong. We learned that the
 *      expensive way with the GIF LZW encoder, which produced files that
 *      decoded cleanly for two frames and then fell apart.
 *
 * If the compressed form would not beat the raw bytes -- possible on noise --
 * the stored-block path is still here and is used instead. The writer can
 * therefore never make a file bigger than it used to be.
 *
 * Public domain.
 */

#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* rgba is w*h*4, top-down. Returns 1 on success. */
int png_write_rgba(const char* path, const uint8_t* rgba, int w, int h);

#endif /* PNG_WRITE_H */

#ifdef PNG_WRITE_IMPLEMENTATION

static uint32_t png_crc_table_[256];
static int      png_crc_ready_ = 0;

static void png_crc_init_(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        png_crc_table_[n] = c;
    }
    png_crc_ready_ = 1;
}

static uint32_t png_crc_(const uint8_t* buf, size_t len, uint32_t crc) {
    if (!png_crc_ready_) png_crc_init_();
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = png_crc_table_[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void png_be32_(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void png_chunk_(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
    uint8_t hdr[8];
    png_be32_(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);

    uint32_t crc = png_crc_((const uint8_t*)type, 4, 0);
    if (len) crc = png_crc_(data, len, crc);
    uint8_t c[4];
    png_be32_(c, crc);
    fwrite(c, 1, 4, f);
}

/* ---- bit output -------------------------------------------------------
 * Deflate packs its own fields least-significant-bit first, but Huffman
 * codes most-significant-bit first. Getting that backwards is the classic
 * way to produce a stream that decodes for a while and then desynchronises,
 * so the two cases are separate functions with the difference in the name. */
typedef struct {
    uint8_t* buf;
    size_t   cap, pos;
    uint32_t acc;
    int      nbits;
    int      ok;
} PngBits;

static void png_bits_raw_(PngBits* b, uint32_t val, int n) {   /* LSB first */
    if (!b->ok) return;
    b->acc |= (val & ((1u << n) - 1u)) << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) {
        if (b->pos >= b->cap) { b->ok = 0; return; }
        b->buf[b->pos++] = (uint8_t)(b->acc & 0xFF);
        b->acc >>= 8;
        b->nbits -= 8;
    }
}

static void png_bits_huff_(PngBits* b, uint32_t code, int n) { /* MSB first */
    for (int i = n - 1; i >= 0; i--) png_bits_raw_(b, (code >> i) & 1u, 1);
}

static void png_bits_flush_(PngBits* b) {
    if (b->nbits > 0) png_bits_raw_(b, 0, 8 - b->nbits);
}

/* Fixed Huffman literal/length code, RFC 1951 section 3.2.6. */
static void png_lit_(PngBits* b, int sym) {
    if      (sym <= 143) png_bits_huff_(b, 0x30u  + (uint32_t)sym,         8);
    else if (sym <= 255) png_bits_huff_(b, 0x190u + (uint32_t)(sym - 144), 9);
    else if (sym <= 279) png_bits_huff_(b, 0x0u   + (uint32_t)(sym - 256), 7);
    else                 png_bits_huff_(b, 0xC0u  + (uint32_t)(sym - 280), 8);
}

static const uint16_t PNG_LEN_BASE_[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258
};
static const uint8_t PNG_LEN_EXTRA_[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t PNG_DIST_BASE_[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t PNG_DIST_EXTRA_[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static void png_match_(PngBits* b, int len, int dist) {
    int lc = 28;
    while (lc > 0 && PNG_LEN_BASE_[lc] > len) lc--;
    png_lit_(b, 257 + lc);
    if (PNG_LEN_EXTRA_[lc])
        png_bits_raw_(b, (uint32_t)(len - PNG_LEN_BASE_[lc]), PNG_LEN_EXTRA_[lc]);

    int dc = 29;
    while (dc > 0 && PNG_DIST_BASE_[dc] > dist) dc--;
    png_bits_huff_(b, (uint32_t)dc, 5);            /* fixed 5-bit distance */
    if (PNG_DIST_EXTRA_[dc])
        png_bits_raw_(b, (uint32_t)(dist - PNG_DIST_BASE_[dc]), PNG_DIST_EXTRA_[dc]);
}

/* ---- LZ77 -------------------------------------------------------------
 * Hash chains over a 32K window. The chain walk is capped: past a hundred or
 * so candidates the extra bytes saved stop paying for the time, and a
 * screenshot has long runs that would otherwise make every chain enormous. */
#define PNG_HBITS   15
#define PNG_HSIZE   (1 << PNG_HBITS)
#define PNG_WINDOW  32768
#define PNG_MINMAT  3
#define PNG_MAXMAT  258
#define PNG_CHAIN   32

static uint32_t png_hash_(const uint8_t* p) {
    return (uint32_t)(((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (PNG_HSIZE - 1));
}

/* Longest match for `pos`, or 0. Sets *out_dist. */
static int png_find_(const uint8_t* d, size_t len, size_t pos,
                     const int32_t* head, const int32_t* prev,
                     int* out_dist, int max_len) {
    if (pos + PNG_MINMAT > len) return 0;
    if (max_len > (int)(len - pos)) max_len = (int)(len - pos);
    if (max_len < PNG_MINMAT) return 0;

    int best = 0, best_dist = 0, chain = PNG_CHAIN;
    int32_t cand = head[png_hash_(d + pos)];
    while (cand >= 0 && chain--) {
        size_t dist = pos - (size_t)cand;
        if (dist == 0 || dist > PNG_WINDOW) break;
        const uint8_t* a = d + pos;
        const uint8_t* b = d + cand;
        if (a[best] == b[best] && a[0] == b[0] && a[1] == b[1]) {
            int n = 0;
            while (n < max_len && a[n] == b[n]) n++;
            if (n > best) {
                best = n; best_dist = (int)dist;
                if (best >= max_len) break;
            }
        }
        cand = prev[(size_t)cand & (PNG_WINDOW - 1)];
    }
    if (best < PNG_MINMAT) return 0;
    *out_dist = best_dist;
    return best;
}

/* Compress `d` into `out` (capacity `cap`) as one fixed-Huffman block.
 * Returns the byte count, or 0 if it did not fit -- which the caller treats
 * as "not worth compressing" and falls back to stored blocks. */
static size_t png_deflate_(const uint8_t* d, size_t len,
                           uint8_t* out, size_t cap) {
    int32_t* head = (int32_t*)malloc(sizeof(int32_t) * PNG_HSIZE);
    int32_t* prev = (int32_t*)malloc(sizeof(int32_t) * PNG_WINDOW);
    if (!head || !prev) { free(head); free(prev); return 0; }
    for (int i = 0; i < PNG_HSIZE; i++)  head[i] = -1;
    for (int i = 0; i < PNG_WINDOW; i++) prev[i] = -1;

    PngBits b;
    b.buf = out; b.cap = cap; b.pos = 0; b.acc = 0; b.nbits = 0; b.ok = 1;
    png_bits_raw_(&b, 1, 1);      /* BFINAL = 1 */
    png_bits_raw_(&b, 1, 2);      /* BTYPE  = 01, fixed Huffman */

    size_t pos = 0;
    while (pos < len && b.ok) {
        int dist = 0;
        int mlen = png_find_(d, len, pos, head, prev, &dist, PNG_MAXMAT);

        /* Lazy matching: if the NEXT byte starts a longer match, emit this
         * one as a literal and take the better match. Cheap, and worth a few
         * percent on text and UI edges. */
        if (mlen >= PNG_MINMAT && mlen < PNG_MAXMAT && pos + 1 < len) {
            int ndist = 0;
            int nlen = png_find_(d, len, pos + 1, head, prev, &ndist, PNG_MAXMAT);
            if (nlen > mlen) mlen = 0;
        }

        if (mlen >= PNG_MINMAT) {
            png_match_(&b, mlen, dist);
            for (int i = 0; i < mlen; i++) {
                if (pos + PNG_MINMAT <= len) {
                    uint32_t hh = png_hash_(d + pos);
                    prev[pos & (PNG_WINDOW - 1)] = head[hh];
                    head[hh] = (int32_t)pos;
                }
                pos++;
            }
        } else {
            png_lit_(&b, d[pos]);
            if (pos + PNG_MINMAT <= len) {
                uint32_t hh = png_hash_(d + pos);
                prev[pos & (PNG_WINDOW - 1)] = head[hh];
                head[hh] = (int32_t)pos;
            }
            pos++;
        }
    }
    png_lit_(&b, 256);            /* end of block */
    png_bits_flush_(&b);

    free(head);
    free(prev);
    return b.ok ? b.pos : 0;
}

/* ---- row filtering ---------------------------------------------------- */

static int png_paeth_(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

/* Try all five filters and keep the one with the smallest sum of absolute
 * signed values -- the standard heuristic, and a good proxy for "compresses
 * best" without actually compressing five times.
 *
 * Written as five tight loops rather than one loop around a switch, and given
 * scratch memory by the caller rather than allocating per row. On a 4K
 * capture this runs eleven million bytes through five filters; a branch and a
 * malloc per row each cost more than they look like they should. */
static void png_filter_row_(const uint8_t* cur, const uint8_t* up,
                            size_t stride, int bpp, uint8_t* out,
                            uint8_t* scratch) {
    uint8_t* cand = out + 1;
    int best_type = 0;
    unsigned long best_score = ~0UL, score;
    size_t i;
    size_t B = (size_t)bpp;

    #define PNG_SCORE_KEEP(t)                                   \
        if (score < best_score) {                               \
            best_score = score; best_type = (t);                \
            memcpy(cand, scratch, stride);                      \
        }
    #define PNG_ACC(v)  do {                                    \
            uint8_t u = (uint8_t)(v); scratch[i] = u;           \
            int s = (int8_t)u; score += (unsigned long)(s < 0 ? -s : s); \
        } while (0)

    score = 0;                                             /* 0: None */
    for (i = 0; i < stride; i++) PNG_ACC(cur[i]);
    PNG_SCORE_KEEP(0);

    score = 0;                                             /* 1: Sub */
    for (i = 0; i < B && i < stride; i++)   PNG_ACC(cur[i]);
    for (; i < stride; i++)                 PNG_ACC(cur[i] - cur[i - B]);
    PNG_SCORE_KEEP(1);

    if (up) {
        score = 0;                                         /* 2: Up */
        for (i = 0; i < stride; i++)        PNG_ACC(cur[i] - up[i]);
        PNG_SCORE_KEEP(2);

        score = 0;                                         /* 3: Average */
        for (i = 0; i < B && i < stride; i++) PNG_ACC(cur[i] - (up[i] >> 1));
        for (; i < stride; i++)               PNG_ACC(cur[i] - ((cur[i - B] + up[i]) >> 1));
        PNG_SCORE_KEEP(3);

        score = 0;                                         /* 4: Paeth */
        for (i = 0; i < B && i < stride; i++) PNG_ACC(cur[i] - up[i]);
        for (; i < stride; i++)
            PNG_ACC(cur[i] - png_paeth_(cur[i - B], up[i], up[i - B]));
        PNG_SCORE_KEEP(4);
    } else {
        /* No row above: Up and Paeth degenerate to None and Sub, and Average
         * to a halved Sub. Only Average is worth the pass. */
        score = 0;
        for (i = 0; i < B && i < stride; i++) PNG_ACC(cur[i]);
        for (; i < stride; i++)               PNG_ACC(cur[i] - (cur[i - B] >> 1));
        PNG_SCORE_KEEP(3);
    }
    #undef PNG_ACC
    #undef PNG_SCORE_KEEP

    out[0] = (uint8_t)best_type;
}

int png_write_rgba(const char* path, const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return 0;

    /* Alpha is almost always a wasted quarter of the file: every screen
     * capture is opaque, and so is anything the editor has flattened. Look
     * rather than assume, so an image that really does have transparency
     * keeps it. */
    int bpp = 3;
    for (size_t i = 0, n = (size_t)w * h; i < n; i++)
        if (rgba[i * 4 + 3] != 255) { bpp = 4; break; }

    FILE* f = fopen(path, "wb");
    if (!f) return 0;

    static const uint8_t SIG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(SIG, 1, 8, f);

    uint8_t ihdr[13];
    png_be32_(ihdr, (uint32_t)w);
    png_be32_(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;                      /* bit depth */
    ihdr[9]  = (bpp == 4) ? 6 : 2;     /* colour type: RGBA or RGB */
    ihdr[10] = 0;                      /* deflate */
    ihdr[11] = 0;                      /* filter method */
    ihdr[12] = 0;                      /* no interlace */
    png_chunk_(f, "IHDR", ihdr, 13);

    size_t stride  = (size_t)w * bpp;
    size_t raw_len = (stride + 1) * (size_t)h;
    uint8_t* raw = (uint8_t*)malloc(raw_len);
    uint8_t* line = (uint8_t*)malloc(stride * 3);   /* cur, up, filter scratch */
    if (!raw || !line) { free(raw); free(line); fclose(f); return 0; }

    uint8_t* cur = line;
    uint8_t* up  = line + stride;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = rgba + (size_t)w * 4 * (size_t)y;
        if (bpp == 3) {
            for (int x = 0; x < w; x++) {
                cur[x * 3 + 0] = src[x * 4 + 0];
                cur[x * 3 + 1] = src[x * 4 + 1];
                cur[x * 3 + 2] = src[x * 4 + 2];
            }
        } else {
            memcpy(cur, src, stride);
        }
        png_filter_row_(cur, y ? up : NULL, stride, bpp,
                        raw + (stride + 1) * (size_t)y, line + stride * 2);
        uint8_t* t = up; up = cur; cur = t;      /* this row becomes "above" */
    }
    free(line);

    /* Adler-32 is over the FILTERED bytes, because those are what deflate
     * compresses and what the decoder will have reconstructed. */
    uint32_t a = 1, bsum = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        bsum = (bsum + a) % 65521;
    }

    size_t zcap = raw_len + 64;
    uint8_t* z = (uint8_t*)malloc(zcap + 16);
    if (!z) { free(raw); fclose(f); return 0; }
    size_t zp = 0;
    z[zp++] = 0x78;   /* CMF: deflate, 32K window */
    z[zp++] = 0x01;   /* FLG: check bits make 0x7801 a multiple of 31 */

    size_t clen = png_deflate_(raw, raw_len, z + zp, zcap - zp);
    if (clen > 0) {
        zp += clen;
    } else {
        /* Did not fit, so compression was not going to pay anyway. Stored
         * blocks: the writer can never produce a file larger than the old
         * one did. */
        free(z);
        size_t nblocks = (raw_len + 65534) / 65535;
        z = (uint8_t*)malloc(2 + nblocks * 5 + raw_len + 4);
        if (!z) { free(raw); fclose(f); return 0; }
        zp = 0;
        z[zp++] = 0x78;
        z[zp++] = 0x01;
        size_t off = 0;
        while (off < raw_len) {
            size_t n = raw_len - off;
            if (n > 65535) n = 65535;
            int final = (off + n >= raw_len);
            z[zp++] = (uint8_t)(final ? 1 : 0);   /* BFINAL, BTYPE=00 */
            z[zp++] = (uint8_t)(n & 0xFF);
            z[zp++] = (uint8_t)(n >> 8);
            z[zp++] = (uint8_t)(~n & 0xFF);
            z[zp++] = (uint8_t)((~n >> 8) & 0xFF);
            memcpy(z + zp, raw + off, n);
            zp += n;
            off += n;
        }
    }

    png_be32_(z + zp, (bsum << 16) | a);
    zp += 4;

    png_chunk_(f, "IDAT", z, (uint32_t)zp);
    png_chunk_(f, "IEND", NULL, 0);

    free(z);
    free(raw);
    fclose(f);
    return 1;
}

#endif /* PNG_WRITE_IMPLEMENTATION */
