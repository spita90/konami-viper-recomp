#!/bin/sh
# Makes the Linux x86-64 release folder and its .tar.xz (the CI runs it; see dist/launcher.py):
#
#   dist/build-release.sh [VERSION]      -> build/release/konami-viper-<VERSION>-linux-x86_64.tar.xz
#
# Needs: python3 with PyInstaller, cmake and SDL2's build dependencies (see the workflow), curl.
# Nothing in it comes from the games: the launcher builds them on the player's computer.
set -eu

VERSION=${1:-$(git describe --tags --always --dirty)}
VERSION=${VERSION#v}
ZIG_VERSION=0.16.0
ZIG_SHA256=70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
SDL2_VERSION=2.32.10
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/release
NAME=konami-viper-$VERSION-linux-x86_64
DIR=$OUT/$NAME
DL=$OUT/downloads
rm -rf "$DIR" && mkdir -p "$DIR/engine/src" "$DIR/engine/sdl2" "$DL"

fetch() {   # url sha256 file
    [ -f "$DL/$3" ] || curl -fsSL -o "$DL/$3" "$1"
    echo "$2  $DL/$3" | sha256sum -c -
}

# the C compiler
fetch "https://ziglang.org/download/$ZIG_VERSION/zig-x86_64-linux-$ZIG_VERSION.tar.xz" $ZIG_SHA256 zig.tar.xz
mkdir -p "$DIR/engine/zig" && tar -xJf "$DL/zig.tar.xz" -C "$DIR/engine/zig" --strip-components=1
# only x86-64 Linux with glibc is needed: the other targets' libc headers and sources go
( cd "$DIR/engine/zig" && rm -rf doc lib/docs lib/build-web lib/libtsan \
    lib/libc/mingw lib/libc/wasi lib/libc/musl lib/libc/darwin lib/libc/freebsd lib/libc/netbsd lib/libc/openbsd &&
  cd lib/libc/include && for d in *; do
      case $d in any-linux-any|generic-glibc|x86-linux-any|x86-linux-gnu) ;; *) rm -rf "$d" ;; esac
  done )

# SDL2: headers to compile against, and a library for the systems that have none
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz" \
    $SDL2_SHA256 sdl2.tar.gz
rm -rf "$OUT/sdl2-src" && mkdir -p "$OUT/sdl2-src" && tar -xzf "$DL/sdl2.tar.gz" -C "$OUT/sdl2-src" --strip-components=1
rm -rf "$OUT/sdl2-build" "$OUT/sdl2-install"
cmake -S "$OUT/sdl2-src" -B "$OUT/sdl2-build" -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$OUT/sdl2-install" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
cmake --build "$OUT/sdl2-build" -j"$(nproc)" >/dev/null
cmake --install "$OUT/sdl2-build" >/dev/null
mkdir -p "$DIR/engine/sdl2/include" "$DIR/engine/sdl2/lib"
cp -R "$OUT/sdl2-install/include/SDL2" "$DIR/engine/sdl2/include/"
cp -L "$OUT/sdl2-install/lib/libSDL2-2.0.so.0" "$DIR/engine/sdl2/lib/"
cp "$OUT/sdl2-src/LICENSE.txt" "$DIR/engine/sdl2/LICENSE.txt"

# the sources compiled on the player's computer
cp -R "$ROOT/runtime" "$ROOT/third_party" "$DIR/engine/src/"
mkdir -p "$DIR/engine/src/tools" && cp "$ROOT/tools/chdextract.c" "$DIR/engine/src/tools/"
cp "$ROOT/LICENSE" "$DIR/engine/src/"
echo "$VERSION" > "$DIR/engine/VERSION"

# the launcher, with the python tools and the game profiles inside
python3 -m PyInstaller --onefile --clean --noconfirm --name viper-setup \
    --distpath "$DIR" --workpath "$OUT/pyinstaller" --specpath "$OUT/pyinstaller" \
    --add-data "$ROOT/tools:tools" --add-data "$ROOT/recomp:recomp" --add-data "$ROOT/games:games" \
    --hidden-import struct --hidden-import re \
    "$ROOT/dist/launcher.py"

# the folders for the player's files, and the instructions
mkdir -p "$DIR/roms"
cp "$ROOT/dist/README.txt" "$DIR/README.txt"
cp "$ROOT/LICENSE" "$DIR/LICENSE.txt"
( cd "$ROOT" && python3 -c "
import sys; sys.path.insert(0, 'tools'); import game
for i in game.ids():
    g = game.load(i)
    print(f\"{g['mame_set']:11s} {g['title']} ({g['version']})\")
    for f in g['files'].values():
        print(f\"    {f['name']:18s} {f.get('sha1') or '(optional)'}\")
" ) > "$DIR/roms/FILES.txt"

XZ_OPT=-9T0 tar -cJf "$OUT/$NAME.tar.xz" -C "$OUT" "$NAME"
echo "$OUT/$NAME.tar.xz"
