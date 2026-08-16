/*
 * gif_reader.h -- compact single-header GIF89a animated-image reader.
 *
 * Mirror of gif.h. Decodes header + global palette + Netscape loop + all
 * frames + Graphics Control Extensions + LZW-compressed image data.
 * Composites frames per disposal method into full RGBA bitmaps.
 *
 * Usage:
 *   #define GIF_READER_IMPLEMENTATION
 *   #include "gif_reader.h"
 *
 *   GifReader r;
 *   if (GifReaderOpen(&r, "cat.gif")) {
 *       for (int i = 0; i < r.nframes; i++) {
 *           GifFrame* f = &r.frames[i];   // f->rgba is width*height*4 RGBA
 *           // f->delay_ms is per-frame delay in milliseconds
 *       }
 *       GifReaderClose(&r);
 *   }
 *
 * Public domain / CC0. Alex Maz -- ORB_Recorder (2026).
 */

#ifndef GIF_READER_H
#define GIF_READER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t* rgba;         /* width*height*4 bytes, top-down */
    int      delay_ms;     /* frame duration (100 * GCE.delay) */
    int      disposal;     /* 0..3 */
} GifFrame;

typedef struct {
    int         width, height;
    int         nframes;
    GifFrame*   frames;
    /* private: */
    uint8_t*    _canvas;       /* live composite buffer during decode */
    uint8_t*    _prev_canvas;  /* for disposal=3 restore */
    int         _canvas_w, _canvas_h;
    uint8_t     _gpal[256][3];
    bool        _has_gpal;
    int         _bg_color;
} GifReader;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true on success; false on malformed file. */
bool GifReaderOpen (GifReader* r, const char* path);
void GifReaderClose(GifReader* r);

#ifdef __cplusplus
}
#endif

#endif /* GIF_READER_H */

/* ================================================================== */
#ifdef GIF_READER_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t* buf;
    size_t         len;
    size_t         pos;
} GifCursor_;

static int gr_u8_(GifCursor_* c) {
    if (c->pos >= c->len) return -1;
    return c->buf[c->pos++];
}
static int gr_u16_(GifCursor_* c) {
    int lo = gr_u8_(c);
    int hi = gr_u8_(c);
    if (lo < 0 || hi < 0) return -1;
    return (hi << 8) | lo;
}
static int gr_read_(GifCursor_* c, void* dst, size_t n) {
    if (c->pos + n > c->len) return 0;
    memcpy(dst, c->buf + c->pos, n);
    c->pos += n;
    return 1;
}

/* Read a chain of GIF sub-blocks (each: 1-byte len, then len bytes; terminated by 0).
 * Returns a malloc'd flat buffer with concatenated payload; sets *out_len. */
static uint8_t* gr_read_sub_blocks_(GifCursor_* c, size_t* out_len) {
    size_t cap = 512, len = 0;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        int b = gr_u8_(c);
        if (b <= 0) { *out_len = len; return buf; }   /* terminator (0) or EOF */
        if (len + (size_t)b > cap) {
            while (cap < len + (size_t)b) cap *= 2;
            uint8_t* g = (uint8_t*)realloc(buf, cap);
            if (!g) { free(buf); return NULL; }
            buf = g;
        }
        if (!gr_read_(c, buf + len, (size_t)b)) { free(buf); return NULL; }
        len += (size_t)b;
    }
}

/* LZW decoder. Input: bytes from gr_read_sub_blocks_ (already stripped of
 * block-length prefixes). Output: pixel-index buffer of `npix` bytes.
 * Returns true on success. Handles the "KwKwK" special case where the
 * emitted code equals next_code (the entry we're about to define). */
static bool gr_lzw_decode_(const uint8_t* in, size_t in_len, int min_code_size,
                           uint8_t* out, int npix) {
    if (min_code_size < 2 || min_code_size > 8) return false;
    int clear_code = 1 << min_code_size;
    int eoi_code   = clear_code + 1;
    int code_size  = min_code_size + 1;
    int next_code  = eoi_code + 1;

    #define GR_MAX 4096
    int      prefix[GR_MAX];
    uint8_t  suffix[GR_MAX];
    uint8_t  first_char[GR_MAX];   /* first char of the string coded by each entry */
    for (int i = 0; i < clear_code; i++) {
        prefix[i]     = -1;
        suffix[i]     = (uint8_t)i;
        first_char[i] = (uint8_t)i;
    }

    uint32_t bit_buf = 0;
    int      bit_cnt = 0;
    size_t   in_pos  = 0;
    int      pos     = 0;
    int      prev    = -1;
    uint8_t  stack[GR_MAX];

    while (1) {
        while (bit_cnt < code_size) {
            if (in_pos >= in_len) return pos == npix;
            bit_buf |= ((uint32_t)in[in_pos++]) << bit_cnt;
            bit_cnt += 8;
        }
        int code = (int)(bit_buf & (((uint32_t)1 << code_size) - 1));
        bit_buf >>= code_size;
        bit_cnt -= code_size;

        if (code == clear_code) {
            code_size = min_code_size + 1;
            next_code = eoi_code + 1;
            prev = -1;
            continue;
        }
        if (code == eoi_code) return pos == npix;

        int c = code;
        int stack_top = 0;
        if (c >= next_code) {
            /* KwKwK case */
            if (prev < 0) return false;
            stack[stack_top++] = first_char[prev];
            c = prev;
        }
        while (c >= 0) {
            if (stack_top >= GR_MAX) return false;
            stack[stack_top++] = suffix[c];
            c = prefix[c];
        }
        /* Emit reversed */
        uint8_t first_emit = stack[stack_top - 1];
        while (stack_top > 0) {
            if (pos >= npix) return false;
            out[pos++] = stack[--stack_top];
        }
        if (prev >= 0 && next_code < GR_MAX) {
            prefix[next_code]     = prev;
            suffix[next_code]     = first_emit;
            first_char[next_code] = first_char[prev];
            next_code++;
            if (next_code == (1 << code_size) && code_size < 12) code_size++;
        }
        prev = code;
    }
    #undef GR_MAX
}

/* Composite a decoded frame into the canvas, honoring disposal + transparency. */
static void gr_composite_(GifReader* r, int fx, int fy, int fw, int fh,
                          const uint8_t* idx, const uint8_t pal[256][3],
                          int transp_idx, uint8_t* out_frame) {
    uint8_t* cv = r->_canvas;
    int cw = r->_canvas_w;
    int ch = r->_canvas_h;

    /* Composite this frame's pixels onto canvas at (fx, fy) with size (fw, fh). */
    for (int y = 0; y < fh; y++) {
        int cy = fy + y;
        if (cy < 0 || cy >= ch) continue;
        for (int x = 0; x < fw; x++) {
            int cx = fx + x;
            if (cx < 0 || cx >= cw) continue;
            int px_idx = idx[y * fw + x];
            if (transp_idx >= 0 && px_idx == transp_idx) continue;
            uint8_t* dst = cv + (cy * cw + cx) * 4;
            dst[0] = pal[px_idx][0];
            dst[1] = pal[px_idx][1];
            dst[2] = pal[px_idx][2];
            dst[3] = 255;
        }
    }
    /* Copy the whole canvas to the frame output. */
    memcpy(out_frame, cv, cw * ch * 4);
}

bool GifReaderOpen(GifReader* r, const char* path) {
    memset(r, 0, sizeof *r);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 13) { fclose(f); return false; }
    uint8_t* raw = (uint8_t*)malloc((size_t)fsz);
    if (!raw) { fclose(f); return false; }
    if (fread(raw, 1, (size_t)fsz, f) != (size_t)fsz) {
        free(raw); fclose(f); return false;
    }
    fclose(f);

    GifCursor_ cur = { raw, (size_t)fsz, 0 };

    /* Header */
    char sig[6];
    if (!gr_read_(&cur, sig, 6)) { free(raw); return false; }
    if (memcmp(sig, "GIF87a", 6) != 0 && memcmp(sig, "GIF89a", 6) != 0) {
        free(raw); return false;
    }
    /* Logical Screen Descriptor */
    int lw = gr_u16_(&cur);
    int lh = gr_u16_(&cur);
    int packed = gr_u8_(&cur);
    int bg = gr_u8_(&cur);
    gr_u8_(&cur);   /* aspect ratio */
    if (lw <= 0 || lh <= 0) { free(raw); return false; }
    r->width  = lw;
    r->height = lh;
    r->_bg_color = bg;

    bool have_gct = (packed & 0x80) != 0;
    int  gct_size = 1 << ((packed & 0x07) + 1);
    if (have_gct) {
        for (int i = 0; i < gct_size; i++) {
            r->_gpal[i][0] = (uint8_t)gr_u8_(&cur);
            r->_gpal[i][1] = (uint8_t)gr_u8_(&cur);
            r->_gpal[i][2] = (uint8_t)gr_u8_(&cur);
        }
        for (int i = gct_size; i < 256; i++) {
            r->_gpal[i][0] = r->_gpal[i][1] = r->_gpal[i][2] = 0;
        }
        r->_has_gpal = true;
    }

    /* Allocate canvas. */
    r->_canvas_w = lw;
    r->_canvas_h = lh;
    r->_canvas      = (uint8_t*)calloc(1, (size_t)lw * lh * 4);
    r->_prev_canvas = (uint8_t*)calloc(1, (size_t)lw * lh * 4);
    if (!r->_canvas || !r->_prev_canvas) { GifReaderClose(r); free(raw); return false; }

    /* Frame list grows on demand. */
    int fcap = 8;
    r->frames = (GifFrame*)calloc(fcap, sizeof(GifFrame));

    /* Per-frame GCE state (default). */
    int cur_delay_10ms = 0;
    int cur_disposal   = 0;
    int cur_transp     = -1;

    while (1) {
        int marker = gr_u8_(&cur);
        if (marker < 0 || marker == 0x3B) break;   /* trailer or EOF */

        if (marker == 0x21) {
            /* Extension */
            int label = gr_u8_(&cur);
            if (label == 0xF9) {
                /* Graphics Control Extension */
                gr_u8_(&cur);   /* block size = 4 */
                int gpack = gr_u8_(&cur);
                cur_delay_10ms = gr_u16_(&cur);
                int t_idx = gr_u8_(&cur);
                gr_u8_(&cur);   /* terminator */
                cur_disposal = (gpack >> 2) & 0x07;
                cur_transp   = (gpack & 0x01) ? t_idx : -1;
            } else {
                /* Skip other extensions' sub-blocks. */
                size_t skipped_len = 0;
                uint8_t* skipped = gr_read_sub_blocks_(&cur, &skipped_len);
                free(skipped);
            }
            continue;
        }
        if (marker == 0x2C) {
            /* Image Descriptor */
            int fx = gr_u16_(&cur);
            int fy = gr_u16_(&cur);
            int fw = gr_u16_(&cur);
            int fh = gr_u16_(&cur);
            int fpack = gr_u8_(&cur);
            if (fw <= 0 || fh <= 0) { free(raw); GifReaderClose(r); return false; }

            uint8_t pal[256][3];
            memcpy(pal, r->_gpal, sizeof pal);
            bool have_lct = (fpack & 0x80) != 0;
            int  lct_size = 1 << ((fpack & 0x07) + 1);
            if (have_lct) {
                for (int i = 0; i < lct_size; i++) {
                    pal[i][0] = (uint8_t)gr_u8_(&cur);
                    pal[i][1] = (uint8_t)gr_u8_(&cur);
                    pal[i][2] = (uint8_t)gr_u8_(&cur);
                }
            }
            int min_code_size = gr_u8_(&cur);
            size_t lzw_len = 0;
            uint8_t* lzw = gr_read_sub_blocks_(&cur, &lzw_len);
            if (!lzw) { free(raw); GifReaderClose(r); return false; }
            int npix = fw * fh;
            uint8_t* idx = (uint8_t*)malloc(npix);
            if (!idx) { free(lzw); free(raw); GifReaderClose(r); return false; }
            bool ok = gr_lzw_decode_(lzw, lzw_len, min_code_size, idx, npix);
            free(lzw);
            if (!ok) { free(idx); free(raw); GifReaderClose(r); return false; }

            /* Snapshot for disposal=3 */
            if (cur_disposal == 3) {
                memcpy(r->_prev_canvas, r->_canvas, (size_t)r->_canvas_w * r->_canvas_h * 4);
            }

            /* Grow frames array if needed. */
            if (r->nframes == fcap) {
                fcap *= 2;
                GifFrame* g = (GifFrame*)realloc(r->frames, fcap * sizeof(GifFrame));
                if (!g) { free(idx); free(raw); GifReaderClose(r); return false; }
                r->frames = g;
            }
            GifFrame* frame = &r->frames[r->nframes++];
            frame->rgba = (uint8_t*)malloc((size_t)r->_canvas_w * r->_canvas_h * 4);
            if (!frame->rgba) { free(idx); free(raw); GifReaderClose(r); return false; }
            frame->delay_ms = cur_delay_10ms * 10;
            if (frame->delay_ms < 20) frame->delay_ms = 100;   /* sanity default */
            frame->disposal = cur_disposal;

            gr_composite_(r, fx, fy, fw, fh, idx, pal, cur_transp, frame->rgba);
            free(idx);

            /* Apply disposal for next frame. */
            if (cur_disposal == 2) {
                /* Restore to background: fill the frame's rect with bg color. */
                for (int y = 0; y < fh; y++) {
                    int cy = fy + y;
                    if (cy < 0 || cy >= r->_canvas_h) continue;
                    for (int x = 0; x < fw; x++) {
                        int cx = fx + x;
                        if (cx < 0 || cx >= r->_canvas_w) continue;
                        uint8_t* d = r->_canvas + (cy * r->_canvas_w + cx) * 4;
                        d[0] = d[1] = d[2] = 0; d[3] = 0;
                    }
                }
            } else if (cur_disposal == 3) {
                memcpy(r->_canvas, r->_prev_canvas,
                       (size_t)r->_canvas_w * r->_canvas_h * 4);
            }
            /* disposal 0 or 1 = leave canvas as-is */
            continue;
        }
        /* Unknown block: stop cleanly. */
        break;
    }
    free(raw);
    return r->nframes > 0;
}

void GifReaderClose(GifReader* r) {
    if (!r) return;
    if (r->frames) {
        for (int i = 0; i < r->nframes; i++) free(r->frames[i].rgba);
        free(r->frames);
    }
    free(r->_canvas);
    free(r->_prev_canvas);
    memset(r, 0, sizeof *r);
}

#endif /* GIF_READER_IMPLEMENTATION */
