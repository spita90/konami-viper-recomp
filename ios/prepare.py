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
     the BIOS) are copied to build/ios/<id>/data/, with the paths the executable expects
     (steps 1-3: tools/mobile.py, shared with the Android app);
  4. the app's icon, the game's logo (docs/images/icon-<game>.png);
  5. CMake generates build/ios/<id>/xcode/viper_ios.xcodeproj (ios/CMakeLists.txt).

The app holds the game's data: like everything the build makes from it, it is for your own
device only (see README.md, "Legal").
"""
import argparse
import os
import secrets
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402
from mobile import game_icon, home_name, run, stage, version  # noqa: E402


def make_icon(gid, out):
    """build/ios/<id>/Assets.xcassets with the AppIcon, at the 1024 x 1024 iOS wants"""
    iconset = os.path.join(out, 'Assets.xcassets', 'AppIcon.appiconset')
    os.makedirs(iconset, exist_ok=True)
    subprocess.check_call(['sips', '-s', 'format', 'png', '-z', '1024', '1024', game_icon(gid), '--out',
                           os.path.join(iconset, 'icon.png')], stdout=subprocess.DEVNULL)
    open(os.path.join(iconset, 'Contents.json'), 'w').write(
        '{"images":[{"filename":"icon.png","idiom":"universal","platform":"ios","size":"1024x1024"}],'
        '"info":{"author":"xcode","version":1}}\n')
    open(os.path.join(out, 'Assets.xcassets', 'Contents.json'), 'w').write('{"info":{"author":"xcode","version":1}}\n')
    return os.path.join(out, 'Assets.xcassets')


def make_ipa(app, out, binary):
    """build/ios/<id>/<binary>-unsigned.ipa: the app in Payload/, zipped (ditto keeps its modes)"""
    tmp = os.path.join(out, 'ipa')
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(os.path.join(tmp, 'Payload'))
    run(['ditto', app, os.path.join(tmp, 'Payload', os.path.basename(app))])
    ipa = os.path.join(out, binary + '-unsigned.ipa')
    if os.path.exists(ipa):
        os.remove(ipa)
    run(['ditto', '-c', '-k', '--norsrc', '--keepParent', os.path.join(tmp, 'Payload'), ipa])
    shutil.rmtree(tmp, ignore_errors=True)
    return ipa


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

    # 1-3. the recompiled game, the calibrated NVRAM, the files it reads (tools/mobile.py)
    stage(gid, data)
    assets = make_icon(gid, out)

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
    run(['cmake', '-G', 'Xcode', '-S', os.path.join(ROOT, 'ios'), '-B', xcode, '-DCMAKE_SYSTEM_NAME=iOS',
         '-DCMAKE_OSX_SYSROOT=iphoneos',
         '-DGAME=' + gid, '-DAPP_BINARY=' + binary, '-DAPP_TITLE=' + home_name(g),
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
        else:                           # automatic signing: the profile made or renewed as needed
            cmd.append('-allowProvisioningUpdates')
        # Release-<sdk>/ (CMake 3) or Release/ (CMake 4, for every SDK): the one this build wrote
        apps = [os.path.join(xcode, d, binary + '.app') for d in ('Release-' + sdk, 'Release')]
        for app in apps:
            if os.path.islink(app):     # left by an Archive in Xcode: the build cannot write through it
                os.remove(app)
        run(cmd)
        built = [p for p in apps if os.path.isdir(p)]
        if not built:
            raise SystemExit(f'no {binary}.app after the build (looked in {", ".join(apps)})')
        app = max(built, key=os.path.getmtime)
        print(f'\nbuilt {os.path.relpath(app, ROOT)}')
        if a.build == 'ipa':
            print(f'unsigned IPA: {os.path.relpath(make_ipa(app, out, binary), ROOT)} (for your own devices only)')
    else:
        print('open it in Xcode, choose your iPhone and Run (with --team, or set the team under Signing & Capabilities)')


if __name__ == '__main__':
    main()
