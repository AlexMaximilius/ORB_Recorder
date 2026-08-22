/* test_paint.c -- invariants of the annotation rasteriser.
 *
 * These are properties a human eye is bad at checking. Whether an arrow looks
 * right is a judgement; whether it wrote one byte past the end of the buffer
 * is a fact, and the fact is the one that corrupts a screenshot.
 *
 * Every image is allocated with a guard band of a known byte on each side, so
 * any write outside the declared bounds is caught even when it lands inside
 * an allocation and would otherwise never show up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "paint.h"

static int failures = 0;

static void ok(const char* what, int cond) {
    if (!cond) { printf("  FAIL  %s\n", what); failures++; }
    else       { printf("  ok    %s\n", what); }
}

/* ---- guarded images --------------------------------------------------- */
#define GUARD 64
#define GUARD_BYTE 0xA5

static uint8_t* g_base;

static PaintImg mk(int w, int h, uint8_t fill) {
    size_t n = (size_t)w * h * 4;
    g_base = (uint8_t*)malloc(n + GUARD * 2);
    memset(g_base, GUARD_BYTE, n + GUARD * 2);
    memset(g_base + GUARD, fill, n);
    for (size_t i = 0; i < n; i += 4) g_base[GUARD + i + 3] = 255;
    PaintImg im; im.px = g_base + GUARD; im.w = w; im.h = h;
    return im;
}

static int guards_intact(PaintImg im) {
    size_t n = (size_t)im.w * im.h * 4;
    for (int i = 0; i < GUARD; i++)
        if (g_base[i] != GUARD_BYTE || g_base[GUARD + n + i] != GUARD_BYTE)
            return 0;
    return 1;
}

static void done(void) { free(g_base); g_base = NULL; }

static int px_at(PaintImg im, int x, int y, int c) {
    return im.px[((size_t)y * im.w + x) * 4 + c];
}
static long count_changed(PaintImg im, uint8_t from) {
    long n = 0;
    for (long i = 0; i < (long)im.w * im.h; i++)
        if (im.px[i*4] != from || im.px[i*4+1] != from || im.px[i*4+2] != from) n++;
    return n;
}

int main(void) {
    printf("paint.h\n");
    uint32_t RED = PAINT_RGB(255, 0, 0);

    /* --- a line marks both of its ends --- */
    {
        PaintImg im = mk(100, 100, 0);
        paint_line(im, 10, 10, 80, 60, RED, 3);
        ok("line covers its start", px_at(im, 10, 10, 0) > 100);
        ok("line covers its end",   px_at(im, 80, 60, 0) > 100);
        ok("line leaves the rest alone", px_at(im, 95, 95, 0) == 0);
        ok("line stays inside the buffer", guards_intact(im));
        done();
    }

    /* --- everything clips; nothing writes outside --- */
    {
        PaintImg im = mk(80, 60, 0);
        paint_line(im, -900, -900, 900, 900, RED, 9);
        paint_rect(im, -50, -50, 200, 200, RED, 7, false);
        paint_rect(im, -50, -50, 200, 200, RED, 1, true);
        paint_ellipse(im, -40, -40, 400, 400, RED, 5, false);
        paint_ellipse(im, -40, -40, 400, 400, RED, 1, true);
        paint_arrow(im, -100, 30, 500, 30, RED, 6);
        paint_highlight(im, -80, -80, 900, 900, PAINT_RGB(255, 255, 0));
        paint_pixelate(im, -80, -80, 900, 900, 7);
        paint_disc(im, -20, -20, 400, RED);
        ok("out-of-bounds drawing writes nothing outside", guards_intact(im));
        done();
    }

    /* --- nothing wraps onto the next row ---
     *
     * Shapes clamp their own bounding box before they reach paint_blend, so
     * this is an end-to-end check rather than a test of that guard: draw hard
     * against and past the right edge, and insist the left column stays
     * clean. The path that DOES reach paint_blend with an out-of-range x is
     * paint_mask, which every piece of text goes through -- see below. */
    {
        PaintImg im = mk(50, 50, 0);
        paint_rect(im, 45, 0, 200, 49, RED, 1, true);      /* runs off the right */
        paint_line(im, 49, 0, 49, 49, RED, 1);             /* exactly the last column */
        paint_line(im, 60, 0, 60, 49, RED, 3);             /* entirely past it */
        int left_clean = 1;
        for (int y = 0; y < 50; y++)
            if (px_at(im, 0, y, 0) != 0) left_clean = 0;
        ok("drawing past the right edge does not wrap onto the next row", left_clean);
        ok("the last column is still reachable", px_at(im, 49, 25, 0) > 100);
        done();
    }

    /* --- degenerate shapes must not hang or crash --- */
    {
        PaintImg im = mk(40, 40, 0);
        paint_line(im, 20, 20, 20, 20, RED, 1);
        paint_rect(im, 20, 20, 20, 20, RED, 1, false);
        paint_ellipse(im, 20, 20, 20, 20, RED, 1, false);
        paint_arrow(im, 20, 20, 20, 20, RED, 4);
        paint_pixelate(im, 20, 20, 20, 20, 1);      /* block < 2 */
        paint_disc(im, 20, 20, 0, RED);
        paint_line(im, 5, 5, 30, 30, RED, 0);       /* thickness < 1 */
        ok("degenerate shapes survive", guards_intact(im));
        done();
    }

    /* --- a filled rectangle fills exactly, and only, its rectangle --- */
    {
        PaintImg im = mk(60, 60, 0);
        paint_rect(im, 10, 20, 29, 39, RED, 1, true);
        int inside_all = 1, edge_clean = 1;
        for (int y = 20; y <= 39; y++)
            for (int x = 10; x <= 29; x++)
                if (px_at(im, x, y, 0) != 255) inside_all = 0;
        for (int y = 0; y < 60; y++)
            for (int x = 0; x < 60; x++)
                if ((x < 10 || x > 29 || y < 20 || y > 39) && px_at(im, x, y, 0) != 0)
                    edge_clean = 0;
        ok("filled rect covers its interior", inside_all);
        ok("filled rect touches nothing else", edge_clean);
        ok("filled rect count is exact", count_changed(im, 0) == 20 * 20);
        done();
    }

    /* --- inverted drags behave like the rectangle they describe --- */
    {
        PaintImg a = mk(50, 50, 0);
        paint_rect(a, 10, 10, 39, 39, RED, 1, true);
        long na = count_changed(a, 0);
        uint8_t* copy = (uint8_t*)malloc((size_t)50 * 50 * 4);
        memcpy(copy, a.px, (size_t)50 * 50 * 4);
        done();
        PaintImg b = mk(50, 50, 0);
        paint_rect(b, 39, 39, 10, 10, RED, 1, true);      /* dragged backwards */
        ok("a rectangle dragged backwards is the same rectangle",
           na == count_changed(b, 0) &&
           memcmp(copy, b.px, (size_t)50 * 50 * 4) == 0);
        free(copy);
        done();
    }

    /* --- the marker must not obliterate what it marks --- */
    {
        PaintImg im = mk(40, 40, 200);
        /* a dark mark to read through the highlight */
        paint_rect(im, 10, 10, 20, 20, PAINT_RGB(0, 0, 0), 1, true);
        paint_highlight(im, 0, 0, 39, 39, PAINT_RGB(255, 255, 0));
        ok("highlight keeps the page lighter than the ink",
           px_at(im, 30, 30, 0) > px_at(im, 15, 15, 0));
        ok("highlight tints (blue channel drops)", px_at(im, 30, 30, 2) == 0);
        ok("highlight leaves red alone", px_at(im, 30, 30, 0) == 200);
        done();
    }

    /* --- obfuscation must actually destroy the detail --- */
    {
        PaintImg im = mk(64, 64, 0);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                if ((x / 2 + y / 2) & 1)
                    paint_rect(im, x, y, x, y, PAINT_RGB(255, 255, 255), 1, true);
        paint_pixelate(im, 0, 0, 63, 63, 16);
        int uniform = 1;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                if (px_at(im, x, y, 0) != px_at(im, 0, 0, 0)) uniform = 0;
        ok("a pixelated block is one flat colour", uniform);
        ok("pixelation is not a no-op", px_at(im, 0, 0, 0) != 0 &&
                                        px_at(im, 0, 0, 0) != 255);
        done();
    }

    /* --- crop --- */
    {
        PaintImg im = mk(100, 80, 0);
        paint_rect(im, 30, 20, 30, 20, PAINT_RGB(1, 2, 3), 1, true);
        int cw = 0, ch = 0;
        uint8_t* out = paint_crop(im, 30, 20, 59, 49, &cw, &ch);
        ok("crop returns the asked-for size", out && cw == 30 && ch == 30);
        ok("crop lands on the right pixel", out && out[0] == 1 && out[1] == 2);
        free(out);
        cw = ch = 0;
        out = paint_crop(im, -10, -10, 4, 4, &cw, &ch);
        ok("crop clamps to the image", out && cw == 5 && ch == 5);
        free(out);
        out = paint_crop(im, 500, 500, 600, 600, &cw, &ch);
        ok("crop entirely outside returns nothing", out == NULL);
        free(out);
        done();
    }

    /* --- a disc is round: symmetric about both axes --- */
    {
        PaintImg im = mk(81, 81, 0);
        paint_disc(im, 40, 40, 30, RED);
        int sym = 1;
        for (int y = 0; y < 81; y++)
            for (int x = 0; x < 81; x++) {
                if (px_at(im, x, y, 0) != px_at(im, 80 - x, y, 0)) sym = 0;
                if (px_at(im, x, y, 0) != px_at(im, x, 80 - y, 0)) sym = 0;
            }
        ok("disc is symmetric", sym);
        ok("disc centre is solid", px_at(im, 40, 40, 0) == 255);
        ok("disc corner is empty", px_at(im, 2, 2, 0) == 0);
        done();
    }

    /* --- masks (the path all text goes through) --- */
    {
        PaintImg im = mk(40, 40, 0);
        uint8_t mask[9] = { 0,255,0, 255,255,255, 0,255,0 };
        paint_mask(im, 5, 5, mask, 3, 3, RED);
        ok("mask blits where told", px_at(im, 6, 6, 0) == 255 && px_at(im, 5, 5, 0) == 0);
        paint_mask(im, -1, -1, mask, 3, 3, RED);       /* partly off the edge */
        paint_mask(im, 5, 5, NULL, 3, 3, RED);         /* no mask at all */
        ok("mask clips at the edges", guards_intact(im));
        done();
    }

    /* --- the call that actually reaches paint_blend out of range ---
     *
     * paint_mask is the only one that hands paint_blend an x past the right
     * edge, and every piece of text goes through it. A bounds check reading
     * x > w instead of x >= w lets that column through, and it lands on
     * column 0 of the NEXT row -- inside the allocation, so no guard byte
     * moves and the damage shows up as a stray dot down the left edge of
     * somebody's screenshot.
     *
     * Its own block: mk() owns a single guard pointer, so two live images
     * would have the second one checking the first one's guards. */
    {
        PaintImg im = mk(40, 40, 0);
        uint8_t solid[9] = { 255,255,255, 255,255,255, 255,255,255 };
        paint_mask(im, 39, 10, solid, 3, 3, RED);      /* two columns overhang */
        int left_clean = 1;
        for (int y = 0; y < 40; y++)
            if (px_at(im, 0, y, 0) != 0) left_clean = 0;
        ok("a mask overhanging the right edge does not wrap", left_clean);
        ok("the last column still takes the mask", px_at(im, 39, 11, 0) == 255);
        ok("overhanging mask stays in the buffer", guards_intact(im));
        done();
        done();
    }

    printf(failures ? "\npaint.h: %d FAILED\n" : "\npaint.h: all good\n", failures);
    return failures ? 1 : 0;
}
