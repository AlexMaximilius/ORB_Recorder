/* test_png.c -- round-trip the PNG writer through an independent decoder.
 *
 * Two distinct things can go wrong, and only one of them is visible:
 *
 *   - The file decodes to the wrong pixels. Caught by comparing every pixel
 *     against what was written.
 *   - The file decodes perfectly and is ten times too big, because the
 *     compressor quietly fell back to stored blocks. Nothing looks wrong;
 *     screenshots simply stop being sendable. That happened during
 *     development -- a stale copy of the header shadowed the real one and
 *     every output was uncompressed -- so the ratio is asserted, not printed.
 *
 * Decoding here uses the vendored stb_image, which is not the encoder. The
 * companion verify_png.py adds zlib and PIL, which are not ours at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PNG_WRITE_IMPLEMENTATION
#include "png_write.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int failures = 0;

static long fsize(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* Write, read back, compare. max_pct is the largest the file may be as a
 * percentage of the raw RGBA bytes; 0 means do not check the size. */
static void roundtrip(const char* name, uint8_t* px, int w, int h,
                      int has_alpha, int max_pct) {
    char path[256];
    snprintf(path, sizeof path, "tests_%s.png", name);
    if (!png_write_rgba(path, px, w, h)) {
        printf("  FAIL  %-12s could not be written\n", name); failures++; return;
    }
    int dw = 0, dh = 0, dc = 0;
    uint8_t* back = stbi_load(path, &dw, &dh, &dc, 4);
    if (!back) {
        printf("  FAIL  %-12s did not decode (%s)\n", name, stbi_failure_reason());
        failures++; remove(path); return;
    }
    long bad = 0;
    if (dw == w && dh == h) {
        int n = has_alpha ? 4 : 3;
        for (long i = 0; i < (long)w * h; i++)
            for (int c = 0; c < n; c++)
                if (px[i * 4 + c] != back[i * 4 + c]) { bad++; break; }
    }
    long raw = (long)w * h * 4;
    long got = fsize(path);
    int pct = (int)(got * 100 / (raw ? raw : 1));

    if (dw != w || dh != h) {
        printf("  FAIL  %-12s came back %dx%d, not %dx%d\n", name, dw, dh, w, h);
        failures++;
    } else if (bad) {
        printf("  FAIL  %-12s %ld pixels differ after a round trip\n", name, bad);
        failures++;
    } else if (max_pct && pct > max_pct) {
        printf("  FAIL  %-12s %ld bytes = %d%% of raw, over the %d%% ceiling"
               " -- compression is not running\n", name, got, pct, max_pct);
        failures++;
    } else {
        printf("  ok    %-12s %5dx%-5d %8ld -> %7ld bytes (%d%%)\n",
               name, w, h, raw, got, pct);
    }
    stbi_image_free(back);
    remove(path);
}

int main(void) {
    printf("png_write.h\n");
    int W = 640, H = 460;
    uint8_t* im = (uint8_t*)malloc((size_t)W * H * 4);

    /* Shaped like a screenshot: flat panels, hard edges, text-ish speckle.
     * Flat regions are what row filtering and LZ77 both feed on, so this is
     * the case whose ratio actually matters. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t* p = im + ((size_t)y * W + x) * 4;
            int panel = (x / 120 + y / 90) % 3;
            uint8_t v = (uint8_t)(30 + panel * 40);
            p[0] = v; p[1] = (uint8_t)(v + 8); p[2] = (uint8_t)(v + 20); p[3] = 255;
            if (y % 90 < 3 || x % 120 < 2) { p[0] = 200; p[1] = 200; p[2] = 210; }
            if ((y % 90) > 40 && (y % 90) < 48 && (x * 7 + y * 3) % 11 < 4) {
                p[0] = 240; p[1] = 240; p[2] = 240;
            }
        }
    roundtrip("ui", im, W, H, 0, 12);

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t* p = im + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)(x * 255 / W); p[1] = (uint8_t)(y * 255 / H);
            p[2] = (uint8_t)((x + y) * 255 / (W + H)); p[3] = 255;
        }
    roundtrip("gradient", im, W, H, 0, 20);

    memset(im, 0x40, (size_t)W * H * 4);
    for (long i = 0; i < (long)W * H; i++) im[i * 4 + 3] = 255;
    roundtrip("solid", im, W, H, 0, 3);

    /* Incompressible. Must still be exact, and must not grow past the raw
     * RGB bytes -- that is the stored-block fallback doing its job. */
    unsigned seed = 12345;
    for (long i = 0; i < (long)W * H; i++) {
        for (int c = 0; c < 3; c++) {
            seed = seed * 1103515245u + 12345u;
            im[i * 4 + c] = (uint8_t)(seed >> 16);
        }
        im[i * 4 + 3] = 255;
    }
    roundtrip("noise", im, W, H, 0, 80);

    /* Real transparency has to survive as transparency. */
    for (long i = 0; i < (long)W * H; i++) {
        im[i * 4 + 0] = (uint8_t)(i % 251);
        im[i * 4 + 1] = 90;
        im[i * 4 + 2] = 200;
        im[i * 4 + 3] = (uint8_t)(i % 256);
    }
    roundtrip("alpha", im, W, H, 1, 0);
    free(im);

    /* Degenerate shapes, where an off-by-one in the filter loop lives. */
    uint8_t one[4] = { 10, 20, 30, 255 };
    roundtrip("1x1", one, 1, 1, 0, 0);

    uint8_t* strip = (uint8_t*)malloc(400 * 4);
    for (int i = 0; i < 400; i++) {
        strip[i*4+0] = (uint8_t)i; strip[i*4+1] = (uint8_t)(255 - i);
        strip[i*4+2] = 7;          strip[i*4+3] = 255;
    }
    roundtrip("1x400", strip, 1, 400, 0, 0);
    roundtrip("400x1", strip, 400, 1, 0, 0);
    free(strip);

    /* Larger than the 32K deflate window, so chain eviction is exercised. */
    int BW = 1280, BH = 720;
    uint8_t* big = (uint8_t*)malloc((size_t)BW * BH * 4);
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++) {
            uint8_t* p = big + ((size_t)y * BW + x) * 4;
            uint8_t v = (uint8_t)(((x >> 4) ^ (y >> 4)) & 0xFF);
            p[0] = v; p[1] = (uint8_t)(v / 2); p[2] = 60; p[3] = 255;
        }
    roundtrip("1280x720", big, BW, BH, 0, 8);
    free(big);

    printf(failures ? "\npng_write.h: %d FAILED\n" : "\npng_write.h: all good\n",
           failures);
    return failures ? 1 : 0;
}
