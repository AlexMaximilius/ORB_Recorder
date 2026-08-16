"""make_icon.py -- generate orb.ico (orange radial orb, multi-size).

Matches the OpenGL orb's idle color (1.0, 0.55, 0.10) with a subtle center
highlight and soft edge falloff. Multi-resolution so Windows picks the right
size for taskbar / start menu / alt-tab.
"""
from PIL import Image, ImageDraw
from pathlib import Path

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
OUT = Path(__file__).with_name("orb.ico")

# Base orange (matches orb_recorder.cpp ORB_IDLE)
R, G, B = 255, 140, 25


def make_orb(size: int) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = (size - 1) / 2.0
    r = size / 2.0 - 0.5
    # Concentric filled disks, brightest at center
    steps = max(size // 2, 8)
    for i in range(steps, 0, -1):
        rad = r * i / steps
        t = 1.0 - (i / steps)                     # 0 edge -> 1 center
        brightness = 0.65 + 0.35 * t
        cr = int(R * brightness)
        cg = int(G * brightness)
        cb = int(B * brightness)
        alpha = 255 if i < steps else 200
        d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad],
                  fill=(cr, cg, cb, alpha))
    # Tiny bright highlight top-left for depth
    hlr = max(size // 6, 2)
    hx = cx - r * 0.35
    hy = cy - r * 0.35
    d.ellipse([hx - hlr, hy - hlr, hx + hlr, hy + hlr],
              fill=(255, 220, 180, 180))
    return img


def main():
    base = make_orb(SIZES[-1])   # largest
    sizes = [(s, s) for s in SIZES]
    base.save(OUT, format="ICO", sizes=sizes)
    print(f"wrote {OUT} with sizes {SIZES}")


if __name__ == "__main__":
    main()
