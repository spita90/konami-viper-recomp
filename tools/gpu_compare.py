#!/usr/bin/env python3
"""Compare the GPU renderer's frames (tools/gpu_replay.cpp: gpu_NNNNNN.ppm) with the software
rasterizer's frames of the same run (frame_NNNNNN.ppm), and write side-by-side images
(software | GPU | difference) as cmp_NNNNNN.ppm.

    tools/gpu_compare.py frames_dir gpu_dir
"""
import os
import sys


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def main():
    frames, gpu = sys.argv[1], sys.argv[2]
    for name in sorted(os.listdir(gpu)):
        if not name.startswith('gpu_'):
            continue
        n = name[4:-4]
        soft = os.path.join(frames, 'frame_%s.ppm' % n)
        if not os.path.exists(soft):
            continue
        sw, sh, s = read_ppm(soft)
        gw, gh, g = read_ppm(os.path.join(gpu, name))
        if (sw, sh) != (gw, gh):
            print('%s: size %dx%d vs %dx%d' % (n, sw, sh, gw, gh))
            continue
        diff = bytearray(len(s))
        npix = sw * sh
        differ = big = 0
        for i in range(npix):
            d = max(abs(s[3 * i + k] - g[3 * i + k]) for k in range(3))
            if d:
                differ += 1
                if d > 24:
                    big += 1
            diff[3 * i:3 * i + 3] = bytes((min(255, d * 4),) * 3)
        print('%s: %5.1f%% of the pixels differ, %5.1f%% by more than 24' % (n, 100.0 * differ / npix, 100.0 * big / npix))
        out = bytearray()
        for y in range(sh):
            row = slice(3 * y * sw, 3 * (y + 1) * sw)
            out += s[row] + g[row] + diff[row]
        with open(os.path.join(gpu, 'cmp_%s.ppm' % n), 'wb') as f:
            f.write(b'P6\n%d %d\n255\n' % (sw * 3, sh))
            f.write(out)


if __name__ == '__main__':
    main()
