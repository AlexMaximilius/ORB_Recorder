/* paint.h -- software rasteriser for screenshot annotation.
 *
 * Pure pixel operations on a straight RGBA8 buffer. No GL, no window, no
 * application state, so the editor's drawing can be reasoned about (and
 * tested) on its own, and so what you SAVE is produced by exactly the code
 * that produced what you SAW. An editor that previews with the GPU and saves
 * with something else is an editor that lies to you occasionally.
 *
 * Integer only. Coverage is 0..255, distances are compared squared in
 * half-pixel units, and the one square root is an integer square root. This
 * is not austerity for its own sake: anti-aliasing wants a coverage ramp, a
 * ramp wants a distance, and a distance in fixed point is both exact and
 * faster than the float version. No value here is ever compared to a float.
 *
 * Coordinates are image pixels. Everything clips to the buffer, so callers
 * may hand in a rubber-band rectangle that is inverted, off-screen, or
 * degenerate, and get sensible behaviour instead of a crash.
 */
#ifndef PAINT_H
#define PAINT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t* px; int w, h; } PaintImg;

/* 0xRRGGBB */
#define PAINT_RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))

/* ---- helpers ---------------------------------------------------------- */

static int paint_isqrt(int64_t v) {
    if (v <= 0) return 0;
    int64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (int)x;
}

static void paint_swap(int* a, int* b) { int t = *a; *a = *b; *b = t; }

/* Source-over with an 8-bit coverage. */
static void paint_blend(PaintImg im, int x, int y, uint32_t rgb, int cov) {
    if (cov <= 0 || x < 0 || y < 0 || x >= im.w || y >= im.h) return;
    if (cov > 255) cov = 255;
    uint8_t* p = im.px + ((size_t)y * im.w + x) * 4;
    int sr = (int)((rgb >> 16) & 0xFF);
    int sg = (int)((rgb >>  8) & 0xFF);
    int sb = (int)( rgb        & 0xFF);
    p[0] = (uint8_t)((sr * cov + p[0] * (255 - cov)) / 255);
    p[1] = (uint8_t)((sg * cov + p[1] * (255 - cov)) / 255);
    p[2] = (uint8_t)((sb * cov + p[2] * (255 - cov)) / 255);
    p[3] = 255;
}

/* Coverage from a distance to the shape edge.
 *
 * Both are in HALF-pixel units, which is what lets the whole rasteriser stay
 * integer: a pixel centre is at an odd half-coordinate, so doubling every
 * coordinate makes centres land exactly on integers. The ramp is one pixel
 * (two half-units) wide, which is the narrowest that still looks smooth. */
static int paint_cov(int dist_half, int radius_half) {
    int d = dist_half - radius_half;
    if (d <= -1) return 255;
    if (d >=  1) return 0;
    return 255 * (1 - d) / 2;
}

static void paint_clamp_rect(PaintImg im, int* x0, int* y0, int* x1, int* y1) {
    if (*x1 < *x0) paint_swap(x0, x1);
    if (*y1 < *y0) paint_swap(y0, y1);
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > im.w - 1) *x1 = im.w - 1;
    if (*y1 > im.h - 1) *y1 = im.h - 1;
}

/* ---- line ------------------------------------------------------------- */

/* Squared distance from a point to a segment, all in half-pixel units. */
static int64_t paint_seg_d2(int px, int py, int ax, int ay, int bx, int by) {
    int64_t vx = bx - ax, vy = by - ay;
    int64_t wx = px - ax, wy = py - ay;
    int64_t len2 = vx * vx + vy * vy;
    int64_t ww   = wx * wx + wy * wy;
    if (len2 <= 0) return ww;

    int64_t dot = wx * vx + wy * vy;
    if (dot <= 0)    return ww;                    /* before the start */
    if (dot >= len2) {                             /* past the end */
        int64_t ux = px - bx, uy = py - by;
        return ux * ux + uy * uy;
    }
    /* Perpendicular: |w|^2 - (w.v)^2/|v|^2. Written this way rather than by
     * finding the foot of the perpendicular, because scaling coordinates by
     * |v|^2 and then squaring them overflows 64 bits on a large image. */
    return ww - dot * dot / len2;
}

void paint_line(PaintImg im, int x0, int y0, int x1, int y1,
                uint32_t rgb, int thick) {
    if (thick < 1) thick = 1;
    int rad = thick;                     /* half-units: thick/2 px * 2 */
    int bx0 = (x0 < x1 ? x0 : x1) - thick - 1;
    int bx1 = (x0 > x1 ? x0 : x1) + thick + 1;
    int by0 = (y0 < y1 ? y0 : y1) - thick - 1;
    int by1 = (y0 > y1 ? y0 : y1) + thick + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > im.w - 1) bx1 = im.w - 1;
    if (by1 > im.h - 1) by1 = im.h - 1;

    int ax = 2 * x0, ay = 2 * y0, ex = 2 * x1, ey = 2 * y1;
    for (int y = by0; y <= by1; y++) {
        int py = 2 * y + 1;
        for (int x = bx0; x <= bx1; x++) {
            int px = 2 * x + 1;
            int d = paint_isqrt(paint_seg_d2(px, py, ax, ay, ex, ey));
            int cov = paint_cov(d, rad);
            if (cov) paint_blend(im, x, y, rgb, cov);
        }
    }
}

/* ---- rectangle -------------------------------------------------------- */

void paint_rect(PaintImg im, int x0, int y0, int x1, int y1,
                uint32_t rgb, int thick, bool fill) {
    if (x1 < x0) paint_swap(&x0, &x1);
    if (y1 < y0) paint_swap(&y0, &y1);
    if (fill) {
        int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
        paint_clamp_rect(im, &cx0, &cy0, &cx1, &cy1);
        for (int y = cy0; y <= cy1; y++)
            for (int x = cx0; x <= cx1; x++)
                paint_blend(im, x, y, rgb, 255);
        return;
    }
    /* Four segments rather than a distance field: corners then join the way
     * a drawn rectangle should, with no rounding at the joints. */
    paint_line(im, x0, y0, x1, y0, rgb, thick);
    paint_line(im, x1, y0, x1, y1, rgb, thick);
    paint_line(im, x1, y1, x0, y1, rgb, thick);
    paint_line(im, x0, y1, x0, y0, rgb, thick);
}

/* ---- ellipse ---------------------------------------------------------- */

void paint_ellipse(PaintImg im, int x0, int y0, int x1, int y1,
                   uint32_t rgb, int thick, bool fill) {
    if (x1 < x0) paint_swap(&x0, &x1);
    if (y1 < y0) paint_swap(&y0, &y1);
    if (thick < 1) thick = 1;

    int cx = x0 + x1 + 1, cy = y0 + y1 + 1;          /* centre, half-units */
    int rx = (x1 - x0 + 1), ry = (y1 - y0 + 1);      /* radii,  half-units */
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;

    int bx0 = x0 - thick - 1, bx1 = x1 + thick + 1;
    int by0 = y0 - thick - 1, by1 = y1 + thick + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > im.w - 1) bx1 = im.w - 1;
    if (by1 > im.h - 1) by1 = im.h - 1;

    for (int y = by0; y <= by1; y++) {
        int py = 2 * y + 1 - cy;
        for (int x = bx0; x <= bx1; x++) {
            int px = 2 * x + 1 - cx;
            /* Radial distance to the boundary, exactly.
             *
             * The local radius along this direction is rx*ry*|p| / f, where
             * f = |(px*ry, py*rx)|. So |p| - r(theta) collapses to
             * |p| * (f - rx*ry) / f -- no trigonometry, no gradient estimate,
             * and every term stays inside 64 bits. Radial distance runs a
             * whisker longer than perpendicular distance on an eccentric
             * ellipse, which softens the ends by a fraction of a pixel and is
             * invisible next to being wrong the other way. */
            int64_t fx = (int64_t)px * ry, fy = (int64_t)py * rx;
            int64_t f  = paint_isqrt(fx * fx + fy * fy);
            int64_t rr = (int64_t)rx * ry;
            int64_t q  = paint_isqrt((int64_t)px * px + (int64_t)py * py);
            int64_t sd = (f == 0) ? -rr : q * (f - rr) / f;   /* signed */
            int dist = (int)(sd < 0 ? -sd : sd);
            int cov;
            if (fill) {
                cov = (sd <= 0) ? 255 : paint_cov(dist, 0);
            } else {
                cov = paint_cov(dist, thick);
            }
            if (cov) paint_blend(im, x, y, rgb, cov);
        }
    }
}

/* ---- arrow ------------------------------------------------------------ */

/* A filled triangle, used for the head. Half-unit coordinates in, so the
 * head lands where the line actually ends rather than a pixel off. */
static void paint_tri(PaintImg im, int ax, int ay, int bx, int by,
                      int cx, int cy, uint32_t rgb) {
    int bx0 = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    int bx1 = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int by0 = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    int by1 = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    bx0 = bx0 / 2 - 1; bx1 = bx1 / 2 + 1;
    by0 = by0 / 2 - 1; by1 = by1 / 2 + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > im.w - 1) bx1 = im.w - 1;
    if (by1 > im.h - 1) by1 = im.h - 1;

    for (int y = by0; y <= by1; y++) {
        int py = 2 * y + 1;
        for (int x = bx0; x <= bx1; x++) {
            int px = 2 * x + 1;
            int64_t d1 = (int64_t)(bx-ax)*(py-ay) - (int64_t)(by-ay)*(px-ax);
            int64_t d2 = (int64_t)(cx-bx)*(py-by) - (int64_t)(cy-by)*(px-bx);
            int64_t d3 = (int64_t)(ax-cx)*(py-cy) - (int64_t)(ay-cy)*(px-cx);
            bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            if (!(neg && pos)) paint_blend(im, x, y, rgb, 255);
        }
    }
}

void paint_arrow(PaintImg im, int x0, int y0, int x1, int y1,
                 uint32_t rgb, int thick) {
    if (thick < 1) thick = 1;
    int64_t dx = x1 - x0, dy = y1 - y0;
    int len = paint_isqrt(dx * dx + dy * dy);
    if (len < 1) { paint_line(im, x0, y0, x1, y1, rgb, thick); return; }

    /* Head scales with the stroke, so a thick arrow does not end in a pin. */
    int head = thick * 4 + 6;
    if (head > len) head = len;

    /* Unit direction, kept as a 1/1024 fixed-point pair. */
    int ux = (int)(dx * 1024 / len), uy = (int)(dy * 1024 / len);
    /* Shorten the shaft so it does not poke out of the head. */
    int sx = x1 - (int)((int64_t)ux * (head - thick) / 1024);
    int sy = y1 - (int)((int64_t)uy * (head - thick) / 1024);
    paint_line(im, x0, y0, sx, sy, rgb, thick);

    int bx = x1 - (int)((int64_t)ux * head / 1024);
    int by = y1 - (int)((int64_t)uy * head / 1024);
    int half = head / 2;
    int px = -uy, py = ux;                       /* perpendicular */
    int lx = bx + (int)((int64_t)px * half / 1024);
    int ly = by + (int)((int64_t)py * half / 1024);
    int rx = bx - (int)((int64_t)px * half / 1024);
    int ry = by - (int)((int64_t)py * half / 1024);
    paint_tri(im, 2*x1, 2*y1, 2*lx, 2*ly, 2*rx, 2*ry, rgb);
}

/* ---- highlight -------------------------------------------------------- */

/* Multiply, not paint. A highlighter that covers the text it marks is a
 * redaction; keeping the darker of the two channels leaves the words
 * readable through the colour, which is the entire point of the tool. */
void paint_highlight(PaintImg im, int x0, int y0, int x1, int y1,
                     uint32_t rgb) {
    paint_clamp_rect(im, &x0, &y0, &x1, &y1);
    int hr = (int)((rgb >> 16) & 0xFF);
    int hg = (int)((rgb >>  8) & 0xFF);
    int hb = (int)( rgb        & 0xFF);
    for (int y = y0; y <= y1; y++) {
        uint8_t* p = im.px + ((size_t)y * im.w + x0) * 4;
        for (int x = x0; x <= x1; x++, p += 4) {
            p[0] = (uint8_t)(p[0] * hr / 255);
            p[1] = (uint8_t)(p[1] * hg / 255);
            p[2] = (uint8_t)(p[2] * hb / 255);
        }
    }
}

/* ---- obfuscation ------------------------------------------------------ */

/* Block average. Deliberately NOT a blur: a blur can be undone by
 * deconvolution and has been, repeatedly, on published screenshots. Averaging
 * a block throws the information away for good. */
void paint_pixelate(PaintImg im, int x0, int y0, int x1, int y1, int block) {
    paint_clamp_rect(im, &x0, &y0, &x1, &y1);
    if (block < 2) block = 2;
    for (int by = y0; by <= y1; by += block) {
        for (int bx = x0; bx <= x1; bx += block) {
            int ex = bx + block - 1, ey = by + block - 1;
            if (ex > x1) ex = x1;
            if (ey > y1) ey = y1;
            uint32_t sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = by; y <= ey; y++) {
                uint8_t* p = im.px + ((size_t)y * im.w + bx) * 4;
                for (int x = bx; x <= ex; x++, p += 4) {
                    sr += p[0]; sg += p[1]; sb += p[2]; n++;
                }
            }
            if (!n) continue;
            uint8_t r = (uint8_t)(sr / n), g = (uint8_t)(sg / n), b = (uint8_t)(sb / n);
            for (int y = by; y <= ey; y++) {
                uint8_t* p = im.px + ((size_t)y * im.w + bx) * 4;
                for (int x = bx; x <= ex; x++, p += 4) {
                    p[0] = r; p[1] = g; p[2] = b;
                }
            }
        }
    }
}

/* ---- step counter ----------------------------------------------------- */

/* Filled disc with a number in it. The digits are drawn by the caller, which
 * owns the font; this draws the disc and reports where the text goes. */
void paint_disc(PaintImg im, int cx, int cy, int radius, uint32_t rgb) {
    if (radius < 1) radius = 1;
    int bx0 = cx - radius - 1, bx1 = cx + radius + 1;
    int by0 = cy - radius - 1, by1 = cy + radius + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > im.w - 1) bx1 = im.w - 1;
    if (by1 > im.h - 1) by1 = im.h - 1;
    int ccx = 2 * cx + 1, ccy = 2 * cy + 1, r2 = 2 * radius;
    for (int y = by0; y <= by1; y++) {
        int py = 2 * y + 1 - ccy;
        for (int x = bx0; x <= bx1; x++) {
            int px = 2 * x + 1 - ccx;
            int d = paint_isqrt((int64_t)px * px + (int64_t)py * py);
            int cov = paint_cov(d, r2);
            if (cov) paint_blend(im, x, y, rgb, cov);
        }
    }
}

/* ---- text ------------------------------------------------------------- */

/* Blit an 8-bit coverage mask -- from the OS font engine, or from the
 * built-in bitmap font -- so text is one code path whatever produced it. */
void paint_mask(PaintImg im, int x, int y,
                const uint8_t* mask, int mw, int mh, uint32_t rgb) {
    if (!mask) return;
    for (int my = 0; my < mh; my++) {
        int ty = y + my;
        if (ty < 0 || ty >= im.h) continue;
        for (int mx = 0; mx < mw; mx++) {
            int cov = mask[(size_t)my * mw + mx];
            if (cov) paint_blend(im, x + mx, ty, rgb, cov);
        }
    }
}

/* ---- crop ------------------------------------------------------------- */

uint8_t* paint_crop(PaintImg im, int x0, int y0, int x1, int y1,
                    int* out_w, int* out_h) {
    paint_clamp_rect(im, &x0, &y0, &x1, &y1);
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w < 1 || h < 1) return NULL;
    uint8_t* out = (uint8_t*)malloc((size_t)w * h * 4);
    if (!out) return NULL;
    for (int y = 0; y < h; y++)
        memcpy(out + (size_t)y * w * 4,
               im.px + ((size_t)(y + y0) * im.w + x0) * 4, (size_t)w * 4);
    *out_w = w; *out_h = h;
    return out;
}

#endif /* PAINT_H */
