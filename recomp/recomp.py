#!/usr/bin/env python3
"""Konami Viper static recompiler: PPC modules -> C.

usage: recomp.py <game id>        work/<id>/ -> generated/<id>/

work/<id>/ is produced by tools/extract.py (kernel.bin + fs/...). The modules to recompile and
the hints for hand-written code come from games/<id>/game.json:
  "hints": {"<module>": {"entries": ["0xADDR", ...],
                          "local_indirect": {"0xADDR": {"slots": [lo, hi, step], "targets": [...]}}}}
and the idle loops to skip (runtime/cpu.c, rt_idle) from:
  "idle": {"<module>": {"0xEND": {"work": ["0xADDR", ...], "loop": ["0xLO", "0xHI"], "crc32": "0x..."}}}
0xEND is reached once per pass of the loop, the work addresses only when a pass has something to do;
the hint applies only if the code in [LO, HI) has that CRC-32 (a version with other code ignores it).
"""
import json
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from analyze import Module, discover          # noqa: E402
from emit import Emitter                      # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
import game                                   # noqa: E402

FUNCS_PER_FILE = 300


def be32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def load_modules(work, modules=None):
    mods = []

    # resident kernel (decompressed by the BIOS from the boot block to RAM 0)
    k = open(os.path.join(work, 'kernel.bin'), 'rb').read()
    ktext_end = be32(k, 0x18)
    mods.append(Module('kernel', 'k', k, 0, toc=be32(k, 0x2c), text=(0x10, ktext_end),
                       entries=[0x10, 0x500, 0x900, 0xc00]))

    # BIOS 941B01: only the uncompressed exception vectors are ever reached from the kernel
    b = open(os.path.join(work, 'bios.bin'), 'rb').read()
    mods.append(Module('bios', 'b', b, 0xfff00000, toc=None, text=(0xfff00100, 0xfff01800),
                       entries=[0xfff00000 + v for v in range(0x200, 0x1500, 0x100)]))

    # load addresses come from the game file's directory (see tools/viper_fs.py)
    manifest = {e['name']: e for e in json.load(open(os.path.join(work, 'fs', 'manifest.json')))}

    def prog(name, prefix, path, layout, base=None):
        d = open(os.path.join(work, 'fs', path), 'rb').read()
        if base is None:
            base = int(manifest[path]['load'], 16)
        entry, toc = be32(d, 0), be32(d, 4)
        if layout == 'app':        # {desc(3), text_start, text_end, data_start, text_len, data_len, bss, toc}
            text = (base + 0x28, be32(d, 0x10))
        else:                      # library: {desc(3)} then code
            text = (base + 0x0c, base + len(d))
        ents = [entry] if layout == 'app' else [entry, base + 0x0c]   # libraries: glue entry right after header
        mods.append(Module(name, prefix, d, base, toc=toc, text=text, entries=ents))

    for m in modules or []:
        prog(m['name'], m['prefix'], m['path'], m['layout'],
             int(m['base'], 16) if 'base' in m else None)
    return mods


def apply_hints(m, h):
    """Apply a module's profile hints; returns the extra entry points for discover()."""
    extra = [int(x, 16) for x in h.get('entries', [])]
    for ent, spec in h.get('local_indirect', {}).items():
        tg = {int(x, 16) for x in spec.get('targets', [])}
        if 'slots' in spec:
            lo, hi, step = int(spec['slots'][0], 16), int(spec['slots'][1], 16), spec['slots'][2]
            tg |= {a for a in range(lo, hi, step) if m.word(a) != 0 and m.insn(a) is not None}
        m.local_indirect[int(ent, 16)] = tg
        extra.append(int(ent, 16))
    return extra


def idle_sites(m, spec):
    """{end address: [work addresses]} of the module's idle hints whose code matches."""
    out = {}
    for end, h in spec.items():
        lo, hi = (int(x, 16) for x in h['loop'])
        crc = zlib.crc32(b''.join(struct.pack('>I', m.word(a)) for a in range(lo, hi, 4)))
        if crc != int(h['crc32'], 16):
            print(f"{m.name}: idle loop {end} not applied (its code differs: CRC-32 {crc:#010x})")
            continue
        out[int(end, 16)] = [int(w, 16) for w in h['work']]
    return out


def main(gid):
    g = game.load(gid)
    work, out = game.work_dir(g), game.gen_dir(g)
    if not os.path.exists(os.path.join(work, 'kernel.bin')):
        raise SystemExit(f"{os.path.relpath(work, game.ROOT)}/ not found: run 'make extract GAME={gid}' first")
    os.makedirs(out, exist_ok=True)
    hints = g.get('hints', {})
    hooks = ((g.get('enhanced') or {}).get('hooks') or {})   # {module: {"0xADDR": name}}
    mods = load_modules(work, g['modules'])
    all_funcs = {}
    for m in mods:
        t0 = time.time()
        m.hooks = {int(a, 16) for a in hooks.get(m.name, {})}
        m.idle = idle_sites(m, g.get('idle', {}).get(m.name, {}))
        funcs, entries = discover(m, apply_hints(m, hints.get(m.name, {})))
        all_funcs[m.name] = (m, funcs, entries)
        ninsn = sum(len(f.insns) for f in funcs.values())
        nbad = sum(1 for f in funcs.values() if f.bad)
        njt = sum(len(f.jumptables) for f in funcs.values())
        print(f"{m.name:9s} base={m.base:#08x} text={m.text[0]:#x}-{m.text[1]:#x} toc={(m.toc or 0):#x} "
              f"funcs={len(funcs)} insns={ninsn} bad={nbad} jumptables={njt} ({time.time()-t0:.1f}s)")

    for m, funcs, _ in all_funcs.values():         # an idle loop and its work addresses: one function
        for end, work in m.idle.items():
            fns = [f for f in funcs.values() if all(a in f.insns for a in [end] + work)]
            if not fns:
                raise SystemExit(f"{m.name}: idle loop {end:#x} and its work addresses are not in one function")

    def resolve_call(mod, target):
        _, funcs, entries = all_funcs[mod.name]
        if target in funcs:
            return funcs[target].name
        return None

    manifest = {}
    for name, (m, funcs, entries) in all_funcs.items():
        hdr = os.path.join(out, f"mod_{name}.h")
        with open(hdr, 'w') as f:
            f.write(f"/* generated: module {name} */\n#pragma once\n#include \"ppc_rt.h\"\n")
            for fn in funcs.values():
                f.write(f"void {fn.name}(PPCContext *c);\n")
        files = []
        items = list(funcs.values())
        for n in range(0, len(items), FUNCS_PER_FILE):
            fname = f"{name}_{n // FUNCS_PER_FILE:03d}.c"
            files.append(fname)
            with open(os.path.join(out, fname), 'w') as f:
                f.write(f"/* generated by recomp.py: module {name} */\n#include \"mod_{name}.h\"\n\n")
                for fn in items[n:n + FUNCS_PER_FILE]:
                    f.write(Emitter(fn, entries, resolve_call, None).emit())
                    f.write("\n\n")
        tname = f"{name}_table.c"
        files.append(tname)
        with open(os.path.join(out, tname), 'w') as f:
            f.write(f"#include \"mod_{name}.h\"\n\nconst RtFunc rt_funcs_{name}[] = {{\n")
            for fn in items:
                f.write(f"  {{0x{fn.entry:08x}u, {fn.name}, 0x{m.word(fn.entry):08x}u}},\n")
            f.write("};\n")
            f.write(f"const unsigned rt_nfuncs_{name} = {len(items)};\n")
            sig = ", ".join(f"0x{m.word(m.base + 4 * k):08x}u" for k in range(8))
            f.write(f"const RtModuleInfo rt_module_{name} = {{\"{name}\", 0x{m.base:08x}u, 0x{m.end:08x}u, "
                    f"0x{m.text[0]:08x}u, 0x{m.text[1]:08x}u, rt_funcs_{name}, {len(items)}, {{{sig}}}}};\n")
        manifest[name] = files
    with open(os.path.join(out, 'sources.mk'), 'w') as f:
        rel = os.path.relpath(out, game.ROOT)
        f.write("GEN_SRCS = " + " ".join(os.path.join(rel, x) for fs in manifest.values() for x in fs) + "\n")
    with open(os.path.join(out, 'modules.h'), 'w') as f:
        f.write("#pragma once\n#include \"ppc_rt.h\"\n")
        for name in manifest:
            f.write(f"extern const RtModuleInfo rt_module_{name};\n")
        f.write("#define RT_ALL_MODULES " + ", ".join(f"&rt_module_{n}" for n in manifest) + "\n")
    game.write_config_header(g, out)
    print("done")


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
