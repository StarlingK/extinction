import struct
import sys
import zlib


def section(xbe, want):
    base = struct.unpack_from('<I', xbe, 0x104)[0]
    count = struct.unpack_from('<I', xbe, 0x11C)[0]
    table = struct.unpack_from('<I', xbe, 0x120)[0] - base
    for i in range(count):
        h = table + i * 0x38
        raw, size, name = struct.unpack_from('<III', xbe, h + 12)
        n = name - base
        if xbe[n:xbe.index(b'\0', n)].decode('ascii', 'replace') == want:
            return xbe[raw:raw + size]
    raise SystemExit('section %s not found' % want)


def rgb565(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def dxt1(data, w, h):
    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1, bits = struct.unpack_from('<HHI', data, o)
            o += 8
            a, b = rgb565(c0), rgb565(c1)
            if c0 > c1:
                pal = [a + (255,), b + (255,),
                       tuple((2 * a[k] + b[k]) // 3 for k in range(3)) + (255,),
                       tuple((a[k] + 2 * b[k]) // 3 for k in range(3)) + (255,)]
            else:
                pal = [a + (255,), b + (255,),
                       tuple((a[k] + b[k]) // 2 for k in range(3)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                px[by + i // 4][bx + i % 4] = pal[bits >> (2 * i) & 3]
    return px


def scale(px, n):
    h, w = len(px), len(px[0])
    out = []
    for y in range(n):
        row = []
        for x in range(n):
            x0, x1 = x * w // n, max(x * w // n + 1, (x + 1) * w // n)
            y0, y1 = y * h // n, max(y * h // n + 1, (y + 1) * h // n)
            acc = [0, 0, 0, 0]
            cnt = 0
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    p = px[yy][xx]
                    for k in range(4):
                        acc[k] += p[k]
                    cnt += 1
            row.append(tuple(v // cnt for v in acc))
        out.append(row)
    return out


def png(px):
    n = len(px)
    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in px)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    xbe = open(sys.argv[1], 'rb').read()
    img = section(xbe, '$$XTIMAGE')
    hdr = struct.unpack_from('<I', img, 8)[0]
    fmt = struct.unpack_from('<I', img, 24)[0]
    if (fmt >> 8) & 0xFF != 0x0C:
        raise SystemExit('unexpected title image format')
    w, h = 1 << (fmt >> 20 & 15), 1 << (fmt >> 24 & 15)
    px = dxt1(img[hdr:], w, h)
    sizes = [256, 64, 48, 32, 16]
    imgs = [png(scale(px, s)) for s in sizes]
    out = struct.pack('<HHH', 0, 1, len(imgs))
    off = 6 + 16 * len(imgs)
    for s, d in zip(sizes, imgs):
        out += struct.pack('<BBBBHHII', s & 255, s & 255, 0, 0, 1, 32, len(d), off)
        off += len(d)
    open(sys.argv[2], 'wb').write(out + b''.join(imgs))


main()
