#!/usr/bin/env python3
"""The iOS app of a game: stages its data, calibrates its controls and generates its Xcode project.

usage: python3 ios/prepare.py [GAME] [--team TEAMID] [--bundle-id ID] [--build sim|device|ipa]

  GAME            the profile (games/<id>), default thrild2
  --team          the Apple development team that signs the app (Xcode > Settings > Accounts;
                  it can also be chosen in Xcode, under Signing & Capabilities)
  --bundle-id     the app's identifier (default: local.viper.<binary>.<random>, kept in
                  build/ios/<id>/bundle_id so that it stays the same)
  --build sim     also build it for the iOS Simulator (no signing needed)
  --build device  also build it for a device (needs --team)
  --build ipa     also build it for a device, unsigned, packed as build/ios/<id>/<binary>-unsigned.ipa
                  (for a sideloading tool that signs it with your Apple ID)

What it does:
  1. the game is extracted and recompiled if it is not yet (make extract, make recomp: the files
     in roms/ are needed for that, as for the desktop build);
  2. the desktop executable is built and runs the first-run calibration (--calibrate): on iOS a
     program cannot start another one, so the app takes this calibrated NVRAM on its first launch;
  3. the files the game reads (the kernel, the CF image, the menu font, the NVRAM, the DS2430 and
     the BIOS) are copied to build/ios/<id>/data/, with the paths the executable expects;
  4. the app's icon, a frame of the attract mode (the profile's ios_icon, or the default below);
  5. CMake generates build/ios/<id>/xcode/viper_ios.xcodeproj (ios/CMakeLists.txt).

The app holds the game's data: like everything the build makes from it, it is for your own
device only (see README.md, "Legal").
"""
import argparse
import os
import re
import secrets
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402


def run(cmd, **kw):
    print('+ ' + ' '.join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=ROOT, **kw)


def config_defines(gid):
    """The #defines of generated/<id>/game_config.h that are strings."""
    text = open(os.path.join(ROOT, 'generated', gid, 'game_config.h')).read()
    return dict(re.findall(r'^#define (\w+) "((?:[^"\\]|\\.)*)"', text, re.M))


# the icon: this frame of the attract mode (classic mode, from boot), and the square cropped from it
# (x, y, size, in its 512x384 pixels); Thrill Drive 2's is the car at night, its headlights on
DEFAULT_ICON = {'frame': 1200, 'crop': [112, 56, 288]}


def make_icon(g, out):
    """build/ios/<id>/Assets.xcassets with the AppIcon: a frame of the game's own attract mode"""
    icon = g.get('ios_icon', DEFAULT_ICON)
    frames = os.path.join(out, 'icon-frames')
    shutil.rmtree(frames, ignore_errors=True)
    os.makedirs(frames)
    n = icon['frame']
    subprocess.run([os.path.join(ROOT, g['binary']), '--headless', '--classic', '--seconds', str(n / 30 + 5),
                    '--frames', frames, '--frame-every', str(n)], cwd=ROOT, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    ppm = os.path.join(frames, 'frame_%06d.ppm' % n)
    if not os.path.exists(ppm):
        print('no icon: the frame was not dumped')
        return None
    subprocess.check_call([sys.executable, '-I', os.path.join(ROOT, 'tools', 'ppm2png.py'), ppm])
    png = ppm[:-4] + '.png'
    x, y, size = icon['crop']
    iconset = os.path.join(out, 'Assets.xcassets', 'AppIcon.appiconset')
    os.makedirs(iconset, exist_ok=True)
    dst = os.path.join(iconset, 'icon.png')
    subprocess.check_call(['sips', '-c', str(size), str(size), '--cropOffset', str(y), str(x), png, '--out', dst],
                          stdout=subprocess.DEVNULL)
    subprocess.check_call(['sips', '-z', '1024', '1024', dst], stdout=subprocess.DEVNULL)
    open(os.path.join(iconset, 'Contents.json'), 'w').write(
        '{"images":[{"filename":"icon.png","idiom":"universal","platform":"ios","size":"1024x1024"}],'
        '"info":{"author":"xcode","version":1}}\n')
    open(os.path.join(out, 'Assets.xcassets', 'Contents.json'), 'w').write('{"info":{"author":"xcode","version":1}}\n')
    shutil.rmtree(frames, ignore_errors=True)
    return os.path.join(out, 'Assets.xcassets')


def make_ipa(app, out, binary):
    """build/ios/<id>/<binary>-unsigned.ipa: the app in Payload/, zipped (ditto keeps its modes)"""
    stage = os.path.join(out, 'ipa')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(os.path.join(stage, 'Payload'))
    run(['ditto', app, os.path.join(stage, 'Payload', os.path.basename(app))])
    ipa = os.path.join(out, binary + '-unsigned.ipa')
    if os.path.exists(ipa):
        os.remove(ipa)
    run(['ditto', '-c', '-k', '--norsrc', '--keepParent', os.path.join(stage, 'Payload'), ipa])
    shutil.rmtree(stage, ignore_errors=True)
    return ipa


def version():
    m = re.search(r'^## \[(\d+\.\d+\.\d+)\]', open(os.path.join(ROOT, 'CHANGELOG.md')).read(), re.M)
    return m.group(1) if m else '0.0.0'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('game', nargs='?', default='thrild2')
    ap.add_argument('--team', default='')
    ap.add_argument('--bundle-id', default='')
    ap.add_argument('--build', choices=('sim', 'device', 'ipa'))
    a = ap.parse_args()
    g = game.load(a.game)
    gid, binary = g['id'], g['binary']
    out = os.path.join(ROOT, 'build', 'ios', gid)
    data = os.path.join(out, 'data')
    os.makedirs(out, exist_ok=True)

    # 1. the recompiled game and its extracted data
    if not os.path.exists(os.path.join(ROOT, 'work', gid, 'kernel.bin')):
        run(['make', 'extract', 'GAME=' + gid])
    if not os.path.exists(os.path.join(ROOT, 'generated', gid, 'sources.mk')):
        run(['make', 'recomp', 'GAME=' + gid])
    d = config_defines(gid)

    # 2. the desktop executable, for the calibrated NVRAM
    run(['make', '-j%d' % (os.cpu_count() or 4), 'GAME=' + gid])
    if os.path.isdir(data):
        shutil.rmtree(data)
    os.makedirs(data)
    run([os.path.join(ROOT, binary), '--calibrate', os.path.join(data, 'calibrated_nvram.bin')])

    # 3. the files the executable reads, at its default paths (relative to the executable)
    work = d['GAME_DEFAULT_WORK']
    files = [(os.path.join(work, 'kernel.bin'), True), (os.path.join(work, 'cf.img'), True),
             (d['GAME_DEFAULT_NVRAM'], True), (d['GAME_DEFAULT_DS2430'], True), (d['GAME_DEFAULT_BIOS'], False)]
    if d.get('GAME_ENH_FONT_FILE'):
        files.append((os.path.join(work, 'fs', d['GAME_ENH_FONT_FILE']), True))
    for rel, needed in files:
        src = os.path.join(ROOT, rel)
        if not os.path.exists(src):
            if needed:
                raise SystemExit(f'{rel} is missing (make check GAME={gid})')
            continue
        dst = os.path.join(data, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
    print(f'staged the game data in {os.path.relpath(data, ROOT)}')
    assets = make_icon(g, out)

    # 4. the Xcode project
    bid = a.bundle_id
    bid_file = os.path.join(out, 'bundle_id')
    if not bid:
        if os.path.exists(bid_file):
            bid = open(bid_file).read().strip()
        else:
            bid = f'local.viper.{binary}.{secrets.token_hex(3)}'
    open(bid_file, 'w').write(bid + '\n')
    xcode = os.path.join(out, 'xcode')
    run(['cmake', '-G', 'Xcode', '-S', os.path.join(ROOT, 'ios'), '-B', xcode,
         '-DGAME=' + gid, '-DAPP_BINARY=' + binary, '-DAPP_TITLE=' + g.get('title', binary),
         '-DAPP_VERSION=' + version(), '-DVIPER_DATA=' + data, '-DBUNDLE_ID=' + bid,
         '-DDEVELOPMENT_TEAM=' + a.team, '-DAPP_ASSETS=' + (assets or '')])
    project = os.path.join(xcode, 'viper_ios.xcodeproj')
    print(f'\nXcode project: {os.path.relpath(project, ROOT)} (bundle id {bid})')

    if a.build:
        sdk = 'iphonesimulator' if a.build == 'sim' else 'iphoneos'
        cmd = ['xcodebuild', '-project', project, '-scheme', 'app', '-configuration', 'Release', '-sdk', sdk, 'build']
        if a.build in ('sim', 'ipa'):
            cmd.append('CODE_SIGNING_ALLOWED=NO')
        elif not a.team:
            raise SystemExit('--build device needs --team')
        app = os.path.join(xcode, 'Release-' + sdk, binary + '.app')
        if os.path.islink(app):         # left by an Archive in Xcode: the build cannot write through it
            os.remove(app)
        run(cmd)
        print(f'\nbuilt {os.path.relpath(app, ROOT)}')
        if a.build == 'ipa':
            print(f'unsigned IPA: {os.path.relpath(make_ipa(app, out, binary), ROOT)} (for your own devices only)')
    else:
        print('open it in Xcode, choose your iPhone and Run (with --team, or set the team under Signing & Capabilities)')


if __name__ == '__main__':
    main()
