/*
 * png_write.h -- minimal single-header PNG writer.
 *
 * Screenshots want PNG: lossless, universally pasteable, and it handles a
 * screenful of flat UI colour far better than GIF's 256-entry palette.
 *
 * Deliberately writes UNCOMPRESSED deflate blocks (BTYPE=00) rather than
 * implementing LZ77 + Huffman. That is a real trade: files are roughly the
 * size of the raw pixels instead of a tenth of it. It buys a writer that is
 * 120 lines and cannot be wrong, versus ~600 lines of Huffman table
 * construction that can be subtly wrong -- and we already learned that
 * lesson the expensive way with the GIF LZW encoder, which produced files
 * that decoded cleanly for two frames and then fell apart.
 *
 * A stored-block PNG is valid PNG. Every viewer opens it. If the size ever
 * matters, this is the place to add fixed-Huffman blocks.
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

int png_write_rgba(const char* path, const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;

    static const uint8_t SIG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(SIG, 1, 8, f);

    uint8_t ihdr[13];
    png_be32_(ihdr, (uint32_t)w);
    png_be32_(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;    /* bit depth   */
    ihdr[9]  = 6;    /* colour type: RGBA */
    ihdr[10] = 0;    /* deflate     */
    ihdr[11] = 0;    /* filter      */
    ihdr[12] = 0;    /* no interlace */
    png_chunk_(f, "IHDR", ihdr, 13);

    /* Raw scanlines, each prefixed with filter byte 0 (None). */
    size_t stride = (size_t)w * 4;
    size_t raw_len = (stride + 1) * (size_t)h;
    uint8_t* raw = (uint8_t*)malloc(raw_len);
    if (!raw) { fclose(f); return 0; }
    for (int y = 0; y < h; y++) {
        uint8_t* row = raw + (stride + 1) * (size_t)y;
        row[0] = 0;
        memcpy(row + 1, rgba + stride * (size_t)y, stride);
    }

    /* zlib wrapper + stored deflate blocks (max 65535 bytes each). */
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + nblocks * 5 + raw_len + 4;
    uint8_t* z = (uint8_t*)malloc(z_len);
    if (!z) { free(raw); fclose(f); return 0; }

    size_t zp = 0;
    z[zp++] = 0x78;   /* CMF: deflate, 32K window */
    z[zp++] = 0x01;   /* FLG: check bits make 0x7801 a multiple of 31 */

    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off;
        if (n > 65535) n = 65535;
        int final = (off + n >= raw_len);
        z[zp++] = (uint8_t)(final ? 1 : 0);        /* BFINAL, BTYPE=00 */
        z[zp++] = (uint8_t)(n & 0xFF);
        z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)(~n & 0xFF);            /* one's complement */
        z[zp++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zp, raw + off, n);
        zp += n;
        off += n;
    }

    /* Adler-32 of the uncompressed data. */
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    png_be32_(z + zp, (b << 16) | a);
    zp += 4;

    png_chunk_(f, "IDAT", z, (uint32_t)zp);
    png_chunk_(f, "IEND", NULL, 0);

    free(z);
    free(raw);
    fclose(f);
    return 1;
}

#endif /* PNG_WRITE_IMPLEMENTATION */
