/* Writes one PNG for verify_png.py to pick apart with zlib and PIL.
 * Content is deliberately mixed -- flat panels, an anti-aliased shape and a
 * gradient -- so every row filter gets chosen at least once. */
#include <stdio.h>
#include <stdlib.h>
#define PNG_WRITE_IMPLEMENTATION
#include "png_write.h"
#include "paint.h"

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "verify_sample.png";
    int W = 300, H = 200;
    uint8_t* px = (uint8_t*)malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t* p = px + ((size_t)y * W + x) * 4;
            int panel = (x / 60 + y / 50) % 3;
            p[0] = (uint8_t)(20 + panel * 50 + x / 8);   /* gradient: Sub */
            p[1] = (uint8_t)(40 + panel * 30);           /* flat:     None/Up */
            p[2] = (uint8_t)(90 + y / 4);                /* vertical: Up */
            p[3] = 255;
        }
    PaintImg im; im.px = px; im.w = W; im.h = H;
    paint_ellipse(im, 40, 40, 200, 150, PAINT_RGB(240, 30, 30), 5, false);
    paint_arrow(im, 20, 180, 270, 30, PAINT_RGB(255, 255, 255), 3);
    int rc = png_write_rgba(path, px, W, H) ? 0 : 1;
    free(px);
    if (rc) fprintf(stderr, "could not write %s\n", path);
    return rc;
}
