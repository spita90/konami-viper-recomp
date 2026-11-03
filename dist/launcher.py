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
  viper-setup          this program (frozen with PyInstaller)
  roms/                the player's files
  engine/              this program's Python, and its tools/, recomp/ and games/
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
import buildspec  # noqa: E402
import game       # noqa: E402
import extract    # noqa: E402
import recomp     # noqa: E402

SYSTEM = platform.system()          # Linux, Darwin (macOS), Windows
if SYSTEM not in ('Linux', 'Darwin', 'Windows'):
    raise SystemExit(f'viper-setup: {SYSTEM} is not supported')
EXE = '.exe' if SYSTEM == 'Windows' else ''


def play_name(exe):
    """How to start an executable from the terminal: ./td2, or td2.exe on Windows."""
    return os.path.basename(exe) if SYSTEM == 'Windows' else './' + os.path.basename(exe)


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
    """(include dir, flags to link SDL2, the DLL to put next to the executables or None)."""
    base = os.path.join(ENGINE, 'sdl2')
    inc = os.environ.get('VIPER_SDL2_INCLUDE') or os.path.join(base, 'include', 'SDL2')
    lib = os.path.join(base, 'lib')
    if SYSTEM == 'Windows':                     # SDL2's MinGW package: SDL2main gives main()
        return inc, [os.path.join(lib, 'libSDL2main.a'), os.path.join(lib, 'libSDL2.dll.a')], \
            os.path.join(lib, 'SDL2.dll')
    if SYSTEM == 'Darwin':                      # macOS has no SDL2 of its own: always the bundled one
        return inc, [os.environ.get('VIPER_SDL2_LIB') or os.path.join(lib, 'libSDL2-2.0.0.dylib'),
                     '-Wl,-rpath,@executable_path/' + os.path.relpath(lib, HOME)], None
    flags = [os.environ.get('VIPER_SDL2_LIB') or os.path.join(lib, 'libSDL2-2.0.so.0')]
    if not system_has_sdl2():                   # the system's SDL2 first, the bundled one if none
        flags.append('-Wl,-rpath,$ORIGIN/' + os.path.relpath(lib, HOME))
    return inc, flags, None


def system_has_sdl2(name='libSDL2-2.0.so.0'):
    """Whether the system itself has SDL2, from the dynamic linker's cache and the usual folders.
    Not with ctypes: PyInstaller would bundle the build machine's SDL2 next to this program, and
    the program's own folder is on its library path, so the answer would always be yes."""
    for ldconfig in ('/sbin/ldconfig', '/usr/sbin/ldconfig', 'ldconfig'):
        try:
            out = subprocess.run([ldconfig, '-p'], capture_output=True, text=True).stdout
        except OSError:
            continue
        if name in out:
            return True
        break
    dirs = ('/usr/lib64', '/usr/lib', '/lib64', '/lib', '/usr/lib/x86_64-linux-gnu', '/lib/x86_64-linux-gnu',
            '/usr/local/lib', '/usr/local/lib64')
    return any(os.path.exists(os.path.join(d, name)) for d in dirs)


def run(cmd, env):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r


class Compiler:
    """The Makefile's compile and link steps, with zig cc: the sources and the flags are
    tools/buildspec.py's, as for make (without its debug information and warnings)."""

    def __init__(self, jobs):
        self.zig, self.jobs = zig(), jobs
        self.env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(DATA, 'zig-cache'),
                        ZIG_LOCAL_CACHE_DIR=os.path.join(DATA, 'zig-cache'))
        self.arch = None        # buildspec's: compiled here for this machine's CPU

    def cc(self, cxx=False):
        return [self.zig, 'c++' if cxx else 'cc']

    def flags(self, kind, gen=''):
        return buildspec.flags(kind, SRC, gen, SYSTEM, arch=self.arch)

    def chdextract(self):
        """tools/chdextract.c on libchdr: reads the CHDs."""
        out = os.path.join(DATA, 'build', 'chdextract' + EXE)
        if not os.path.exists(out):
            os.makedirs(os.path.dirname(out), exist_ok=True)
            run(self.cc() + self.flags('chdextract') + buildspec.chdextract_sources(SRC) + ['-o', out], self.env)
        return out

    def build(self, g):
        gen, build = game.gen_dir(g), os.path.join(DATA, 'build', g['id'])
        sdl_inc, sdl_link, sdl_dll = sdl2()
        units = []      # (source, object, command)

        def unit(src, flags, cxx=False):
            obj = os.path.join(build, os.path.relpath(src, os.path.dirname(src) if src.startswith(gen) else SRC))
            obj = os.path.splitext(obj)[0] + '.o'
            units.append((src, obj, self.cc(cxx) + flags + ['-c', src, '-o', obj]))

        srcs = buildspec.sources(SRC, SYSTEM)
        for f in srcs['runtime']:
            unit(f, self.flags('runtime', gen) + ['-I' + sdl_inc])
        for f in srcs['voodoo']:
            unit(f, self.flags('voodoo', gen), True)
        for f in srcs['gl']:
            unit(f, self.flags('gl', gen) + ['-I' + sdl_inc], True)
        for f in srcs['third_party']:
            unit(f, self.flags('third_party', gen))
        gen_srcs = open(os.path.join(gen, 'sources.mk')).read().split('=', 1)[1].split()
        for f in gen_srcs:
            unit(os.path.join(gen, os.path.basename(f)), self.flags('generated', gen))

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
        log('  linking (the first time zig also builds its C++ library)')

        exe = os.path.join(HOME, g['binary'] + EXE)
        libs = buildspec.libs(SYSTEM)
        if SYSTEM == 'Windows':     # main.c exports the hints for the dedicated GPU: their import library, out of sight
            libs = libs + ['-Wl,--out-implib,' + os.path.join(build, g['binary'] + '.lib')]
        run(self.cc(True) + ['-s', '-o', exe] + [o for _, o, _ in units] + sdl_link + libs, self.env)
        if sdl_dll:
            shutil.copyfile(sdl_dll, os.path.join(HOME, os.path.basename(sdl_dll)))
        return exe


# ---------------------------------------------------------------------------------------------

def cpu_id():
    """The CPU the games are compiled for (-march=native): its model and instruction set extensions.
    A folder moved to another computer (a microSD card between a PC and a Steam Deck) rebuilds."""
    if SYSTEM == 'Darwin':
        r = subprocess.run(['sysctl', '-n', 'machdep.cpu.brand_string'], capture_output=True, text=True)
        return r.stdout.strip() or platform.machine()
    if SYSTEM == 'Windows':                     # family, model and stepping
        return os.environ.get('PROCESSOR_IDENTIFIER') or platform.processor() or platform.machine()
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
        log(f"{g['title']} ({g['version']}): up to date, {play_name(exe)}")
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
    log(f"done in {time.time() - t0:.0f} s: {play_name(exe)}")
    return exe


def main():
    ap = argparse.ArgumentParser(prog='viper-setup', description=__doc__.split('\n')[0])
    ap.add_argument('games', nargs='*', help='MAME set names (default: every game found)')
    ap.add_argument('--roms', action='append', default=[], metavar='DIR', help='another folder to search')
    ap.add_argument('--list', action='store_true', help='show the games found and the missing files')
    ap.add_argument('--rebuild', action='store_true', help='rebuild also the games that are up to date')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    log(f"Konami Viper setup, version {version()}")

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
        if not os.path.exists(os.path.join(DATA, 'build', 'chdextract' + EXE)):
            log('Preparing the tools (the first time only, a few seconds)...')
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
    log('\nReady. To play: ' + ', '.join(play_name(b) for b in built) +
        ' (enhanced mode; --classic for the cabinet)')


def started_from_explorer():
    """Windows: true when the program has a console of its own (a double-click in Explorer),
    which closes as soon as it ends; from a terminal the console also holds the shell."""
    if SYSTEM != 'Windows':
        return False
    try:
        import ctypes
        ids = (ctypes.c_uint * 8)()
        return ctypes.windll.kernel32.GetConsoleProcessList(ids, 8) == 1
    except Exception:
        return False


def console_quick_edit(on):
    """Windows: turns the console's QuickEdit mode off (on=False) and back on. With it, a click
    in the window starts a selection ("Select" in the title) and Windows suspends the program at
    its next output until a key is pressed, so a build looks stuck. Returns the previous mode."""
    if SYSTEM != 'Windows':
        return None
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-10)             # STD_INPUT_HANDLE
        mode = ctypes.c_uint32()
        if not k.GetConsoleMode(h, ctypes.byref(mode)):
            return None
        quick_edit, extended = 0x0040, 0x0080
        k.SetConsoleMode(h, (mode.value | extended) | quick_edit if on else (mode.value | extended) & ~quick_edit)
        return bool(mode.value & quick_edit)
    except Exception:
        return None


if __name__ == '__main__':
    was_quick_edit = console_quick_edit(False)
    try:
        main()
    finally:
        if started_from_explorer():
            input('\nPress Enter to close this window.')
        if was_quick_edit:
            console_quick_edit(True)
