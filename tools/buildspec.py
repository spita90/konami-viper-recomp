#!/usr/bin/env python3
"""What a game's executable and tools/chdextract are built from, and with which flags: the one
description used by the Makefile and by the release launcher (dist/launcher.py, which compiles
with zig on the player's computer).

  python3 tools/buildspec.py make     the Makefile's variables (the Makefile writes them to
                                      build/make-vars.mk; $(GEN) and $(OPT) are left to make)

The two builds differ only where they mean to: `dev` (make) keeps the debug information and the
warnings, the package does not. SDL2 is the caller's: its flags are added to the "runtime" and
"gl" sources (sdl2-config for make, the copy in the package for the launcher).
"""
import os
import platform
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RUNTIME_C = ('cpu', 'sched', 'hw', 'main', 'frontend_sdl', 'enhanced', 'net', 'portmap')
VOODOO_CPP = ('voodoo', 'voodoo_2', 'voodoo_banshee', 'voodoo_render', 'voodoo_bridge', 'voodoo_gpu')
LIBCHDR_DEPS = ('lzma-26.02/include', 'miniz-3.1.2', 'zstd-1.5.7')


def arch_flags(machine=None):
    """Built where it runs: the CPU's own instruction set on x86-64, the default on ARM. Without
    AVX-512: with it gcc changes a few pixels of the software renderer (1-8 per frame in Thrill
    Drive 2's attract, against clang and gcc within AVX2), and the builds are compared bit for bit."""
    return ['-march=native', '-mno-avx512f'] if (machine or platform.machine()) in ('x86_64', 'AMD64') else []


def sources(root, system):
    """{kind: [paths]}: "runtime" (C, with SDL2), "voodoo" (C++), "gl" (C++, with SDL2: the GPU
    renderer, GL loaded through SDL at run time), "third_party" (C: UPnP and NAT-PMP for the
    link-play host). The generated sources come from generated/<id>/sources.mk."""
    j = lambda *p: os.path.join(root, *p)
    upnp = sorted(f for f in os.listdir(j('third_party', 'miniupnpc', 'src')) if f.endswith('.c'))
    natpmp = ['natpmp.c', 'getgateway.c'] + (['wingettimeofday.c'] if system == 'Windows' else [])
    return {
        'runtime': [j('runtime', f + '.c') for f in RUNTIME_C],
        'voodoo': [j('runtime', 'voodoo', f + '.cpp') for f in VOODOO_CPP],
        'gl': [j('runtime', 'gpu_gl.cpp')],
        'third_party': [j('third_party', 'miniupnpc', 'src', f) for f in upnp] +
                       [j('third_party', 'libnatpmp', f) for f in natpmp],
    }


def flags(kind, root, gen, system, dev=False, opt='-O2', arch=None):
    """Compiler flags for a kind of source ("runtime", "generated", "voodoo", "gl", "third_party",
    "chdextract"); `gen` is the game's generated/<id>/ folder."""
    j = lambda *p: os.path.join(root, *p)
    arch = arch_flags() if arch is None else arch
    debug = ['-g'] if dev else ['-g0']
    tp_inc = ['-I' + j('third_party', 'miniupnpc', 'include'), '-I' + j('third_party', 'libnatpmp'),
              '-DMINIUPNP_STATICLIB', '-DNATPMP_STATICLIB']
    # -ffp-contract=off keeps the floating point as the original's (no fused operations)
    c = [opt, *arch, *debug, '-std=c11', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE', '-I' + j('runtime'),
         '-I' + gen, '-ffp-contract=off', '-fno-strict-aliasing']
    c += ['-Wall', '-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-function'] if dev else ['-w']
    cxx = [opt, *arch, *debug, '-std=c++20', '-I' + j('runtime', 'voodoo')]
    cxx += ['-Wno-unused-private-field', '-Wno-deprecated-declarations'] if dev else ['-w']
    if kind == 'runtime':      # on Windows SDL2main provides main() and calls the program's SDL_main()
        return c + tp_inc + (['-Dmain=SDL_main'] if system == 'Windows' else [])
    if kind == 'generated':    # the guest's rounding mode is set at run time (rt_mtfsf -> fesetround)
        return c + (['-Wno-unused-but-set-variable', '-Wno-parentheses-equality'] if dev else []) + \
            ['-frounding-math']
    if kind in ('voodoo', 'gl'):
        return cxx
    if kind == 'third_party':
        return ['-O2', *arch, '-w'] + tp_inc + ['-I' + j('third_party', 'miniupnpc', 'src'),
                                               '-DMINIUPNPC_SET_SOCKET_TIMEOUT', '-DMINIUPNPC_GET_SRC_ADDR',
                                               '-D_BSD_SOURCE', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE']
    if kind == 'chdextract':
        ch = j('third_party', 'libchdr')
        return ['-O2', '-w', '-DZ7_ST', '-I' + os.path.join(ch, 'include')] + \
            ['-I' + os.path.join(ch, 'deps', d) for d in LIBCHDR_DEPS]
    raise ValueError(kind)


def chdextract_sources(root):
    """tools/chdextract.c and libchdr (a unity build, plus the codec it leaves out)."""
    ch = os.path.join(root, 'third_party', 'libchdr')
    return [os.path.join(root, 'tools', 'chdextract.c'), os.path.join(ch, 'unity.c'),
            os.path.join(ch, 'src', 'libchdr_codec_avhuff.c')]


def libs(system):
    """System libraries of the executable (SDL2's are the caller's)."""
    if system == 'Windows':    # Winsock and IP Helper for link play; SDL2main needs shell32
        return ['-lpthread', '-lws2_32', '-liphlpapi', '-lmswsock', '-lshell32', '-luser32']
    return ['-lpthread', '-lm']


def make_vars():
    """The Makefile's variables, paths relative to the repository."""
    system = platform.system()
    rel = lambda paths: ' '.join(os.path.relpath(p, ROOT) for p in paths)
    src = sources(ROOT, system)
    fl = lambda kind: ' '.join(flags(kind, '', '$(GEN)', system, dev=True, opt='$(OPT)')
                               if kind != 'chdextract' else flags(kind, '', '', system))
    return '\n'.join([
        '# generated by tools/buildspec.py (make rebuilds it when that file changes)',
        f"RT_SRCS := {rel(src['runtime'])}",
        f"VD_SRCS := {rel(src['voodoo'])}",
        f"GL_SRCS := {rel(src['gl'])}",
        f"TP_SRCS := {rel(src['third_party'])}",
        f"CHD_SRCS := {rel(chdextract_sources(ROOT))}",
        f"RT_FLAGS = {fl('runtime')}",
        f"GEN_FLAGS = {fl('generated')}",
        f"CXX_FLAGS = {fl('voodoo')}",
        f"TP_FLAGS = {fl('third_party')}",
        f"CHD_FLAGS = {fl('chdextract')}",
        f"LIBS := {' '.join(libs(system))}",
    ]) + '\n'


if __name__ == '__main__':
    if sys.argv[1:] != ['make']:
        raise SystemExit(__doc__)
    sys.stdout.write(make_vars())
