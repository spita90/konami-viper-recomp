"""Function discovery for Viper PPC modules.

Strategy (safe over-approximation):
  * entry points = module entry descriptor, exception vectors, PowerOpen function
    descriptors {code, TOC}, raw code pointers to prologues, the word after each
    function trailer (0x8000xxxx), prologues right after an unconditional branch,
    every `bl` target and hint-file entries;
  * a function body = everything reachable from its entry through b/bc (bl does not
    split flow), stopping at blr/bctr/rfi; switch tables (bctr or mtlr+blr) are resolved
    by path-sensitive symbolic slicing on the partial CFG;
  * a branch that lands on *another* entry is emitted as a tail call, anything else is
    a local label (code may be duplicated between functions, always correct).
"""
import struct
from ppc import decode

VOLATILE = {0} | set(range(3, 13))


class Module:
    def __init__(self, name, prefix, data, base, toc=None, text=None, entries=(), bss_end=None):
        self.name = name
        self.prefix = prefix
        self.data = data
        self.base = base
        self.end = base + len(data)
        self.toc = toc
        self.text = text or (base, self.end)
        self.initial = set(entries)
        self.bss_end = bss_end
        self.local_indirect = {}   # entry -> set of targets reachable by bctr/blr inside that function
        self.hooks = set()         # addresses where the generated code calls rt_hook() first (mods)
        self.idle = {}             # idle loops: {end of a pass: [work addresses]} (rt_idle)
        self._cache = {}

    def contains(self, a):
        return self.text[0] <= a < self.text[1]

    def word(self, a):
        return struct.unpack_from('>I', self.data, a - self.base)[0]

    def insn(self, a):
        if a in self._cache:
            return self._cache[a]
        i = decode(a, self.word(a)) if self.contains(a) and not a & 3 else None
        self._cache[a] = i
        return i


class Function:
    def __init__(self, mod, entry):
        self.mod = mod
        self.entry = entry
        self.insns = {}          # addr -> Insn (None for undecodable)
        self.labels = set()      # local branch targets
        self.calls = set()       # bl targets
        self.jumptables = {}     # bcctr/bclr addr -> list of targets
        self.indirect = set()    # unresolved unconditional bcctr / mtlr+blr sites
        self.bad = False

    @property
    def name(self):
        return f"f_{self.mod.prefix}_{self.entry:08x}"

    # successors inside the body (for the CFG used by the slicer)
    def succs(self, a):
        i = self.insns.get(a)
        if i is None:
            return []
        n = i.name
        out = []
        if n == 'b' and not i.lk:
            if i.target in self.insns:
                out.append(i.target)
            return out
        if n == 'bc' and not i.lk:
            if i.target in self.insns:
                out.append(i.target)
            if i.unconditional:
                return out
        if n in ('bclr', 'bcctr') and not i.lk and i.unconditional:
            return [t for t in self.jumptables.get(a, ()) if t in self.insns]
        if n == 'rfi':
            return out
        if a + 4 in self.insns:
            out.append(a + 4)
        return out


def is_call(i):
    return i.lk and i.name in ('b', 'bc', 'bclr', 'bcctr')


def writes_gpr(i, reg):
    """True if instruction i (may) define GPR reg.  Calls clobber volatile registers."""
    n = i.name
    if is_call(i):
        return reg in VOLATILE
    if n in ('ori', 'oris', 'xori', 'xoris', 'andi.', 'andis.', 'rlwinm', 'rlwimi', 'rlwnm',
             'and', 'andc', 'or', 'orc', 'xor', 'nand', 'nor', 'eqv', 'slw', 'srw', 'sraw',
             'srawi', 'extsb', 'extsh', 'cntlzw'):
        return i.ra == reg
    if n in ('lmw',):
        return reg >= i.rd
    if n in ('lswi', 'lswx'):
        return True
    if n.startswith(('lf', 'stf')):
        return n.endswith(('u', 'ux')) and i.ra == reg
    if n.startswith('st'):
        return n.endswith(('u', 'ux')) and i.ra == reg
    if n.startswith(('cmp', 'b', 'mt', 'dc', 'ic', 'tw', 'sync', 'isync', 'eieio', 'cr', 'mcrf',
                     'sc', 'rfi', 'tlb', 'f', 'mcrfs')):
        return False
    if n.endswith(('u', 'ux')) and i.ra == reg:
        return True
    return i.rd == reg


class Sym:
    """Symbolic value for jump-table slicing: kind in const|idx|load|loadplus."""
    __slots__ = ('kind', 'k', 'plus')

    def __init__(self, kind, k=0, plus=0):
        self.kind, self.k, self.plus = kind, k & 0xffffffff, plus & 0xffffffff

    def key(self):
        return (self.kind, self.k, self.plus)


class Slicer:
    def __init__(self, fn):
        self.fn = fn
        self.mod = fn.mod
        self.preds = {}
        for a in fn.insns:
            for s in fn.succs(a):
                self.preds.setdefault(s, []).append(a)

    def reaching_defs(self, reg, addr):
        """Definitions of reg reaching instruction `addr` (None = function entry / unknown)."""
        res = set()
        seen = set()
        stack = list(self.preds.get(addr, []))
        if not stack:
            res.add(None)
        while stack and len(res) < 8:
            a = stack.pop()
            if a in seen:
                continue
            seen.add(a)
            i = self.fn.insns.get(a)
            if i is None:
                res.add(None)
                continue
            if writes_gpr(i, reg):
                res.add(a)
                continue
            p = self.preds.get(a, [])
            if not p:
                res.add(None)
            stack.extend(p)
        return res

    def value(self, reg, addr, depth=0):
        if reg == 2 and self.mod.toc is not None:
            return Sym('const', self.mod.toc)
        if depth > 16:
            return None
        defs = self.reaching_defs(reg, addr)
        if None in defs or not defs:
            return None
        vals = {}
        for d in defs:
            v = self.eval_def(self.fn.insns[d], reg, depth)
            if v is None:
                return None
            vals[v.key()] = v
        return next(iter(vals.values())) if len(vals) == 1 else None

    def eval_def(self, i, reg, depth):
        n = i.name
        mod = self.mod
        if is_call(i):
            return None

        def ev(r):
            return self.value(r, i.addr, depth + 1)

        if n in ('addi', 'addis'):
            imm = i.imm if n == 'addi' else (i.uimm << 16)
            if i.ra == 0:
                return Sym('const', imm)
            v = ev(i.ra)
            if v is not None and v.kind in ('const', 'idx'):
                return Sym(v.kind, v.k + imm)
            return None
        if n == 'addic' and i.rd == reg:
            v = ev(i.ra)
            return Sym('const', v.k + i.imm) if v and v.kind == 'const' else None
        if n == 'ori' and i.ra == reg:
            v = ev(i.rd)
            return Sym('const', v.k | i.uimm) if v and v.kind == 'const' else None
        if n == 'or' and i.rd == i.rb and i.ra == reg:
            return ev(i.rd)
        if n == 'lwz' and i.rd == reg:
            base = ev(i.ra) if i.ra else Sym('const', 0)
            if base is None:
                return None
            if base.kind == 'const':
                ea = (base.k + i.imm) & 0xffffffff
                if i.ra == 2 and mod.base <= ea < mod.end - 3:
                    return Sym('const', mod.word(ea))
                return None
            if base.kind == 'idx':
                return Sym('load', base.k + i.imm)
            return None
        if n == 'lwzx' and i.rd == reg:
            a = ev(i.ra) if i.ra else Sym('const', 0)
            b = ev(i.rb)
            if a is None or b is None:
                return None
            if a.kind == 'const' and b.kind == 'idx':
                return Sym('load', a.k + b.k)
            if b.kind == 'const' and a.kind == 'idx':
                return Sym('load', a.k + b.k)
            return None
        if n == 'rlwinm' and i.ra == reg and i.me == 29:   # any field scaled by 4
            return Sym('idx', 0)
        if n in ('add', 'addc') and i.rd == reg:
            a, b = ev(i.ra), ev(i.rb)
            if a is None or b is None:
                return None
            if a.kind != 'const':
                a, b = b, a
            if a.kind == 'const' and b.kind == 'const':
                return Sym('const', a.k + b.k)
            if a.kind == 'const' and b.kind == 'idx':
                return Sym('idx', a.k + b.k)
            if a.kind == 'const' and b.kind == 'load':
                return Sym('loadplus', b.k, a.k)
            return None
        return None


def find_jumptable(fn, site):
    """Switch tables: rlwinm rI,idx,2,..,29 ; lwz/lwzx rX,(table+rI) ; [add rX,rX,base] ;
    mtctr/mtlr rX ; bctr/blr.  Entries are absolute or base-relative."""
    mod = fn.mod
    site_i = fn.insns[site]
    want_spr = 9 if site_i.name == 'bcctr' else 8
    mt = None
    a = site - 4
    for _ in range(10):
        i = fn.insns.get(a)
        if i is None:
            break
        if i.name == 'mtspr' and i.spr == want_spr:
            mt = i
            break
        if is_call(i) or i.name in ('b', 'bclr', 'bcctr', 'rfi') or \
                (i.name == 'mtspr' and i.spr in (8, 9)):
            break
        a -= 4
    if mt is None:
        return None
    v = Slicer(fn).value(mt.rd, mt.addr)
    if v is None or v.kind not in ('load', 'loadplus'):
        return None
    count = None
    a = site - 4
    for _ in range(16):
        i = fn.insns.get(a)
        if i is not None and i.name == 'cmpli':
            count = i.uimm + 1
            break
        a -= 4
    tgts = []
    for k in range(count or 256):
        ea = (v.k + 4 * k) & 0xffffffff
        if not (mod.base <= ea < mod.end - 3):
            return None if count else (tgts or None)
        t = mod.word(ea)
        if v.kind == 'loadplus':
            t = (t + v.plus) & 0xffffffff
        if not mod.contains(t) or t & 3 or mod.insn(t) is None:
            if count:
                return None
            break
        tgts.append(t)
    return tgts or None


def explore(mod, entry, entries):
    """Collect the body of the function starting at `entry`."""
    fn = Function(mod, entry)
    work = [entry]
    forced = mod.local_indirect.get(entry, set())
    for t in forced:
        fn.labels.add(t)
        work.append(t)
    while True:
        while work:
            a = work.pop()
            while a not in fn.insns:
                if not mod.contains(a):
                    fn.bad = True
                    break
                i = mod.insn(a)
                fn.insns[a] = i
                if i is None:
                    fn.bad = True
                    break
                n = i.name
                if n in ('b', 'bc') and i.lk:
                    if i.target != a + 4:
                        fn.calls.add(i.target)
                    a += 4
                    continue
                if n in ('b', 'bc'):
                    t = i.target
                    local = mod.contains(t) and (not (t in entries and t != entry) or t in forced)
                    if local:
                        fn.labels.add(t)
                        work.append(t)
                    if i.unconditional:
                        break
                    a += 4
                    continue
                if n in ('bclr', 'bcctr'):
                    if i.lk or not i.unconditional:
                        a += 4
                        continue
                    if forced:
                        fn.jumptables[a] = sorted(forced)
                    else:
                        fn.indirect.add(a)
                    break
                if n == 'rfi':
                    break
                a += 4
        # resolve switch tables now that more of the CFG is known
        progress = False
        for site in sorted(fn.indirect):
            jt = find_jumptable(fn, site)
            if jt:
                fn.indirect.discard(site)
                fn.jumptables[site] = jt
                for t in jt:
                    fn.labels.add(t)
                    if t not in fn.insns:
                        work.append(t)
                progress = True
        if not progress or not work:
            break
    return fn


def scan_descriptors(mod):
    """PowerOpen function descriptors: {code_addr, TOC, env}."""
    found = set()
    if mod.toc is None:
        return found
    d = mod.data
    for off in range(0, len(d) - 7, 4):
        a, t = struct.unpack_from('>II', d, off)
        if t == mod.toc and mod.contains(a) and not a & 3 and mod.insn(a) is not None:
            found.add(a)
    return found


def is_prologue(w):
    # mflr r0 / stwu r1,-x(r1) / mfcr r12
    return w == 0x7c0802a6 or (w & 0xffff8000) == 0x94218000 or w == 0x7d800026


def scan_code_pointers(mod):
    """Raw code pointers stored in the image that land on a plausible function prologue."""
    found = set()
    d = mod.data
    for off in range(0, len(d) - 3, 4):
        a = struct.unpack_from('>I', d, off)[0]
        if not mod.contains(a) or a & 3 or a == mod.base + off:
            continue
        i = mod.insn(a)
        if i is not None and (is_prologue(i.word) or (i.name == 'stmw' and i.ra == 1)):
            found.add(a)
    return found


def ends_block(w):
    # blr, bctr, rfi, unconditional b (not bl)
    return w in (0x4e800020, 0x4e800420, 0x4c000064) or (w >> 26 == 18 and not w & 1)


def scan_boundaries(mod):
    """Function starts implied by layout: after trailer words, or a prologue right after
    an unconditional branch."""
    found = set()
    lo, hi = mod.text
    for a in range(lo + 4, hi - 4, 4):
        w, p = mod.word(a), mod.word(a - 4)
        if (w & 0xffff0000) == 0x80000000 and ends_block(p) and mod.insn(a + 4) is not None:
            found.add(a + 4)
        elif ends_block(p) and is_prologue(w):
            found.add(a)
    return found


def scan_glue(mod):
    """Cross-module glue stubs: mflr r0; stw r2,12(r1); stw r0,16(r1); ... (exported entry points)."""
    found = set()
    lo, hi = mod.text
    for a in range(lo, hi - 8, 4):
        if mod.word(a) == 0x7c0802a6 and mod.word(a + 4) == 0x9041000c and mod.word(a + 8) == 0x90010010:
            found.add(a)
    return found


def discover(mod, extra=()):
    entries = set(mod.initial) | set(extra)
    entries |= scan_glue(mod)
    entries |= scan_boundaries(mod)
    entries |= scan_descriptors(mod)
    entries |= scan_code_pointers(mod)
    entries = {e for e in entries if mod.contains(e) and mod.insn(e) is not None}
    pending = set(entries)
    while pending:
        new = set()
        for e in pending:
            for t in explore(mod, e, entries).calls:
                if mod.contains(t) and t not in entries and mod.insn(t) is not None:
                    new.add(t)
        entries |= new
        pending = new
    # final pass with the complete entry set, so branches onto entries become tail calls
    funcs = {e: explore(mod, e, entries) for e in sorted(entries)}
    return funcs, entries
