"""What the phone apps (ios/prepare.py, android/prepare.py) share: the game recompiled, its
controls calibrated on this computer, the files it reads staged for the app, its icon and name.

A phone cannot start another program (the first-run calibration runs the game a second time,
headless), so the desktop executable calibrates here (--calibrate) and the app takes that NVRAM at
its first launch. The staged files are what the executable reads beside itself, at the same paths.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402

CALIBRATED_NVRAM = 'calibrated_nvram.bin'   # runtime/main.c, MOBILE_CALIBRATED_NVRAM
ICONS = os.path.join(ROOT, 'docs', 'images')
PORT_ICON = os.path.join(ICONS, 'viper-recomp-icon.png')


def run(cmd, **kw):
    print('+ ' + ' '.join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=kw.pop('cwd', ROOT), **kw)


def config_defines(gid):
    """The #defines of generated/<id>/game_config.h that are strings."""
    text = open(os.path.join(ROOT, 'generated', gid, 'game_config.h')).read()
    return dict(re.findall(r'^#define (\w+) "((?:[^"\\]|\\.)*)"', text, re.M))


def game_icon(gid):
    """docs/images/icon-<game>.png, the game's logo from the port's icon (a version of a game,
    thrild2j or gticlub2ea, takes its game's); the port's own icon for a game without one"""
    names = sorted((f[5:-4] for f in os.listdir(ICONS) if f.startswith('icon-') and f.endswith('.png')),
                   key=len, reverse=True)
    name = next((n for n in names if gid.startswith(n)), None)
    return os.path.join(ICONS, f'icon-{name}.png') if name else PORT_ICON


def home_name(g):
    """the name under the icon: the title, or the first other name short enough not to be cut"""
    names = [g.get('title', g['binary'])] + g.get('aka', [])
    return next((n for n in names if len(n) <= 14), names[0])


def version():
    """the port's version: the last released section of CHANGELOG.md"""
    m = re.search(r'^## \[(\d+\.\d+\.\d+)\]', open(os.path.join(ROOT, 'CHANGELOG.md')).read(), re.M)
    return m.group(1) if m else '0.0.0'


def stage(gid, data):
    """The game recompiled (make extract, make recomp if not yet: the files in roms/ are needed),
    the desktop executable built and calibrating into data/calibrated_nvram.bin, and the files the
    executable reads copied into data/ at their paths. Returns those paths, relative to data/."""
    g = game.load(gid)
    if not os.path.exists(os.path.join(ROOT, 'work', gid, 'kernel.bin')):
        run(['make', 'extract', 'GAME=' + gid])
    if not os.path.exists(os.path.join(ROOT, 'generated', gid, 'sources.mk')):
        run(['make', 'recomp', 'GAME=' + gid])
    d = config_defines(gid)
    run(['make', '-j%d' % (os.cpu_count() or 4), 'GAME=' + gid])
    if os.path.isdir(data):
        shutil.rmtree(data)
    os.makedirs(data)
    run([os.path.join(ROOT, g['binary']), '--calibrate', os.path.join(data, CALIBRATED_NVRAM)])
    work = d['GAME_DEFAULT_WORK']
    files = [(os.path.join(work, 'kernel.bin'), True), (os.path.join(work, 'cf.img'), True),
             (d['GAME_DEFAULT_NVRAM'], True), (d['GAME_DEFAULT_DS2430'], True), (d['GAME_DEFAULT_BIOS'], False)]
    if d.get('GAME_ENH_FONT_FILE'):
        files.append((os.path.join(work, 'fs', d['GAME_ENH_FONT_FILE']), True))
    staged = [CALIBRATED_NVRAM]
    for rel, needed in files:
        src = os.path.join(ROOT, rel)
        if not os.path.exists(src):
            if needed:
                raise SystemExit(f'{rel} is missing (make check GAME={gid})')
            continue
        dst = os.path.join(data, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        staged.append(rel)
    print(f'staged the game data in {os.path.relpath(data, ROOT)}')
    return staged
