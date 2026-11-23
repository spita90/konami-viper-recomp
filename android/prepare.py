#!/usr/bin/env python3
"""The Android app of a game: stages its data, calibrates its controls, builds its APK.

usage: python3 android/prepare.py [GAME] [--install] [--app-id ID]

  GAME         the profile (games/<id>), default thrild2
  --install    also install the APK on the device connected (adb install)
  --app-id     the app's identifier (default local.viper.<binary>)

What it does:
  1-3. the recompiled game, its controls calibrated on this computer and the files it reads
       (tools/mobile.py, shared with the iOS app);
  4. SDL2's release sources, fetched once into build/android/ (the same release as the iOS app);
  5. the Gradle project build/android/<id>/project/, from SDL's android-project with this
     repository's android/ files (app.gradle, AndroidManifest.xml, java/, CMakeLists.txt); the
     game's files go into its assets, copied out of the APK at the app's first launch;
  6. Gradle builds build/android/<id>/<binary>.apk, signed with this computer's debug key.

It needs the Android SDK with the NDK (Android Studio's SDK Manager: "NDK (Side by side)"; found in
ANDROID_HOME, or Android Studio's default place) and JDK 17 (JAVA_HOME, or found on macOS).

The APK holds the game's data: like everything the build makes from it, it is for your own
devices only (see README.md, "Legal").
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game  # noqa: E402
from mobile import game_icon, home_name, run, stage, version  # noqa: E402

# the same release as ios/CMakeLists.txt
SDL_VERSION = '2.32.10'
SDL_URL = f'https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL2-{SDL_VERSION}.tar.gz'
SDL_SHA256 = '5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165'


def sdl_sources(base):
    """build/android/SDL2-<version>/, fetched and checked once"""
    src = os.path.join(base, f'SDL2-{SDL_VERSION}')
    if os.path.isdir(os.path.join(src, 'android-project')):
        return src
    tgz = src + '.tar.gz'
    if not os.path.exists(tgz):
        print(f'fetching {SDL_URL}', flush=True)
        urllib.request.urlretrieve(SDL_URL, tgz + '.part')
        os.replace(tgz + '.part', tgz)
    if hashlib.sha256(open(tgz, 'rb').read()).hexdigest() != SDL_SHA256:
        os.remove(tgz)
        raise SystemExit(f'{tgz}: not the expected file (SHA-256), removed: run again')
    with tarfile.open(tgz) as t:
        try:
            t.extractall(base, filter='data')
        except TypeError:               # Python before 3.12 (and its backports): the same checks by hand
            top = os.path.realpath(base)
            for m in t.getmembers():
                dst = os.path.realpath(os.path.join(base, m.name))
                if not (dst == top or dst.startswith(top + os.sep)) or not (m.isfile() or m.isdir()):
                    continue            # outside the folder, or a link or a device: not taken
                t.extract(m, base)
    return src


def android_sdk():
    for k in ('ANDROID_HOME', 'ANDROID_SDK_ROOT'):
        if os.environ.get(k) and os.path.isdir(os.environ[k]):
            return os.environ[k]
    home = os.path.expanduser('~')
    for p in (os.path.join(home, 'Library', 'Android', 'sdk'), os.path.join(home, 'Android', 'Sdk'),
              os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk')):
        if os.path.isdir(p):
            return p
    raise SystemExit('no Android SDK: set ANDROID_HOME (Android Studio > Settings > Android SDK shows where it is)')


def newest_ndk(sdk):
    ndks = sorted(glob.glob(os.path.join(sdk, 'ndk', '*')),
                  key=lambda p: [int(x) for x in os.path.basename(p).split('.') if x.isdigit()])
    if not ndks:
        raise SystemExit('no NDK in the Android SDK: install "NDK (Side by side)" with the SDK Manager')
    return os.path.basename(ndks[-1])


def jdk17():
    """Gradle 8.1 (SDL's android-project) runs on JDK 17"""
    if os.environ.get('JAVA_HOME'):
        return os.environ['JAVA_HOME']
    if sys.platform == 'darwin':
        r = subprocess.run(['/usr/libexec/java_home', '-v', '17'], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout.strip()
    raise SystemExit('no JDK 17: install one (e.g. Temurin 17) and set JAVA_HOME')


def make_project(sdl, proj, g, app_id, data, staged, sdk):
    """the Gradle project: SDL's android-project, then this repository's files over it"""
    template = os.path.join(sdl, 'android-project')
    for d, _, files in os.walk(template):
        rel = os.path.relpath(d, template)
        if rel.startswith(os.path.join('app', 'jni')) or os.path.basename(d).startswith('mipmap'):
            continue                    # SDL's own native build, its icons: not ours
        os.makedirs(os.path.join(proj, rel), exist_ok=True)
        for f in files:
            if rel == 'app' and f == 'build.gradle' or f == 'AndroidManifest.xml':
                continue
            shutil.copy2(os.path.join(d, f), os.path.join(proj, rel, f))
    gradlew = os.path.join(proj, 'gradlew')
    os.chmod(gradlew, 0o755)
    shutil.copyfile(os.path.join(ROOT, 'android', 'app.gradle'), os.path.join(proj, 'app', 'build.gradle'))
    props = open(os.path.join(template, 'gradle.properties')).read()
    sl = lambda p: p.replace('\\', '/')
    props += (f'\nviperRoot={sl(ROOT)}\nviperGame={g["id"]}\nviperSdl={sl(sdl)}\nviperAppId={app_id}\n'
              f'viperVersion={version()}\nviperNdk={newest_ndk(sdk)}\norg.gradle.jvmargs=-Xmx3g\n')
    open(os.path.join(proj, 'gradle.properties'), 'w').write(props)
    open(os.path.join(proj, 'local.properties'), 'w').write(f'sdk.dir={sl(sdk)}\n')
    res = os.path.join(proj, 'app', 'src', 'main', 'res')
    os.makedirs(os.path.join(res, 'mipmap-xxxhdpi'), exist_ok=True)   # one large icon: the launcher scales it
    shutil.copyfile(game_icon(g['id']), os.path.join(res, 'mipmap-xxxhdpi', 'ic_launcher.png'))
    name = home_name(g).replace('&', '&amp;').replace('<', '&lt;').replace("'", "\\'")
    open(os.path.join(res, 'values', 'strings.xml'), 'w').write(
        f'<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <string name="app_name">{name}</string>\n</resources>\n')
    # the game's files, and the list runtime/platform_android.c copies them out with
    assets = os.path.join(proj, 'app', 'src', 'main', 'assets')
    shutil.rmtree(assets, ignore_errors=True)
    stamp = hashlib.sha1()
    for rel in staged:
        dst = os.path.join(assets, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(data, rel), dst)
        st = os.stat(dst)
        stamp.update(f'{rel} {st.st_size} {st.st_mtime_ns}\n'.encode())
    open(os.path.join(assets, 'viper_files.txt'), 'w').write(
        f'{version()}-{stamp.hexdigest()[:12]}\n' + ''.join(sl(r) + '\n' for r in staged))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('game', nargs='?', default='thrild2')
    ap.add_argument('--install', action='store_true')
    ap.add_argument('--app-id', default='')
    a = ap.parse_args()
    g = game.load(a.game)
    gid, binary = g['id'], g['binary']
    base = os.path.join(ROOT, 'build', 'android')
    out = os.path.join(base, gid)
    data = os.path.join(out, 'data')
    os.makedirs(out, exist_ok=True)
    sdk = android_sdk()
    java = jdk17()

    staged = stage(gid, data)                              # 1-3
    sdl = sdl_sources(base)                                 # 4
    proj = os.path.join(out, 'project')                     # 5
    app_id = a.app_id or f'local.viper.{binary}'
    make_project(sdl, proj, g, app_id, data, staged, sdk)

    env = dict(os.environ, JAVA_HOME=java, ANDROID_HOME=sdk)  # 6
    gradlew = os.path.join(proj, 'gradlew.bat' if sys.platform == 'win32' else 'gradlew')
    run([gradlew, '--no-daemon', 'assembleRelease'], cwd=proj, env=env)
    apk = os.path.join(out, binary + '.apk')
    shutil.copyfile(os.path.join(proj, 'app', 'build', 'outputs', 'apk', 'release', 'app-release.apk'), apk)
    print(f'\nbuilt {os.path.relpath(apk, ROOT)} ({app_id}): for your own devices only')
    if a.install:
        adb = os.path.join(sdk, 'platform-tools', 'adb')
        run([adb, 'install', '-r', apk])
        print('installed: open it from the app drawer')


if __name__ == '__main__':
    main()
