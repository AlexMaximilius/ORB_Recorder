"""Second opinion on the PNG writer, from code that is not ours.

test_png.c decodes with the vendored stb_image -- good, but stb and this
writer could in principle share a misunderstanding. zlib is the reference
deflate implementation and raises on a bad stream or a bad Adler-32 rather
than returning plausible bytes, and PIL is a third decoder again. Unfiltering
is written out longhand here so a filter-type bug cannot hide behind a
decoder that happens to be forgiving.

Optional: test.bat skips this when Python or Pillow is absent. It writes its
own sample rather than depending on the C test's leftovers.
"""
import struct, subprocess, sys, os, zlib

def chunks(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError("not a PNG signature")
    off = 8
    while off < len(data):
        ln = struct.unpack('>I', data[off:off+4])[0]
        typ = data[off+4:off+8]
        body = data[off+8:off+8+ln]
        crc = struct.unpack('>I', data[off+8+ln:off+12+ln])[0]
        if crc != (zlib.crc32(typ + body) & 0xFFFFFFFF):
            raise ValueError("CRC mismatch on chunk %s" % typ.decode('ascii', 'replace'))
        yield typ, body
        off += 12 + ln

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)

def decode(path):
    data = open(path, 'rb').read()
    idat, w, h, ctype = b'', None, None, None
    for typ, body in chunks(data):
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            if depth != 8:
                raise ValueError("expected 8-bit, got %d" % depth)
        elif typ == b'IDAT':
            idat += body
    raw = zlib.decompress(idat)          # validates the stream and the adler
    bpp = {2: 3, 6: 4}[ctype]
    stride = w * bpp
    if len(raw) != (stride + 1) * h:
        raise ValueError("filtered stream is %d bytes, expected %d"
                         % (len(raw), (stride + 1) * h))
    out, prev, p = bytearray(), bytearray(stride), 0
    for _ in range(h):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        for i in range(stride):
            a = line[i-bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i-bpp] if i >= bpp else 0
            if   ft == 0: v = line[i]
            elif ft == 1: v = line[i] + a
            elif ft == 2: v = line[i] + b
            elif ft == 3: v = line[i] + ((a + b) >> 1)
            elif ft == 4: v = line[i] + paeth(a, b, c)
            else: raise ValueError("unknown filter type %d" % ft)
            line[i] = v & 0xFF
        out += line
        prev = line
    return w, h, ctype, bpp, bytes(out), len(data)

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    sample = os.path.join(here, 'verify_sample.png')
    maker = os.path.join(here, 'make_sample.exe')
    if not os.path.exists(maker):
        maker = os.path.join(here, 'make_sample')
    if not os.path.exists(maker):
        print("  skipped: make_sample was not built")
        return 0
    subprocess.run([maker, sample], check=True)

    bad = 0
    try:
        w, h, ctype, bpp, pixels, nbytes = decode(sample)
        print("  ok    zlib accepts the stream and the checksum")
        print("  ok    %dx%d colour type %d, %d bytes" % (w, h, ctype, nbytes))
    except Exception as e:
        print("  FAIL  %s" % e)
        return 1

    try:
        from PIL import Image
    except ImportError:
        print("  note  Pillow not installed -- skipping the third decoder")
        os.remove(sample)
        return 0

    im = Image.open(sample); im.load()
    ref = im.convert('RGB' if bpp == 3 else 'RGBA').tobytes()
    if ref != pixels:
        print("  FAIL  PIL disagrees with the hand-written unfilter")
        bad += 1
    else:
        print("  ok    PIL agrees, pixel for pixel")
    os.remove(sample)
    return bad

if __name__ == '__main__':
    rc = main()
    print("\nindependent decoders: " + ("FAILED" if rc else "all good"))
    sys.exit(1 if rc else 0)
