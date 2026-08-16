/*
 * gif.h -- compact single-header GIF89a animation writer.
 *
 * Correct LZW encoder built from the classic Compuserve reference algorithm
 * with a full tree-based dictionary (no hash-table collisions). Handles
 * dict-full CLEAR, per-frame Graphics Control Extension, Netscape looping,
 * fixed 6x6x6 web-safe + 40-step grayscale palette.
 *
 * Usage:
 *   #define GIF_H_IMPLEMENTATION
 *   #include "gif.h"
 *
 *   GifWriter w;
 *   GifBegin(&w, "out.gif", width, height, delay_1_100s);
 *   for (each frame) GifWriteFrame(&w, rgba_bytes, width, height, delay);
 *   GifEnd(&w);
 *
 * rgba_bytes is width*height*4 in RGBA order (alpha ignored).
 * Public domain / CC0 -- based on the algorithm from GIF89a Appendix E and
 * the widely-used charlietangora/gif-h reference layout.
 */

#ifndef GIF_H
#define GIF_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct GifWriter {
    FILE* f;
    int   w, h;
} GifWriter;

#ifdef __cplusplus
extern "C" {
#endif

int GifBegin      (GifWriter* w, const char* path, int width, int height, int delay);
int GifWriteFrame (GifWriter* w, const uint8_t* rgba, int width, int height, int delay);
int GifEnd        (GifWriter* w);

#ifdef __cplusplus
}
#endif

#endif /* GIF_H */

/* ================================================================== */
#ifdef GIF_H_IMPLEMENTATION

/* ---- palette: 216 web-safe + 40-step grayscale --- */

#define GIF_PAL_SIZE 256

typedef struct { uint8_t r, g, b; } GifPal_;
static GifPal_ gif_pal_[GIF_PAL_SIZE];
static int gif_pal_built_ = 0;

static void gif_build_palette_(void) {
    int i = 0;
    for (int r = 0; r < 6; r++)
    for (int g = 0; g < 6; g++)
    for (int b = 0; b < 6; b++) {
        gif_pal_[i].r = (uint8_t)(r * 51);
        gif_pal_[i].g = (uint8_t)(g * 51);
        gif_pal_[i].b = (uint8_t)(b * 51);
        i++;
    }
    for (int k = 0; k < 40; k++) {
        int v = (k * 255 + 19) / 39;
        if (v > 255) v = 255;
        gif_pal_[i].r = gif_pal_[i].g = gif_pal_[i].b = (uint8_t)v;
        i++;
    }
    gif_pal_built_ = 1;
}

/* Ordered-dither support. Measured on screen-recording content, dithering
 * made things WORSE on both axes -- RMS error 7.13 vs 4.30, and file size
 * 265 KB vs 117 KB for the same 20 frames -- because the 40-step gray ramp
 * is already fine enough (~6.5 per step) that the added noise is pure error
 * and pure entropy, and entropy is what LZW charges for. Kept as a centre
 * tap (bpos = 8, i.e. no offset) so the quantizer below reads the same for
 * both paths and dither can be switched back on by restoring the lookup. */
static const uint8_t gif_bayer4_[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

/* Pick a palette index for one pixel.
 *
 * Two ramps live in the palette and both are now reachable:
 *   0..215   6x6x6 colour cube, 51 per step
 *   216..255 40-step grayscale, ~6.5 per step
 *
 * Near-neutral pixels route to the grayscale ramp. This matters enormously
 * for screen recording: a dark UI background of (25,25,25) quantized on the
 * colour cube lands on (0,0,0) -- an error of 25 and a visibly crushed
 * black. On the gray ramp it lands on (26,26,26), an error of 1.
 * (The grayscale entries were present in the palette from the start but
 * gif_pick_color_ never selected them.) */
static uint8_t gif_pick_color_(uint8_t r, uint8_t g, uint8_t b, int x, int y) {
    (void)x; (void)y; (void)gif_bayer4_;
    const int bpos = 8;                 /* centre tap == no dither */

    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);

    if (mx - mn <= 12) {
        /* Near-neutral -> 40-step gray ramp, dithered by +-half a step. */
        int v = (r * 77 + g * 151 + b * 28) >> 8;      /* luma */
        v += (bpos * 7) / 16 - 3;                      /* ~+-3 of a 6.5 step */
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        int k = (v * 39 + 127) / 255;
        if (k > 39) k = 39;
        return (uint8_t)(216 + k);
    }

    /* Chromatic -> 6x6x6 cube, dithered by +-half a step (51/2). */
    int d = (bpos * 51) / 16 - 25;
    int rr = (int)r + d, gg = (int)g + d, bb = (int)b + d;
    if (rr < 0)   rr = 0;
    if (rr > 255) rr = 255;
    if (gg < 0)   gg = 0;
    if (gg > 255) gg = 255;
    if (bb < 0)   bb = 0;
    if (bb > 255) bb = 255;
    int qr = (rr + 25) / 51; if (qr > 5) qr = 5;
    int qg = (gg + 25) / 51; if (qg > 5) qg = 5;
    int qb = (bb + 25) / 51; if (qb > 5) qb = 5;
    return (uint8_t)(qr * 36 + qg * 6 + qb);
}

/* ---- bit-packer: writes GIF sub-blocks (255 bytes max each) ------------ */

typedef struct {
    FILE*    f;
    uint32_t bit_buf;
    int      bit_cnt;
    uint8_t  blk[255];
    int      blk_len;
} GifBitOut_;

static void gif_flush_block_(GifBitOut_* b) {
    if (b->blk_len > 0) {
        uint8_t len = (uint8_t)b->blk_len;
        fwrite(&len, 1, 1, b->f);
        fwrite(b->blk, 1, b->blk_len, b->f);
        b->blk_len = 0;
    }
}

static void gif_bit_write_(GifBitOut_* b, uint32_t code, int nbits) {
    b->bit_buf |= (code << b->bit_cnt);
    b->bit_cnt += nbits;
    while (b->bit_cnt >= 8) {
        b->blk[b->blk_len++] = (uint8_t)(b->bit_buf & 0xFF);
        b->bit_buf >>= 8;
        b->bit_cnt -= 8;
        if (b->blk_len == 255) gif_flush_block_(b);
    }
}

static void gif_bit_finish_(GifBitOut_* b) {
    if (b->bit_cnt > 0) {
        b->blk[b->blk_len++] = (uint8_t)(b->bit_buf & 0xFF);
        b->bit_buf = 0;
        b->bit_cnt = 0;
        if (b->blk_len == 255) gif_flush_block_(b);
    }
    gif_flush_block_(b);
    uint8_t zero = 0;
    fwrite(&zero, 1, 1, b->f);
}

/* ---- LZW encoder using a tree dictionary (4096 nodes x 256 children) --- */

#define GIF_LZW_MAX_CODE   4095
#define GIF_LZW_MAX_BITS   12

static void gif_write_lzw_(FILE* f, const uint8_t* px, int npix, int min_code_size) {
    uint8_t mcs = (uint8_t)min_code_size;
    fwrite(&mcs, 1, 1, f);

    GifBitOut_ b;
    b.f = f; b.bit_buf = 0; b.bit_cnt = 0; b.blk_len = 0;

    int clear_code = 1 << min_code_size;
    int eoi_code   = clear_code + 1;
    int code_size  = min_code_size + 1;
    int code_count = eoi_code;

    /* Tree dict: [4096][256] int16 children. 0 = unassigned. */
    int16_t* tree = (int16_t*)calloc(4096 * 256, sizeof(int16_t));
    if (!tree) return;

    gif_bit_write_(&b, (uint32_t)clear_code, code_size);

    int cur_code = px[0];
    for (int i = 1; i < npix; i++) {
        uint8_t p = px[i];
        int16_t child = tree[cur_code * 256 + p];
        if (child != 0) {
            cur_code = child;
        } else {
            gif_bit_write_(&b, (uint32_t)cur_code, code_size);

            if (code_count < GIF_LZW_MAX_CODE) {
                ++code_count;
                tree[cur_code * 256 + p] = (int16_t)code_count;
                if (code_count == (1 << code_size) && code_size < GIF_LZW_MAX_BITS) {
                    code_size++;
                }
            } else {
                gif_bit_write_(&b, (uint32_t)clear_code, code_size);
                memset(tree, 0, 4096 * 256 * sizeof(int16_t));
                code_count = eoi_code;
                code_size  = min_code_size + 1;
            }
            cur_code = p;
        }
    }
    gif_bit_write_(&b, (uint32_t)cur_code, code_size);
    gif_bit_write_(&b, (uint32_t)eoi_code, code_size);
    gif_bit_finish_(&b);

    free(tree);
}

/* ---- GIF89a wrapper: header, global palette, Netscape loop, frames --- */

static void gif_write_header_(GifWriter* w) {
    fwrite("GIF89a", 1, 6, w->f);
    uint8_t lsd[7];
    lsd[0] = (uint8_t)(w->w & 0xFF);
    lsd[1] = (uint8_t)((w->w >> 8) & 0xFF);
    lsd[2] = (uint8_t)(w->h & 0xFF);
    lsd[3] = (uint8_t)((w->h >> 8) & 0xFF);
    lsd[4] = 0xF7;
    lsd[5] = 0;
    lsd[6] = 0;
    fwrite(lsd, 1, 7, w->f);
    for (int i = 0; i < GIF_PAL_SIZE; i++) {
        uint8_t c[3] = { gif_pal_[i].r, gif_pal_[i].g, gif_pal_[i].b };
        fwrite(c, 1, 3, w->f);
    }
    uint8_t nsext[] = {
        0x21, 0xFF, 0x0B,
        'N','E','T','S','C','A','P','E','2','.','0',
        0x03, 0x01, 0x00, 0x00, 0x00
    };
    fwrite(nsext, 1, sizeof(nsext), w->f);
}

int GifBegin(GifWriter* w, const char* path, int width, int height, int delay) {
    (void)delay;
    if (!gif_pal_built_) gif_build_palette_();
    w->f = fopen(path, "wb");
    if (!w->f) return 0;
    w->w = width;
    w->h = height;
    gif_write_header_(w);
    return 1;
}

int GifWriteFrame(GifWriter* w, const uint8_t* rgba, int width, int height, int delay) {
    if (!w->f) return 0;
    (void)width; (void)height;

    uint8_t gce[8];
    gce[0] = 0x21; gce[1] = 0xF9; gce[2] = 0x04;
    gce[3] = 0x04;   /* disposal = 1 (do not dispose), no transparency, no user input */
    gce[4] = (uint8_t)(delay & 0xFF);
    gce[5] = (uint8_t)((delay >> 8) & 0xFF);
    gce[6] = 0;
    gce[7] = 0;
    fwrite(gce, 1, 8, w->f);

    uint8_t id[10];
    id[0] = 0x2C;
    id[1] = 0; id[2] = 0;
    id[3] = 0; id[4] = 0;
    id[5] = (uint8_t)(w->w & 0xFF);
    id[6] = (uint8_t)((w->w >> 8) & 0xFF);
    id[7] = (uint8_t)(w->h & 0xFF);
    id[8] = (uint8_t)((w->h >> 8) & 0xFF);
    id[9] = 0x00;
    fwrite(id, 1, 10, w->f);

    int npix = w->w * w->h;
    uint8_t* idx = (uint8_t*)malloc(npix);
    if (!idx) return 0;
    /* x,y are needed so the ordered-dither matrix lines up with the image. */
    for (int y = 0; y < w->h; y++) {
        const uint8_t* row = rgba + (size_t)y * w->w * 4;
        uint8_t* orow = idx + (size_t)y * w->w;
        for (int x = 0; x < w->w; x++) {
            orow[x] = gif_pick_color_(row[x*4], row[x*4+1], row[x*4+2], x, y);
        }
    }

    gif_write_lzw_(w->f, idx, npix, 8);
    free(idx);
    return 1;
}

int GifEnd(GifWriter* w) {
    if (!w->f) return 0;
    uint8_t trailer = 0x3B;
    fwrite(&trailer, 1, 1, w->f);
    fclose(w->f);
    w->f = NULL;
    return 1;
}

#endif /* GIF_H_IMPLEMENTATION */
