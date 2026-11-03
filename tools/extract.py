#!/usr/bin/env python3
"""Full asset pipeline:  roms/<game>/<chd> -> work/<game>/

  cf.img         raw CF card image (chdman extracthd)
  cf/GMxxxxx.BIN the single file on the card's FAT16 partition
  kernel.bin     boot block payload (resident kernel, runs at RAM 0)
  bios.bin       BIOS 941B01 (only its exception vectors are recompiled)
  fs/...         internal filesystem of the game file, decompressed

usage: extract.py <game id>      (see games/, e.g. thrild2)
"""
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import fat_extract   # noqa: E402
import game          # noqa: E402
import viper_fs      # noqa: E402
from klzss import decompress   # noqa: E402



def boot_block(binimg):
    """Parse the boot-block descriptor at +0x100 (see BIOS 941B01 @ 0xFE12DC)."""
    d = struct.unpack_from('>8I', binimg, 0x100)
    hsum = sum(struct.unpack_from('>14H', binimg, 0x100)) & 0xffffffff
    if hsum != d[7]:
        raise SystemExit('boot block descriptor checksum mismatch')
    start_sector, length, bytesum, load, entry = d[2], d[3], d[4], d[5], d[6]
    raw = binimg[start_sector * 512: start_sector * 512 + length]
    if (sum(raw) & 0xffffffff) != bytesum:
        raise SystemExit('boot image checksum mismatch')
    if not load & 0x80000000:
        raise SystemExit('uncompressed boot images not supported')
    return decompress(raw)[0], load & 0x7fffffff, entry


def main(gid):
    g = game.load(gid)
    print(f"{g['title']} ({g['version']}): checking files in roms/")
    if not game.check(g):
        raise SystemExit('missing files: see README.md, "Required files"')
    chd, bios, work = game.file_path(g, 'chd'), game.file_path(g, 'bios'), game.work_dir(g)
    os.makedirs(work, exist_ok=True)
    img = os.path.join(work, 'cf.img')
    if not os.path.exists(img):
        game.chd_extracthd(chd, img)
    fat_extract.main(img, os.path.join(work, 'cf'))
    bins = [f for f in os.listdir(os.path.join(work, 'cf')) if f.upper().endswith('.BIN')]
    binpath = os.path.join(work, 'cf', bins[0])
    data = open(binpath, 'rb').read()
    kernel, load, entry = boot_block(data)
    print(f"boot block: {len(kernel)} bytes, load {load:#x}, entry {entry:#x}")
    open(os.path.join(work, 'kernel.bin'), 'wb').write(kernel)
    shutil.copyfile(bios, os.path.join(work, 'bios.bin'))
    names = [os.path.join(os.path.dirname(__file__), 'names.txt'),
             os.path.join(game.ROOT, 'games', gid, 'names.txt')]
    fs = os.path.join(work, 'fs')
    if os.path.isdir(fs):
        shutil.rmtree(fs)       # no stale files under old names (e.g. _unk/ entries named since)
    viper_fs.main(binpath, fs, names)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
