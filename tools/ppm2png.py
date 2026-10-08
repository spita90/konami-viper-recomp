#!/usr/bin/env python3
"""Convert PPM (P6) files to PNG, next to them: tools/ppm2png.py a.ppm [b.ppm ...]"""
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from gpu_compare import read_ppm  # noqa: E402

for path in sys.argv[1:]:
    w, h, rgb = read_ppm(path)
    raw = b''.join(b'\0' + rgb[3 * w * y:3 * w * (y + 1)] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    with open(path[:-4] + '.png', 'wb') as f:
        f.write(png)
