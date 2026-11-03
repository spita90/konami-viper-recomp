"""C code generation for recompiled PPC functions (runtime ABI: runtime/ppc_rt.h)."""
import os

# debug probes: RECOMP_PROBES=addr,addr inserts a TRACE point (breakpoint-capable) before those insns
PROBES = {int(x, 16) for x in os.environ.get('RECOMP_PROBES', '').split(',') if x}
# debug: RECOMP_CYCLE_SCALE=k charges k cycles per instruction (a k times slower CPU)
CYCLE_SCALE = int(os.environ.get('RECOMP_CYCLE_SCALE', '1'))


def mask32(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


def R(n):
    return f"c->r[{n}]"


def RA0(n):
    return "0" if n == 0 else f"c->r[{n}]"


def F(n):
    return f"c->f[{n}]"


def h(v):
    return f"0x{v & 0xffffffff:x}u"


def crbit_expr(bi):
    return f"((c->cr[{bi >> 2}] >> {3 - (bi & 3)}) & 1)"


def cond_expr(bo, bi):
    """Returns (pre_statements, condition) for BO/BI; condition None means always."""
    pre = []
    parts = []
    if not bo & 0x04:
        pre.append("c->ctr--;")
        parts.append("c->ctr == 0" if bo & 0x02 else "c->ctr != 0")
    if not bo & 0x10:
        f, s = bi >> 2, 3 - (bi & 3)
        parts.append(f"(c->cr[{f}] & {1 << s})" if bo & 0x08 else f"!(c->cr[{f}] & {1 << s})")
    return pre, (" && ".join(f"({p})" for p in parts) if parts else None)


class Emitter:
    def __init__(self, fn, entries, resolve_call, name_of):
        self.fn = fn
        self.mod = fn.mod
        self.entries = entries
        self.resolve_call = resolve_call   # addr -> C function name or None (-> rt_call)
        self.name_of = name_of
        self.out = []
        # hand-written dispatchers: callees may "return" into local labels via LR
        self.lr_targets = sorted(fn.mod.local_indirect.get(fn.entry, ()))

    def w(self, s):
        self.out.append("    " + s)

    # ------------------------------------------------------------------ helpers
    def call(self, target, ret_addr, tail=False):
        name = self.resolve_call(self.mod, target)
        if tail:
            if name:
                self.w(f"{name}(c); return;")
            else:
                self.w(f"rt_call(c, {h(target)}); return;")
            return
        self.w(f"c->lr = {h(ret_addr)};")
        if name:
            self.w(f"{name}(c); if (c->unwind) return;")
        else:
            self.w(f"rt_call(c, {h(target)}); if (c->unwind) return;")

    def lr_continue(self):
        """After a callee returned (guest blr to LR): continue at a local label if LR points to one."""
        if not self.lr_targets:
            return "return;"
        cases = " ".join(f"case {h(t)}: goto L_{t:08x};" for t in self.lr_targets)
        return f"switch (c->lr) {{ {cases} default: return; }}"

    def jump(self, a, target):
        forced = target in self.lr_targets
        if target in self.fn.labels and (target == self.fn.entry or target not in self.entries or forced):
            if target <= a:
                # back-edge: interrupt/time checkpoint, only when the branch is taken
                return f"{{ CHK(c, {h(target)}, 0); goto L_{target:08x}; }}"
            return f"goto L_{target:08x};"
        name = self.resolve_call(self.mod, target)
        call = f"{name}(c)" if name else f"rt_call(c, {h(target)})"
        if self.lr_targets:
            return f"{{ {call}; if (c->unwind) return; {self.lr_continue()} }}"
        return f"{{ {call}; return; }}"

    def cr0(self, expr):
        self.w(f"c->cr[0] = CMPS((int32_t)({expr}), 0) | c->xer_so;")

    def rc(self, i, dest):
        if i.rc:
            self.cr0(dest)

    # ------------------------------------------------------------------ main
    def emit(self):
        fn = self.fn
        self.out.append(f"void {fn.name}(PPCContext *c) {{")
        # idle loops in this function: the state of each, and which loop each work address marks
        idle = {end: work for end, work in self.mod.idle.items() if end in fn.insns}
        work = {w: end for end, ws in idle.items() for w in ws}
        for end in idle:
            self.w(f"static RtIdle idle_{end:08x};")
        self.w(f"CHK(c, {h(fn.entry)}, 0);")
        if min(fn.insns) != fn.entry:
            self.w(f"goto L_{fn.entry:08x};")   # body contains code below the entry point
        # cycle accounting: each basic block charges its own instruction count when entered
        addrs = sorted(fn.insns)
        starts = {fn.entry} | set(fn.labels)
        for a in addrs:
            i = fn.insns[a]
            if i is not None and i.name in ('bc', 'bclr', 'bcctr') and not self.ends_flow(i):
                starts.add(a + 4)
        blen = {}
        for a in addrs:
            if a not in starts:
                continue
            n, b = 0, a
            while b in fn.insns:
                n += 1
                ib = fn.insns[b]
                if ib is None or self.ends_flow(ib) or (b + 4) in starts:
                    break
                b += 4
            blen[a] = n
        for a in addrs:
            i = fn.insns[a]
            if a in fn.labels or a == fn.entry:
                self.out.append(f"  L_{a:08x}:")
                self.w(f"TRACE(c, {h(a)});")
            if a in blen:
                self.w(f"c->budget -= {blen[a] * CYCLE_SCALE};")
            if i is None:
                self.w(f"rt_bad_insn(c, {h(a)}, {h(self.mod.word(a))}); return;")
                continue
            if a in PROBES and not (a in fn.labels or a == fn.entry):
                self.w(f"TRACE(c, {h(a)});")
            if a in self.mod.hooks:
                self.w(f"rt_hook(c, {h(a)});")
            if a in idle:
                self.w(f"rt_idle(c, &idle_{a:08x});")
            if a in work:
                self.w(f"idle_{work[a]:08x}.clean = 0;")
            self.insn(i)
            # every block either ends in a branch or continues at a+4 (which is in the body)
            if not self.ends_flow(i) and a + 4 not in fn.insns:
                self.w(f"rt_fallthrough(c, {h(a + 4)}); return;")
        self.w("return;")
        self.out.append("}")
        return "\n".join(self.out)

    @staticmethod
    def ends_flow(i):
        if i.name == 'b' and not i.lk:
            return True
        if i.name in ('bc', 'bclr', 'bcctr') and i.unconditional and not i.lk:
            return True
        return i.name == 'rfi'

    # ------------------------------------------------------------------ instructions
    def insn(self, i):
        n = i.name.replace('.', '_d')
        m = getattr(self, 'i_' + n, None)
        a = i.addr
        self.w(f"/* {a:08x}: {i.word:08x} {i.name} */")
        if m is None:
            self.w(f"rt_unimpl(c, {h(a)}, {h(i.word)});")
            return
        m(i)

    # ---- branches
    def i_b(self, i):
        if i.lk:
            if i.target == i.addr + 4:
                self.w(f"c->lr = {h(i.addr + 4)};")
            else:
                self.call(i.target, i.addr + 4)
            return
        self.w(self.jump(i.addr, i.target))

    def i_bc(self, i):
        pre, cond = cond_expr(i.bo, i.bi)
        for p in pre:
            self.w(p)
        if i.lk:
            if i.target == i.addr + 4:
                self.w(f"c->lr = {h(i.addr + 4)};")
                return
            if cond:
                self.w(f"if ({cond}) {{")
            self.call(i.target, i.addr + 4)
            if cond:
                self.w("}")
            return
        j = self.jump(i.addr, i.target)
        if cond:
            self.w(f"if ({cond}) {j}")
        else:
            self.w(j)

    def i_bclr(self, i):
        pre, cond = cond_expr(i.bo, i.bi)
        for p in pre:
            self.w(p)
        jt = self.fn.jumptables.get(i.addr)
        if jt:  # switch implemented as mtlr rX; blr
            self.w("switch (c->lr) {")
            for t in sorted(set(jt)):
                self.w(f"case {h(t)}: goto L_{t:08x};")
            self.w("default: break;")
            self.w("}")
        if i.lk:
            body = f"{{ uint32_t t = c->lr; c->lr = {h(i.addr + 4)}; rt_call(c, t); if (c->unwind) return; }}"
        else:
            body = "RETURN(c);"
        self.w(f"if ({cond}) {body}" if cond else body)

    def i_bcctr(self, i):
        pre, cond = cond_expr(i.bo, i.bi)
        for p in pre:
            self.w(p)
        if i.lk:
            body = f"{{ c->lr = {h(i.addr + 4)}; rt_call(c, c->ctr); if (c->unwind) return; }}"
            self.w(f"if ({cond}) {body}" if cond else body)
            return
        jt = self.fn.jumptables.get(i.addr)
        if jt:
            self.w("switch (c->ctr) {")
            for t in sorted(set(jt)):
                self.w(f"case {h(t)}: goto L_{t:08x};")
            self.w("default: break;")
            self.w("}")
        if self.lr_targets:
            body = f"{{ rt_call(c, c->ctr); if (c->unwind) return; {self.lr_continue()} }}"
        else:
            body = "{ rt_call(c, c->ctr); return; }"
        self.w(f"if ({cond}) {body}" if cond else body)

    def i_sc(self, i):
        self.w(f"if (rt_sc(c, {h(i.addr + 4)}) || c->unwind) return;")

    def i_rfi(self, i):
        self.w("rt_rfi(c); return;")

    def i_twi(self, i):
        self.trap(i, R(i.ra), f"(uint32_t){i.imm}")

    def i_tw(self, i):
        self.trap(i, R(i.ra), R(i.rb))

    def trap(self, i, a, b):
        to = i.rd
        conds = []
        if to & 16: conds.append(f"(int32_t){a} < (int32_t){b}")
        if to & 8: conds.append(f"(int32_t){a} > (int32_t){b}")
        if to & 4: conds.append(f"{a} == {b}")
        if to & 2: conds.append(f"{a} < {b}")
        if to & 1: conds.append(f"{a} > {b}")
        if to == 31:
            self.w(f"rt_trap(c, {h(i.addr)});")
        elif conds:
            self.w(f"if ({' || '.join(conds)}) rt_trap(c, {h(i.addr)});")

    # ---- CR ops
    def i_mcrf(self, i):
        self.w(f"c->cr[{i.rd >> 2}] = c->cr[{i.ra >> 2}];")

    def crop(self, i, expr):
        d, a, b = i.rd, i.ra, i.rb
        self.w(f"{{ uint32_t a = {crbit_expr(a)}, b = {crbit_expr(b)}; uint32_t v = ({expr}) & 1;"
               f" c->cr[{d >> 2}] = (c->cr[{d >> 2}] & ~{1 << (3 - (d & 3))}) | (v << {3 - (d & 3)}); }}")

    def i_crand(self, i): self.crop(i, "a & b")
    def i_cror(self, i): self.crop(i, "a | b")
    def i_crxor(self, i): self.crop(i, "a ^ b")
    def i_crnand(self, i): self.crop(i, "~(a & b)")
    def i_crnor(self, i): self.crop(i, "~(a | b)")
    def i_creqv(self, i): self.crop(i, "~(a ^ b)")
    def i_crandc(self, i): self.crop(i, "a & ~b")
    def i_crorc(self, i): self.crop(i, "a | ~b")

    def i_isync(self, i): pass
    def i_sync(self, i): pass
    def i_eieio(self, i): pass

    # ---- integer arithmetic (D-form)
    def i_addi(self, i):
        self.w(f"{R(i.rd)} = {h(i.imm)};" if i.ra == 0 else f"{R(i.rd)} = {R(i.ra)} + {h(i.imm)};")

    def i_addis(self, i):
        v = (i.uimm << 16) & 0xffffffff
        self.w(f"{R(i.rd)} = {h(v)};" if i.ra == 0 else f"{R(i.rd)} = {R(i.ra)} + {h(v)};")

    def i_addic(self, i):
        self.w(f"{{ uint64_t t = (uint64_t){R(i.ra)} + {h(i.imm)}; c->xer_ca = (uint8_t)(t >> 32); {R(i.rd)} = (uint32_t)t; }}")

    def i_addic_d(self, i):
        self.i_addic(i)
        self.cr0(R(i.rd))

    def i_subfic(self, i):
        self.w(f"{{ uint64_t t = (uint64_t)(uint32_t)~{R(i.ra)} + {h(i.imm)} + 1; c->xer_ca = (uint8_t)(t >> 32); {R(i.rd)} = (uint32_t)t; }}")

    def i_mulli(self, i):
        self.w(f"{R(i.rd)} = {R(i.ra)} * {h(i.imm)};")

    def cmp_signed(self, crf, a, b):
        self.w(f"c->cr[{crf}] = CMPS((int32_t)({a}), (int32_t)({b})) | c->xer_so;")

    def cmp_unsigned(self, crf, a, b):
        self.w(f"c->cr[{crf}] = CMPU((uint32_t)({a}), (uint32_t)({b})) | c->xer_so;")

    def i_cmpi(self, i): self.cmp_signed(i.rd >> 2, R(i.ra), str(i.imm))
    def i_cmpli(self, i): self.cmp_unsigned(i.rd >> 2, R(i.ra), h(i.uimm))
    def i_cmp(self, i): self.cmp_signed(i.rd >> 2, R(i.ra), R(i.rb))
    def i_cmpl(self, i): self.cmp_unsigned(i.rd >> 2, R(i.ra), R(i.rb))

    # logical immediate: rS = rd field, rA = ra field
    def i_ori(self, i):
        if i.word == 0x60000000:
            return
        self.w(f"{R(i.ra)} = {R(i.rd)} | {h(i.uimm)};")

    def i_oris(self, i): self.w(f"{R(i.ra)} = {R(i.rd)} | {h(i.uimm << 16)};")
    def i_xori(self, i): self.w(f"{R(i.ra)} = {R(i.rd)} ^ {h(i.uimm)};")
    def i_xoris(self, i): self.w(f"{R(i.ra)} = {R(i.rd)} ^ {h(i.uimm << 16)};")

    def i_andi_d(self, i):
        self.w(f"{R(i.ra)} = {R(i.rd)} & {h(i.uimm)};")
        self.cr0(R(i.ra))

    def i_andis_d(self, i):
        self.w(f"{R(i.ra)} = {R(i.rd)} & {h(i.uimm << 16)};")
        self.cr0(R(i.ra))

    # rotates
    def i_rlwinm(self, i):
        m = mask32(i.mb, i.me)
        src = f"ROTL({R(i.rd)}, {i.sh})" if i.sh else R(i.rd)
        self.w(f"{R(i.ra)} = {src} & {h(m)};")
        self.rc(i, R(i.ra))

    def i_rlwnm(self, i):
        m = mask32(i.mb, i.me)
        self.w(f"{R(i.ra)} = ROTL({R(i.rd)}, {R(i.rb)} & 31) & {h(m)};")
        self.rc(i, R(i.ra))

    def i_rlwimi(self, i):
        m = mask32(i.mb, i.me)
        src = f"ROTL({R(i.rd)}, {i.sh})" if i.sh else R(i.rd)
        self.w(f"{R(i.ra)} = ({src} & {h(m)}) | ({R(i.ra)} & {h(~m)});")
        self.rc(i, R(i.ra))

    # ---- X/XO form integer
    def arith(self, i, x, y, cin, carry, ovf_expr=None):
        """d = x + y + cin (64-bit); x,y C expressions (uint32)."""
        self.w(f"{{ uint32_t x = {x}, y = {y}; uint64_t t = (uint64_t)x + y + {cin}; uint32_t r = (uint32_t)t;")
        if carry:
            self.w("  c->xer_ca = (uint8_t)(t >> 32);")
        if i.oe:
            self.w("  c->xer_ov = (uint8_t)((((x ^ r) & (y ^ r)) >> 31) & 1); c->xer_so |= c->xer_ov;")
        self.w(f"  {R(i.rd)} = r; }}")
        self.rc(i, R(i.rd))

    def i_add(self, i): self.arith(i, R(i.ra), R(i.rb), "0", False)
    def i_addc(self, i): self.arith(i, R(i.ra), R(i.rb), "0", True)
    def i_adde(self, i): self.arith(i, R(i.ra), R(i.rb), "c->xer_ca", True)
    def i_addze(self, i): self.arith(i, R(i.ra), "0", "c->xer_ca", True)
    def i_addme(self, i): self.arith(i, R(i.ra), "0xffffffffu", "c->xer_ca", True)
    def i_subf(self, i): self.arith(i, f"~{R(i.ra)}", R(i.rb), "1", False)
    def i_subfc(self, i): self.arith(i, f"~{R(i.ra)}", R(i.rb), "1", True)
    def i_subfe(self, i): self.arith(i, f"~{R(i.ra)}", R(i.rb), "c->xer_ca", True)
    def i_subfze(self, i): self.arith(i, f"~{R(i.ra)}", "0", "c->xer_ca", True)
    def i_subfme(self, i): self.arith(i, f"~{R(i.ra)}", "0xffffffffu", "c->xer_ca", True)

    def i_neg(self, i):
        if i.oe:
            self.w(f"c->xer_ov = ({R(i.ra)} == 0x80000000u); c->xer_so |= c->xer_ov;")
        self.w(f"{R(i.rd)} = 0u - {R(i.ra)};")
        self.rc(i, R(i.rd))

    def i_mullw(self, i):
        self.w(f"{{ int64_t t = (int64_t)(int32_t){R(i.ra)} * (int32_t){R(i.rb)};")
        if i.oe:
            self.w("  c->xer_ov = (t != (int32_t)t); c->xer_so |= c->xer_ov;")
        self.w(f"  {R(i.rd)} = (uint32_t)t; }}")
        self.rc(i, R(i.rd))

    def i_mulhw(self, i):
        self.w(f"{R(i.rd)} = (uint32_t)(((int64_t)(int32_t){R(i.ra)} * (int32_t){R(i.rb)}) >> 32);")
        self.rc(i, R(i.rd))

    def i_mulhwu(self, i):
        self.w(f"{R(i.rd)} = (uint32_t)(((uint64_t){R(i.ra)} * {R(i.rb)}) >> 32);")
        self.rc(i, R(i.rd))

    def i_divw(self, i):
        self.w(f"{R(i.rd)} = rt_divw(c, {R(i.ra)}, {R(i.rb)}, {i.oe});")
        self.rc(i, R(i.rd))

    def i_divwu(self, i):
        self.w(f"{R(i.rd)} = rt_divwu(c, {R(i.ra)}, {R(i.rb)}, {i.oe});")
        self.rc(i, R(i.rd))

    def logic(self, i, expr):
        self.w(f"{R(i.ra)} = {expr};")
        self.rc(i, R(i.ra))

    def i_and(self, i): self.logic(i, f"{R(i.rd)} & {R(i.rb)}")
    def i_andc(self, i): self.logic(i, f"{R(i.rd)} & ~{R(i.rb)}")
    def i_or(self, i): self.logic(i, f"{R(i.rd)} | {R(i.rb)}")
    def i_orc(self, i): self.logic(i, f"{R(i.rd)} | ~{R(i.rb)}")
    def i_xor(self, i): self.logic(i, f"{R(i.rd)} ^ {R(i.rb)}")
    def i_nand(self, i): self.logic(i, f"~({R(i.rd)} & {R(i.rb)})")
    def i_nor(self, i): self.logic(i, f"~({R(i.rd)} | {R(i.rb)})")
    def i_eqv(self, i): self.logic(i, f"~({R(i.rd)} ^ {R(i.rb)})")
    def i_extsb(self, i): self.logic(i, f"(uint32_t)(int32_t)(int8_t){R(i.rd)}")
    def i_extsh(self, i): self.logic(i, f"(uint32_t)(int32_t)(int16_t){R(i.rd)}")
    def i_cntlzw(self, i): self.logic(i, f"CLZ32({R(i.rd)})")

    def i_slw(self, i):
        self.logic(i, f"(({R(i.rb)} & 0x20) ? 0u : ({R(i.rd)} << ({R(i.rb)} & 31)))")

    def i_srw(self, i):
        self.logic(i, f"(({R(i.rb)} & 0x20) ? 0u : ({R(i.rd)} >> ({R(i.rb)} & 31)))")

    def i_sraw(self, i):
        self.w(f"{R(i.ra)} = rt_sraw(c, {R(i.rd)}, {R(i.rb)} & 0x3f);")
        self.rc(i, R(i.ra))

    def i_srawi(self, i):
        s = i.sh
        if s == 0:
            self.w(f"{R(i.ra)} = {R(i.rd)}; c->xer_ca = 0;")
        else:
            self.w(f"{{ int32_t v = (int32_t){R(i.rd)}; c->xer_ca = (v < 0) && (v & {h((1 << s) - 1)});"
                   f" {R(i.ra)} = (uint32_t)(v >> {s}); }}")
        self.rc(i, R(i.ra))

    # ---- special registers
    def i_mfcr(self, i): self.w(f"{R(i.rd)} = rt_cr_pack(c);")
    def i_mtcrf(self, i): self.w(f"rt_cr_unpack(c, {R(i.rd)}, {h(i.crm)});")
    def i_mfmsr(self, i): self.w(f"{R(i.rd)} = c->msr;")

    def i_mtmsr(self, i):
        self.w(f"rt_mtmsr(c, {R(i.rd)}, {h(i.addr + 4)}); if (c->unwind) return;")

    def i_mcrxr(self, i):
        self.w(f"c->cr[{i.rd >> 2}] = (c->xer_so << 3) | (c->xer_ov << 2) | (c->xer_ca << 1); c->xer_so = c->xer_ov = c->xer_ca = 0;")

    def i_mfspr(self, i):
        s = i.spr
        if s == 8:
            self.w(f"{R(i.rd)} = c->lr;")
        elif s == 9:
            self.w(f"{R(i.rd)} = c->ctr;")
        elif s == 1:
            self.w(f"{R(i.rd)} = rt_xer_pack(c);")
        elif s == 26:
            self.w(f"{R(i.rd)} = c->srr0;")
        elif s == 27:
            self.w(f"{R(i.rd)} = c->srr1;")
        elif 272 <= s <= 275:
            self.w(f"{R(i.rd)} = c->sprg[{s - 272}];")
        else:
            self.w(f"{R(i.rd)} = rt_mfspr(c, {s});")

    def i_mtspr(self, i):
        s = i.spr
        if s == 8:
            self.w(f"c->lr = {R(i.rd)};")
        elif s == 9:
            self.w(f"c->ctr = {R(i.rd)};")
        elif s == 1:
            self.w(f"rt_xer_unpack(c, {R(i.rd)});")
        elif s == 26:
            self.w(f"c->srr0 = {R(i.rd)};")
        elif s == 27:
            self.w(f"c->srr1 = {R(i.rd)};")
        elif 272 <= s <= 275:
            self.w(f"c->sprg[{s - 272}] = {R(i.rd)};")
        else:
            self.w(f"rt_mtspr(c, {s}, {R(i.rd)});")

    def i_mftb(self, i):
        self.w(f"{R(i.rd)} = rt_mftb(c, {i.spr});")

    def i_mfsr(self, i): self.w(f"{R(i.rd)} = c->sr[{i.ra & 15}];")
    def i_mtsr(self, i): self.w(f"c->sr[{i.ra & 15}] = {R(i.rd)};")
    def i_mfsrin(self, i): self.w(f"{R(i.rd)} = c->sr[{R(i.rb)} >> 28];")
    def i_mtsrin(self, i): self.w(f"c->sr[{R(i.rb)} >> 28] = {R(i.rd)};")

    # cache / tlb: no-ops except dcbz
    def i_dcbst(self, i): pass
    def i_dcbf(self, i): pass
    def i_dcbt(self, i): pass
    def i_dcbtst(self, i): pass
    def i_dcbi(self, i): pass
    def i_icbi(self, i): pass
    def i_tlbie(self, i): pass
    def i_tlbia(self, i): pass
    def i_tlbsync(self, i): pass
    def i_tlbld(self, i): pass
    def i_tlbli(self, i): pass

    def i_dcbz(self, i):
        self.w(f"rt_dcbz(c, {RA0(i.ra)} + {R(i.rb)});")

    # ---- loads / stores
    def ea_d(self, i):
        return f"({RA0(i.ra)} + {h(i.imm)})" if i.imm else f"{RA0(i.ra)}"

    def ea_x(self, i):
        return f"({RA0(i.ra)} + {R(i.rb)})"

    def load(self, i, fn, ea, upd, cast=""):
        if upd:
            self.w(f"{{ uint32_t ea = {ea}; {R(i.rd)} = {cast}{fn}(ea); {R(i.ra)} = ea; }}")
        else:
            self.w(f"{R(i.rd)} = {cast}{fn}({ea});")

    def store(self, i, fn, ea, val, upd):
        if upd:
            self.w(f"{{ uint32_t ea = {ea}; {fn}(ea, {val}); {R(i.ra)} = ea; }}")
        else:
            self.w(f"{fn}({ea}, {val});")

    def i_lwz(self, i): self.load(i, "LD32", self.ea_d(i), False)
    def i_lwzu(self, i): self.load(i, "LD32", self.ea_d(i), True)
    def i_lbz(self, i): self.load(i, "LD8", self.ea_d(i), False)
    def i_lbzu(self, i): self.load(i, "LD8", self.ea_d(i), True)
    def i_lhz(self, i): self.load(i, "LD16", self.ea_d(i), False)
    def i_lhzu(self, i): self.load(i, "LD16", self.ea_d(i), True)
    def i_lha(self, i): self.load(i, "LD16", self.ea_d(i), False, "(uint32_t)(int32_t)(int16_t)")
    def i_lhau(self, i): self.load(i, "LD16", self.ea_d(i), True, "(uint32_t)(int32_t)(int16_t)")
    def i_lwzx(self, i): self.load(i, "LD32", self.ea_x(i), False)
    def i_lwzux(self, i): self.load(i, "LD32", self.ea_x(i), True)
    def i_lbzx(self, i): self.load(i, "LD8", self.ea_x(i), False)
    def i_lbzux(self, i): self.load(i, "LD8", self.ea_x(i), True)
    def i_lhzx(self, i): self.load(i, "LD16", self.ea_x(i), False)
    def i_lhzux(self, i): self.load(i, "LD16", self.ea_x(i), True)
    def i_lhax(self, i): self.load(i, "LD16", self.ea_x(i), False, "(uint32_t)(int32_t)(int16_t)")
    def i_lhaux(self, i): self.load(i, "LD16", self.ea_x(i), True, "(uint32_t)(int32_t)(int16_t)")
    def i_lwbrx(self, i): self.load(i, "LD32LE", self.ea_x(i), False)
    def i_lhbrx(self, i): self.load(i, "LD16LE", self.ea_x(i), False)

    def i_lwarx(self, i):
        self.w(f"{R(i.rd)} = LD32({self.ea_x(i)}); c->reserve = 1;")

    def i_stwcx_d(self, i):
        self.w(f"if (c->reserve) {{ ST32({self.ea_x(i)}, {R(i.rd)}); c->reserve = 0; c->cr[0] = 2 | c->xer_so; }}"
               f" else c->cr[0] = c->xer_so;")

    def i_stw(self, i): self.store(i, "ST32", self.ea_d(i), R(i.rd), False)
    def i_stwu(self, i): self.store(i, "ST32", self.ea_d(i), R(i.rd), True)
    def i_stb(self, i): self.store(i, "ST8", self.ea_d(i), R(i.rd), False)
    def i_stbu(self, i): self.store(i, "ST8", self.ea_d(i), R(i.rd), True)
    def i_sth(self, i): self.store(i, "ST16", self.ea_d(i), R(i.rd), False)
    def i_sthu(self, i): self.store(i, "ST16", self.ea_d(i), R(i.rd), True)
    def i_stwx(self, i): self.store(i, "ST32", self.ea_x(i), R(i.rd), False)
    def i_stwux(self, i): self.store(i, "ST32", self.ea_x(i), R(i.rd), True)
    def i_stbx(self, i): self.store(i, "ST8", self.ea_x(i), R(i.rd), False)
    def i_stbux(self, i): self.store(i, "ST8", self.ea_x(i), R(i.rd), True)
    def i_sthx(self, i): self.store(i, "ST16", self.ea_x(i), R(i.rd), False)
    def i_sthux(self, i): self.store(i, "ST16", self.ea_x(i), R(i.rd), True)
    def i_stwbrx(self, i): self.store(i, "ST32LE", self.ea_x(i), R(i.rd), False)
    def i_sthbrx(self, i): self.store(i, "ST16LE", self.ea_x(i), R(i.rd), False)

    def i_lmw(self, i):
        self.w(f"{{ uint32_t ea = {self.ea_d(i)}; for (int k = {i.rd}; k < 32; k++, ea += 4) c->r[k] = LD32(ea); }}")

    def i_stmw(self, i):
        self.w(f"{{ uint32_t ea = {self.ea_d(i)}; for (int k = {i.rd}; k < 32; k++, ea += 4) ST32(ea, c->r[k]); }}")

    def i_lswi(self, i):
        self.w(f"rt_lswi(c, {RA0(i.ra)}, {i.rd}, {i.rb or 32});")

    def i_stswi(self, i):
        self.w(f"rt_stswi(c, {RA0(i.ra)}, {i.rd}, {i.rb or 32});")

    def i_lswx(self, i):
        self.w(f"rt_lswi(c, {self.ea_x(i)}, {i.rd}, c->xer_bc);")

    def i_stswx(self, i):
        self.w(f"rt_stswi(c, {self.ea_x(i)}, {i.rd}, c->xer_bc);")

    def i_eciwx(self, i): self.w(f"rt_unimpl(c, {h(i.addr)}, {h(i.word)});")
    def i_ecowx(self, i): self.w(f"rt_unimpl(c, {h(i.addr)}, {h(i.word)});")

    # ---- FP loads/stores
    def fload(self, i, single, ea, upd):
        fnc = "LDF32" if single else "LDF64"
        if upd:
            self.w(f"{{ uint32_t ea = {ea}; {F(i.rd)} = {fnc}(ea); {R(i.ra)} = ea; }}")
        else:
            self.w(f"{F(i.rd)} = {fnc}({ea});")

    def fstore(self, i, single, ea, upd):
        fnc = "STF32" if single else "STF64"
        if upd:
            self.w(f"{{ uint32_t ea = {ea}; {fnc}(ea, {F(i.rd)}); {R(i.ra)} = ea; }}")
        else:
            self.w(f"{fnc}({ea}, {F(i.rd)});")

    def i_lfs(self, i): self.fload(i, True, self.ea_d(i), False)
    def i_lfsu(self, i): self.fload(i, True, self.ea_d(i), True)
    def i_lfd(self, i): self.fload(i, False, self.ea_d(i), False)
    def i_lfdu(self, i): self.fload(i, False, self.ea_d(i), True)
    def i_lfsx(self, i): self.fload(i, True, self.ea_x(i), False)
    def i_lfsux(self, i): self.fload(i, True, self.ea_x(i), True)
    def i_lfdx(self, i): self.fload(i, False, self.ea_x(i), False)
    def i_lfdux(self, i): self.fload(i, False, self.ea_x(i), True)
    def i_stfs(self, i): self.fstore(i, True, self.ea_d(i), False)
    def i_stfsu(self, i): self.fstore(i, True, self.ea_d(i), True)
    def i_stfd(self, i): self.fstore(i, False, self.ea_d(i), False)
    def i_stfdu(self, i): self.fstore(i, False, self.ea_d(i), True)
    def i_stfsx(self, i): self.fstore(i, True, self.ea_x(i), False)
    def i_stfsux(self, i): self.fstore(i, True, self.ea_x(i), True)
    def i_stfdx(self, i): self.fstore(i, False, self.ea_x(i), False)
    def i_stfdux(self, i): self.fstore(i, False, self.ea_x(i), True)

    def i_stfiwx(self, i):
        self.w(f"ST32({self.ea_x(i)}, (uint32_t)FPR_BITS({F(i.rd)}));")

    # ---- FP arithmetic
    def frc(self, i):
        if i.rc:
            self.w("c->cr[1] = (uint8_t)(c->fpscr >> 28);")

    def fop(self, i, expr, single):
        self.w(f"{F(i.rd)} = {'ROUND_S' if single else ''}({expr});")
        self.frc(i)

    def i_fadd(self, i): self.fop(i, f"{F(i.ra)} + {F(i.rb)}", False)
    def i_fadds(self, i): self.fop(i, f"{F(i.ra)} + {F(i.rb)}", True)
    def i_fsub(self, i): self.fop(i, f"{F(i.ra)} - {F(i.rb)}", False)
    def i_fsubs(self, i): self.fop(i, f"{F(i.ra)} - {F(i.rb)}", True)
    def i_fmul(self, i): self.fop(i, f"{F(i.ra)} * {F(i.frc)}", False)
    def i_fmuls(self, i): self.fop(i, f"{F(i.ra)} * {F(i.frc)}", True)
    def i_fdiv(self, i): self.fop(i, f"{F(i.ra)} / {F(i.rb)}", False)
    def i_fdivs(self, i): self.fop(i, f"{F(i.ra)} / {F(i.rb)}", True)
    def i_fmadd(self, i): self.fop(i, f"fma({F(i.ra)}, {F(i.frc)}, {F(i.rb)})", False)
    def i_fmadds(self, i): self.fop(i, f"fma({F(i.ra)}, {F(i.frc)}, {F(i.rb)})", True)
    def i_fmsub(self, i): self.fop(i, f"fma({F(i.ra)}, {F(i.frc)}, -{F(i.rb)})", False)
    def i_fmsubs(self, i): self.fop(i, f"fma({F(i.ra)}, {F(i.frc)}, -{F(i.rb)})", True)
    def i_fnmadd(self, i): self.fop(i, f"-fma({F(i.ra)}, {F(i.frc)}, {F(i.rb)})", False)
    def i_fnmadds(self, i): self.fop(i, f"-fma({F(i.ra)}, {F(i.frc)}, {F(i.rb)})", True)
    def i_fnmsub(self, i): self.fop(i, f"-fma({F(i.ra)}, {F(i.frc)}, -{F(i.rb)})", False)
    def i_fnmsubs(self, i): self.fop(i, f"-fma({F(i.ra)}, {F(i.frc)}, -{F(i.rb)})", True)
    def i_fsqrt(self, i): self.fop(i, f"sqrt({F(i.rb)})", False)
    def i_fsqrts(self, i): self.fop(i, f"sqrt({F(i.rb)})", True)
    def i_fres(self, i): self.fop(i, f"1.0 / {F(i.rb)}", True)
    def i_frsqrte(self, i): self.fop(i, f"1.0 / sqrt({F(i.rb)})", False)
    def i_frsp(self, i): self.fop(i, F(i.rb), True)
    def i_fmr(self, i): self.fop(i, F(i.rb), False)
    def i_fneg(self, i): self.fop(i, f"-{F(i.rb)}", False)
    def i_fabs(self, i): self.fop(i, f"fabs({F(i.rb)})", False)
    def i_fnabs(self, i): self.fop(i, f"-fabs({F(i.rb)})", False)

    def i_fsel(self, i):
        self.fop(i, f"({F(i.ra)} >= 0.0) ? {F(i.frc)} : {F(i.rb)}", False)

    def i_fctiwz(self, i):
        self.w(f"{F(i.rd)} = rt_fctiw(c, {F(i.rb)}, 1);")
        self.frc(i)

    def i_fctiw(self, i):
        self.w(f"{F(i.rd)} = rt_fctiw(c, {F(i.rb)}, 0);")
        self.frc(i)

    def i_fcmpu(self, i):
        self.w(f"c->cr[{i.rd >> 2}] = CMPF({F(i.ra)}, {F(i.rb)});")

    i_fcmpo = i_fcmpu

    def i_mffs(self, i):
        self.w(f"{F(i.rd)} = BITS_FPR(0xfff8000000000000ull | c->fpscr);")
        self.frc(i)

    def i_mtfsf(self, i):
        self.w(f"rt_mtfsf(c, {h(i.fm)}, (uint32_t)FPR_BITS({F(i.rb)}));")
        self.frc(i)

    def i_mtfsfi(self, i):
        crf = i.rd >> 2
        imm = (i.word >> 12) & 15
        self.w(f"rt_mtfsf(c, {h(1 << (7 - crf))}, {h(imm << (4 * (7 - crf)))});")
        self.frc(i)

    def i_mtfsb0(self, i):
        self.w(f"c->fpscr &= ~{h(1 << (31 - i.rd))}; rt_fpscr_changed(c);")
        self.frc(i)

    def i_mtfsb1(self, i):
        self.w(f"c->fpscr |= {h(1 << (31 - i.rd))}; rt_fpscr_changed(c);")
        self.frc(i)

    def i_mcrfs(self, i):
        s = i.ra >> 2
        self.w(f"c->cr[{i.rd >> 2}] = (c->fpscr >> {4 * (7 - s)}) & 15;")
