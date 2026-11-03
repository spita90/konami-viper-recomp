#!/usr/bin/env python3
"""viper-setup: builds the games of a release on the player's own computer.

A release holds no game code: only this launcher, the sources of the runtime and a C compiler
(zig). From the player's dumps it does what `make game` does in the repository: it checks the
files, extracts the game, recompiles it to C and compiles it, and leaves the executable (td2,
gticlub2, ...) next to itself.

  viper-setup                  build every game whose files are found
  viper-setup GAME ...         only these (MAME set names: thrild2, gticlub2, ...)
  viper-setup --roms DIR       also look for the files in DIR (repeatable)
  viper-setup --list           show which games are found, and what is missing
  viper-setup --rebuild        rebuild even the games that are up to date

The files may be anywhere under roms/ (next to this program) or under a --roms folder, in any
layout, loose or in MAME's zips: they are recognised by their name and SHA1.

Release layout (dist/build-release.sh makes it):
  viper-setup          this program (frozen with PyInstaller; tools/, recomp/ and games/ inside)
  roms/                the player's files
  engine/zig/          the C compiler
  engine/sdl2/         SDL2 headers and library
  engine/src/          runtime/, third_party/, tools/chdextract.c
  engine/VERSION       the release version (a change rebuilds the games)
  engine/data/         made here: the files found, work/, generated/, build/, the compiler cache
"""
import argparse
import concurrent.futures
import hashlib
import io
import json
import os
import platform
import shutil
import subprocess
import sys
import time
import zipfile

FROZEN = getattr(sys, 'frozen', False)
# the python sources (tools/, recomp/, games/): inside the frozen program, else the repository
PYROOT = getattr(sys, '_MEIPASS', None) or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.dirname(os.path.abspath(sys.executable if FROZEN else __file__))
if not FROZEN:
    HOME = os.environ.get('VIPER_HOME') or os.path.join(PYROOT, 'build', 'release-test')
ENGINE = os.environ.get('VIPER_ENGINE') or os.path.join(HOME, 'engine')
DATA = os.path.join(ENGINE, 'data')
# the C sources: engine/src/ in a release, the repository when run from it
SRC = os.path.join(ENGINE, 'src') if os.path.isdir(os.path.join(ENGINE, 'src')) else PYROOT

os.environ['VIPER_DATA'] = DATA
os.environ['VIPER_BINDIR'] = HOME
sys.path.insert(0, os.path.join(PYROOT, 'tools'))
sys.path.insert(0, os.path.join(PYROOT, 'recomp'))
import game      # noqa: E402
import extract   # noqa: E402
import recomp    # noqa: E402

# Linux and macOS for now; Windows will need its own SDL2 linking and executables
SYSTEM = platform.system()
if SYSTEM not in ('Linux', 'Darwin'):
    raise SystemExit('viper-setup: only Linux and macOS are supported for now')
EXE = ''


def log(msg=''):
    print(msg, flush=True)


def version():
    p = os.path.join(ENGINE, 'VERSION')
    return open(p).read().strip() if os.path.exists(p) else 'dev'


# ---------------------------------------------------------------------------------------------
# finding the player's files

def sha1(data):
    return hashlib.sha1(data).hexdigest()


class Candidate:
    """A file found on disk or in a zip, with the expected name."""

    def __init__(self, name, path, member=None):
        self.name, self.path, self.member = name, path, member
        self._sha1 = None

    def read(self):
        if self.member is None:
            return open(self.path, 'rb').read()
        with zipfile.ZipFile(self.path) as z:
            return z.read(self.member)

    def sha1(self, chd):
        if self._sha1 is None:
            self._sha1 = game.chd_sha1(self.path) if chd else sha1(self.read())
        return self._sha1

    def where(self):
        return f"{self.path}:{self.member}" if self.member else self.path


def scan(dirs, wanted):
    """{lowercase name: [Candidate]} for the files named in `wanted`, under the folders."""
    found = {}
    for top in dirs:
        for root, subdirs, files in os.walk(top):
            subdirs[:] = [d for d in subdirs if not d.startswith('.')]
            for f in files:
                p = os.path.join(root, f)
                if f.lower() in wanted:
                    found.setdefault(f.lower(), []).append(Candidate(f, p))
                elif f.lower().endswith('.zip'):
                    try:
                        with zipfile.ZipFile(p) as z:
                            for m in z.namelist():
                                b = os.path.basename(m).lower()
                                if b in wanted:
                                    found.setdefault(b, []).append(Candidate(b, p, m))
                    except (zipfile.BadZipFile, OSError):
                        pass
    return found


def resolve(g, found):
    """{key: Candidate or None} for the game's files; the candidate with the right SHA1 wins,
    else the first one with the right name (as `make check`, a different SHA1 only warns)."""
    out = {}
    for key, f in g['files'].items():
        cands = found.get(f['name'].lower(), [])
        chd = f['name'].lower().endswith('.chd')
        good = [c for c in cands if f.get('sha1') and c.sha1(chd) == f['sha1']]
        out[key] = (good or cands or [None])[0]
    return out


def missing(g, files):
    return [g['files'][k]['name'] for k, c in files.items() if c is None and not g['files'][k].get('optional')]


def stage(g, files):
    """Puts the files where tools/game.py looks for them: engine/data/roms/<dir>/<name>."""
    for key, c in files.items():
        f = g['files'][key]
        dst = os.path.join(game.ROMS, game.dirs(f)[0], f['name'])
        for d in game.dirs(f):                  # no stale copy in another of its folders
            other = os.path.join(game.ROMS, d, f['name'])
            if other != dst and os.path.lexists(other):
                os.remove(other)
        if c is None:
            if os.path.lexists(dst):
                os.remove(dst)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.lexists(dst):
            os.remove(dst)
        if c.member is None and dst.endswith('.chd'):
            try:                                # a CHD is large and only read once: link it
                os.symlink(os.path.abspath(c.path), dst)
                continue
            except OSError:
                pass
        if c.member is None:
            shutil.copyfile(c.path, dst)
        else:
            open(dst, 'wb').write(c.read())


# ---------------------------------------------------------------------------------------------
# the compiler

def zig():
    for p in (os.path.join(ENGINE, 'zig', 'zig' + EXE), os.environ.get('VIPER_ZIG'), shutil.which('zig')):
        if p and os.path.exists(p):
            return p
    raise SystemExit('the C compiler (engine/zig) is missing: download the release again')


def sdl2():
    """(include dir, flags to link SDL2)."""
    base = os.path.join(ENGINE, 'sdl2')
    inc = os.environ.get('VIPER_SDL2_INCLUDE') or os.path.join(base, 'include', 'SDL2')
    lib = os.path.join(base, 'lib')
    if SYSTEM == 'Darwin':                      # macOS has no SDL2 of its own: always the bundled one
        return inc, [os.environ.get('VIPER_SDL2_LIB') or os.path.join(lib, 'libSDL2-2.0.0.dylib'),
                     '-Wl,-rpath,@executable_path/' + os.path.relpath(lib, HOME)]
    flags = [os.environ.get('VIPER_SDL2_LIB') or os.path.join(lib, 'libSDL2-2.0.so.0')]
    if not system_has_sdl2():                   # the system's SDL2 first, the bundled one if none
        flags.append('-Wl,-rpath,$ORIGIN/' + os.path.relpath(lib, HOME))
    return inc, flags


def system_has_sdl2():
    try:
        import ctypes
        ctypes.CDLL('libSDL2-2.0.so.0')
        return True
    except OSError:
        return False


def run(cmd, env):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r


class Compiler:
    """The Makefile's compile and link steps, with zig cc. Keep the flags in step with it."""

    def __init__(self, jobs):
        self.zig, self.jobs = zig(), jobs
        self.env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(DATA, 'zig-cache'),
                        ZIG_LOCAL_CACHE_DIR=os.path.join(DATA, 'zig-cache'))
        # compiled here for this machine: the CPU's own instruction set (the Makefile uses AVX2
        # and FMA on x86-64; on ARM, as the Makefile, the default); -ffp-contract=off keeps the
        # floating point as the original
        self.arch = ['-march=native'] if platform.machine() in ('x86_64', 'AMD64') else []

    def cc(self, cxx=False):
        return [self.zig, 'c++' if cxx else 'cc']

    def chdextract(self):
        """tools/chdextract.c on libchdr: reads the CHDs (no chdman needed)."""
        out = os.path.join(DATA, 'build', 'chdextract' + EXE)
        if not os.path.exists(out):
            os.makedirs(os.path.dirname(out), exist_ok=True)
            ch = os.path.join(SRC, 'third_party', 'libchdr')
            run(self.cc() + ['-O2', '-w', '-DZ7_ST', '-I' + os.path.join(ch, 'include'),
                             '-I' + os.path.join(ch, 'deps', 'lzma-26.02', 'include'),
                             '-I' + os.path.join(ch, 'deps', 'miniz-3.1.2'),
                             '-I' + os.path.join(ch, 'deps', 'zstd-1.5.7'),
                             os.path.join(SRC, 'tools', 'chdextract.c'), os.path.join(ch, 'unity.c'),
                             os.path.join(ch, 'src', 'libchdr_codec_avhuff.c'), '-o', out], self.env)
        return out

    def build(self, g):
        gen, build = game.gen_dir(g), os.path.join(DATA, 'build', g['id'])
        rt = os.path.join(SRC, 'runtime')
        tp = os.path.join(SRC, 'third_party')
        sdl_inc, sdl_link = sdl2()
        tp_inc = ['-I' + os.path.join(tp, 'miniupnpc', 'include'), '-I' + os.path.join(tp, 'libnatpmp'),
                  '-DMINIUPNP_STATICLIB', '-DNATPMP_STATICLIB']
        cflags = ['-O2', *self.arch, '-g0', '-std=c11', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE', '-I' + rt,
                  '-I' + gen, '-ffp-contract=off', '-fno-strict-aliasing', '-w']
        cxxflags = ['-O2', *self.arch, '-g0', '-std=c++20', '-I' + os.path.join(rt, 'voodoo'), '-w']
        units = []      # (source, object, command)

        def unit(src, flags, cxx=False):
            obj = os.path.join(build, os.path.relpath(src, os.path.dirname(src) if src.startswith(gen) else SRC))
            obj = os.path.splitext(obj)[0] + '.o'
            units.append((src, obj, self.cc(cxx) + flags + ['-c', src, '-o', obj]))

        for f in ('cpu', 'sched', 'hw', 'main', 'frontend_sdl', 'enhanced', 'net', 'portmap'):
            unit(os.path.join(rt, f + '.c'), cflags + ['-I' + sdl_inc] + tp_inc)
        for f in ('voodoo', 'voodoo_2', 'voodoo_banshee', 'voodoo_render', 'voodoo_bridge', 'voodoo_gpu'):
            unit(os.path.join(rt, 'voodoo', f + '.cpp'), cxxflags, True)
        unit(os.path.join(rt, 'gpu_gl.cpp'), cxxflags + ['-I' + sdl_inc], True)
        tps = [os.path.join(tp, 'miniupnpc', 'src', f) for f in sorted(os.listdir(os.path.join(tp, 'miniupnpc', 'src')))
               if f.endswith('.c')]
        tps += [os.path.join(tp, 'libnatpmp', 'natpmp.c'), os.path.join(tp, 'libnatpmp', 'getgateway.c')]
        for f in tps:
            unit(f, ['-O2', *self.arch, '-w'] + tp_inc + ['-I' + os.path.join(tp, 'miniupnpc', 'src'),
                     '-DMINIUPNPC_SET_SOCKET_TIMEOUT', '-DMINIUPNPC_GET_SRC_ADDR', '-D_BSD_SOURCE',
                     '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE'])
        gen_srcs = open(os.path.join(gen, 'sources.mk')).read().split('=', 1)[1].split()
        for f in gen_srcs:
            unit(os.path.join(gen, os.path.basename(f)), cflags + ['-frounding-math'])

        if os.path.isdir(build):
            shutil.rmtree(build)
        for _, obj, _ in units:
            os.makedirs(os.path.dirname(obj), exist_ok=True)
        t0, done = time.time(), 0
        # the biggest first: the generated files take most of the time
        units.sort(key=lambda u: -os.path.getsize(u[0]))
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as ex:
            futs = [ex.submit(run, cmd, self.env) for _, _, cmd in units]
            for fu in concurrent.futures.as_completed(futs):
                fu.result()
                done += 1
                if sys.stdout.isatty():
                    print(f"\r  compiling {done}/{len(units)}", end='', flush=True)
        log(f"\r  compiled {len(units)} files in {time.time() - t0:.0f} s" + ' ' * 10)

        exe = os.path.join(HOME, g['binary'] + EXE)
        run(self.cc(True) + ['-s', '-o', exe] + [o for _, o, _ in units] + sdl_link + ['-lpthread', '-lm'], self.env)
        return exe


# ---------------------------------------------------------------------------------------------

def cpu_id():
    """The CPU the games are compiled for (-march=native): its model and instruction set extensions.
    A folder moved to another computer (a microSD card between a PC and a Steam Deck) rebuilds."""
    if SYSTEM == 'Darwin':
        r = subprocess.run(['sysctl', '-n', 'machdep.cpu.brand_string'], capture_output=True, text=True)
        return r.stdout.strip() or platform.machine()
    try:
        info = open('/proc/cpuinfo').read()
    except OSError:
        return platform.machine()
    fields = dict(l.split(':', 1) for l in info.split('\n\n')[0].splitlines() if ':' in l)
    model, flags = fields.get('model name\t', '').strip(), fields.get('flags\t\t', '').split()
    return f"{model} / {hashlib.sha1(' '.join(sorted(flags)).encode()).hexdigest()[:12]}"


def stamp_of(g, files):
    return {'version': version(), 'cpu': cpu_id(),
            'files': {k: (c.sha1(k == 'chd') if c else None) for k, c in files.items()}}


def quietly(fn, *args):
    """Runs a step of the pipeline, showing its output only if it fails."""
    out, stdout = io.StringIO(), sys.stdout
    sys.stdout = out
    try:
        fn(*args)
    except BaseException:
        sys.stdout = stdout
        log(out.getvalue())
        raise
    finally:
        sys.stdout = stdout


def build_game(g, files, comp, rebuild):
    exe = os.path.join(HOME, g['binary'] + EXE)
    stamp_file = os.path.join(DATA, 'build', g['id'] + '.stamp.json')
    stamp = stamp_of(g, files)
    if not rebuild and os.path.exists(exe) and os.path.exists(stamp_file) and json.load(open(stamp_file)) == stamp:
        log(f"{g['title']} ({g['version']}): up to date, ./{g['binary'] + EXE}")
        return exe
    log(f"\n=== {g['title']} ({g['version']}) ===")
    t0 = time.time()
    stage(g, files)
    if os.path.exists(stamp_file):
        os.remove(stamp_file)
    for d in (game.work_dir(g), game.gen_dir(g)):      # nothing left from another version
        if os.path.isdir(d):
            shutil.rmtree(d)
    log('- extracting the game')
    quietly(extract.main, g['id'])
    log('- recompiling the PowerPC code to C')
    quietly(recomp.main, g['id'])
    log('- compiling')
    exe = comp.build(g)
    json.dump(stamp, open(stamp_file, 'w'))
    log(f"done in {time.time() - t0:.0f} s: ./{os.path.basename(exe)}")
    return exe


def main():
    ap = argparse.ArgumentParser(prog='viper-setup', description=__doc__.split('\n')[0])
    ap.add_argument('games', nargs='*', help='MAME set names (default: every game found)')
    ap.add_argument('--roms', action='append', default=[], metavar='DIR', help='another folder to search')
    ap.add_argument('--list', action='store_true', help='show the games found and the missing files')
    ap.add_argument('--rebuild', action='store_true', help='rebuild also the games that are up to date')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    ids = a.games or game.ids()
    for i in ids:
        if i not in game.ids():
            raise SystemExit(f"unknown game '{i}' (known: {', '.join(game.ids())})")
    games = [game.load(i) for i in ids]
    dirs = [os.path.join(HOME, 'roms')] + a.roms
    os.makedirs(dirs[0], exist_ok=True)
    os.makedirs(DATA, exist_ok=True)

    comp = None
    if not a.list or any(n.endswith('.chd') for g in games for n in (f['name'] for f in g['files'].values())):
        comp = Compiler(a.jobs)
        os.environ['VIPER_CHDEXTRACT'] = comp.chdextract()

    wanted = {f['name'].lower() for g in games for f in g['files'].values()}
    log('Looking for the game files in: ' + ', '.join(dirs))
    found = scan(dirs, wanted)
    ready = []
    for g in games:
        files = resolve(g, found)
        miss = missing(g, files)
        label = f"{g['mame_set']:11s} {g['title']} ({g['version']})"
        if miss:
            if a.list or a.games:
                log(f"  {label}: missing {', '.join(miss)}")
            continue
        bad = [g['files'][k]['name'] for k, c in files.items()
               if c and g['files'][k].get('sha1') and c.sha1(k == 'chd') != g['files'][k]['sha1']]
        log(f"  {label}: found" + (f" (unexpected SHA1: {', '.join(bad)})" if bad else ''))
        ready.append((g, files))
    if a.list:
        return
    if not ready:
        log('\nNo game has all its files. Put them in roms/ (README.txt, roms/FILES.txt), then run this again;\n'
            "'viper-setup --list' shows what each game needs.")
        sys.exit(1)
    built = []
    for g, files in ready:
        try:
            built.append(build_game(g, files, comp, a.rebuild))
        except (RuntimeError, SystemExit) as e:
            log(f"\n{g['title']}: the build failed.\n{e}")
            sys.exit(1)
    log('\nReady. To play: ' + ', '.join('./' + os.path.basename(b) for b in built) +
        ' (enhanced mode; --classic for the cabinet)')


if __name__ == '__main__':
    main()
