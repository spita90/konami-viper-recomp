/*
 * CPU-side runtime: dispatch, exceptions, task fibers, virtual time, ALU/FPU helpers.
 *
 * Task switching model
 * --------------------
 * The Viper kernel switches tasks by loading another task's context and executing
 * `rfi` from an exception handler (or from its context-restore routine at 0xC70).
 * Every guest task runs on its own host fiber.  Whenever an exception is taken at a
 * checkpoint, the fiber records a resume key (srr0, r1).  On `rfi` to (pc, sp):
 *   - key belongs to the current fiber   -> unwind host frames back to that checkpoint;
 *   - key belongs to a suspended fiber   -> hand the CPU to that fiber;
 *   - unknown key                        -> start (or recycle) a fiber at pc.
 * Fibers are host threads that pass a baton, so exactly one runs at a time.
 */
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fenv.h>

PPCContext g_ctx;

/* ============================================================ dispatch */
#define DISP_BITS 16
#define DISP_SIZE (1u << DISP_BITS)
typedef struct DispNode { const RtFunc *f; const RtModuleInfo *m; struct DispNode *next; } DispNode;
static DispNode *g_disp[DISP_SIZE];

static inline unsigned disp_hash(uint32_t a) { return (a * 2654435761u) >> (32 - DISP_BITS); }

void rt_register_module(const RtModuleInfo *m) {
    for (unsigned i = 0; i < m->nfuncs; i++) {
        DispNode *n = (DispNode *)malloc(sizeof *n);
        n->f = &m->funcs[i];
        n->m = m;
        unsigned h = disp_hash(m->funcs[i].addr);
        n->next = g_disp[h];
        g_disp[h] = n;
    }
    rt_log("module %-9s %08x-%08x: %u functions\n", m->name, m->base, m->end, m->nfuncs);
}

static int module_resident(const RtModuleInfo *m) {
    if (m->base >= RAM_LIMIT) return 1;
    for (int k = 0; k < 8; k++)
        if (LD32(m->base + 4 * k) != m->sig[k]) return 0;
    return 1;
}

RtFn rt_lookup(uint32_t addr) {
    uint32_t w = (addr < RAM_LIMIT) ? LD32(addr) : 0;
    RtFn any = NULL, match = NULL;
    int nmatch = 0;
    for (DispNode *n = g_disp[disp_hash(addr)]; n; n = n->next) {
        if (n->f->addr != addr) continue;
        if (n->f->first_word == w) {
            if (!match || module_resident(n->m)) match = n->f->fn;
            nmatch++;
        }
        any = n->f->fn;
    }
    if (match) return match;
    if (any && addr < RAM_LIMIT) {
        static uint32_t warned[64]; static int nw;
        int seen = 0;
        for (int i = 0; i < nw; i++) seen |= warned[i] == addr;
        if (!seen && nw < 64) { warned[nw++] = addr; rt_log("warning: code at %08x modified in RAM (now %08x)\n", addr, w); }
    }
    return any; /* code in RAM differs (patched?) - still better than nothing */
}

static void dispatch_miss(PPCContext *c, uint32_t target) {
    rt_log("FATAL: no recompiled function at %08x (lr=%08x r1=%08x)\n", target, c->lr, c->r[1]);
    if (target < RAM_LIMIT)
        for (int i = 0; i < 16; i += 4)
            rt_log("  %08x: %08x %08x %08x %08x\n", target + 4 * i, LD32(target + 4 * i), LD32(target + 4 * i + 4),
                   LD32(target + 4 * i + 8), LD32(target + 4 * i + 12));
    FILE *f = fopen("dispatch_miss.txt", "a");
    if (f) { fprintf(f, "%08x\n", target); fclose(f); }
    rt_fatal("dispatch miss");
}

/* Export stubs built in RAM by the kernel for loaded modules:
 *   mflr r0; stw r2,12(r1); stw r0,16(r1); lwz r2,0x2c(0); bl F; lwz r0,16(r1); lwz r2,12(r1); mtlr r0; blr */
static int run_kernel_stub(PPCContext *c, uint32_t a) {
    static const uint32_t tmpl[9] = {0x7c0802a6, 0x9041000c, 0x90010010, 0x8040002c, 0,
                                     0x80010010, 0x8041000c, 0x7c0803a6, 0x4e800020};
    if (a >= RAM_LIMIT) return 0;
    for (int i = 0; i < 9; i++)
        if (i != 4 && LD32(a + 4 * i) != tmpl[i]) return 0;
    uint32_t bl = LD32(a + 16);
    if ((bl & 0xfc000003) != 0x48000001) return 0;
    uint32_t f = a + 16 + (uint32_t)(((int32_t)((bl & 0x03fffffc) << 6)) >> 6);
    c->r[0] = c->lr;
    ST32(c->r[1] + 12, c->r[2]);
    ST32(c->r[1] + 16, c->r[0]);
    c->r[2] = LD32(0x2c);
    c->lr = a + 20;
    rt_call(c, f);
    if (c->unwind) return 1;
    c->r[0] = LD32(c->r[1] + 16);
    c->r[2] = LD32(c->r[1] + 12);
    c->lr = c->r[0];
    return 1;
}

static uint32_t g_bp[16];
static uint32_t g_bp_mem;
static int g_nbp = -1;

int g_bp_any;
static void check_bp(PPCContext *c, uint32_t target);
void rt_trace_bp(PPCContext *c, uint32_t pc) { check_bp(c, pc); }
void rt_bp_init(void) { check_bp(&g_ctx, 0xffffffffu); g_bp_any = g_nbp > 0; }

static void check_bp(PPCContext *c, uint32_t target) {
    if (g_nbp < 0) {
        g_nbp = 0;
        const char *e = getenv("RT_BP"), *m = getenv("RT_BP_MEM");
        if (m) g_bp_mem = (uint32_t)strtoul(m, NULL, 16);
        while (e && *e && g_nbp < 16) {
            g_bp[g_nbp++] = (uint32_t)strtoul(e, (char **)&e, 16);
            while (*e == ',') e++;
        }
    }
    for (int i = 0; i < g_nbp; i++)
        if (g_bp[i] == target && getenv("RT_BP_STOP")) {
            rt_log("BPHIT %x -> stop\n", target);
            rt_fatal("breakpoint");
        } else if (g_bp[i] == target)
            rt_log("BPHIT %x r0=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x lr=%08x r1=%08x m=%08x\n", target, c->r[0], c->r[3],
                   c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->lr, c->r[1], LD32(g_bp_mem));
}

void rt_call(PPCContext *c, uint32_t target) {
    if (UNLIKELY(g_nbp) && !g_bp_any) check_bp(c, target);
    RtFn fn = rt_lookup(target);
    if (UNLIKELY(!fn)) {
        if (run_kernel_stub(c, target)) return;
        if (target >= 0xfff00000u) {
            rt_log("FATAL: jump into BIOS %08x (lr=%08x srr0=%08x srr1=%08x)\n", target, c->lr, c->srr0, c->srr1);
            rt_fatal("bios");
        }
        dispatch_miss(c, target);
        return;
    }
    fn(c);
}

/* ============================================================ fibers */
#define MAX_PEND 32
typedef struct Fiber {
    int id;
    pthread_t th;
    pthread_cond_t cv;
    int running;             /* baton */
    int free;                /* parked with no resume keys: may be recycled */
    uint32_t start_pc;
    int restart;             /* resumed to start fresh at start_pc */
    uint32_t pend_pc[MAX_PEND], pend_sp[MAX_PEND], pend_lr[MAX_PEND];
    uint8_t pend_sc[MAX_PEND];   /* syscall: may also be resumed at LR (return from stub) */
    int npend;
    int resume_lr;               /* set by the resumer: continue by returning to LR */
    int unwind_to;           /* pend index to stop unwinding at, -1 = whole fiber */
    struct Fiber *next;
} Fiber;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Fiber *g_fibers, *g_cur;
static int g_nfibers;

static void apply_rounding(uint32_t fpscr);

static void baton_to(Fiber *to) {
    /* called with g_lock held by the running fiber `self` */
    Fiber *self = g_cur;
    self->running = 0;
    to->running = 1;
    g_cur = to;
    pthread_cond_signal(&to->cv);
    while (!self->running)
        pthread_cond_wait(&self->cv, &g_lock);
    g_cur = self;
    apply_rounding(g_ctx.fpscr);
}

static void *fiber_main(void *arg) {
    Fiber *f = (Fiber *)arg;
    PPCContext *c = &g_ctx;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!f->running)
            pthread_cond_wait(&f->cv, &g_lock);
        g_cur = f;
        f->restart = 0;
        f->free = 0;
        f->npend = 0;
        c->unwind = 0;
        uint32_t pc = f->start_pc;
        apply_rounding(c->fpscr);
        rt_call(c, pc);             /* the running fiber always holds g_lock (the baton) */
        if (c->unwind && f->unwind_to == -1) {
            c->unwind = 0;          /* recycled: loop and start at the new pc */
            continue;
        }
        rt_log("fiber %d: task function %08x returned (lr=%08x) - parking\n", f->id, pc, c->lr);
        rt_fatal("task returned");
    }
    return NULL;
}

static Fiber *fiber_new(void) {
    Fiber *f = (Fiber *)calloc(1, sizeof *f);
    f->id = g_nfibers++;
    pthread_cond_init(&f->cv, NULL);
    f->next = g_fibers;
    g_fibers = f;
    return f;
}

static Fiber *fiber_spawn(uint32_t pc) {
    for (Fiber *f = g_fibers; f; f = f->next) {
        if (f->free && f != g_cur) {
            f->start_pc = pc;
            f->restart = 1;
            f->free = 0;
            return f;
        }
    }
    Fiber *f = fiber_new();
    f->start_pc = pc;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    pthread_create(&f->th, &at, fiber_main, f);
    pthread_attr_destroy(&at);
    if (rt_verbose()) rt_log("fiber %d created for pc %08x\n", f->id, pc);
    return f;
}

/* Start guest execution at `pc` on a fiber; the calling (host main) thread returns
 * immediately and stays free for the frontend. */
void rt_start(uint32_t pc) {
    pthread_mutex_lock(&g_lock);
    Fiber *f = fiber_spawn(pc);
    f->running = 1;
    g_cur = f;
    pthread_cond_signal(&f->cv);
    pthread_mutex_unlock(&g_lock);
}

/* ============================================================ exceptions */
enum { MSR_EE = 0x8000, MSR_PR = 0x4000, MSR_FP = 0x2000, MSR_ME = 0x1000, MSR_FE0 = 0x800,
       MSR_SE = 0x400, MSR_BE = 0x200, MSR_FE1 = 0x100, MSR_IP = 0x40, MSR_IR = 0x20,
       MSR_DR = 0x10, MSR_RI = 0x2, MSR_LE = 0x1 };

static uint64_t g_exc_count[0x20];

static int take_exception(PPCContext *c, uint32_t vector, uint32_t resume_pc, int is_sc) {
    Fiber *f = g_cur;
    if (f->npend >= MAX_PEND) rt_fatal("exception nesting too deep");
    int idx = f->npend++;
    int via_lr = 0;
    f->pend_pc[idx] = resume_pc;
    f->pend_sp[idx] = c->r[1];
    f->pend_lr[idx] = c->lr;
    f->pend_sc[idx] = (uint8_t)is_sc;
    g_exc_count[(vector >> 8) & 0x1f]++;
    if (rt_verbose()) rt_log("exception %04x at %08x sp=%08x (fiber %d)\n", vector, resume_pc, c->r[1], f->id);

    c->srr0 = resume_pc;
    c->srr1 = c->msr & 0x87c0ffffu;
    c->msr &= ~(uint32_t)(MSR_EE | MSR_PR | MSR_FP | MSR_FE0 | MSR_SE | MSR_BE | MSR_FE1 | MSR_IR | MSR_DR | MSR_RI);
    uint32_t base = (c->msr & MSR_IP) ? 0xfff00000u : 0;

    rt_call(c, base | vector);

    if (c->unwind) {
        if (f->unwind_to == idx) {
            c->unwind = 0;           /* rfi landed here: continue at resume_pc (or LR) */
            via_lr = f->resume_lr;
            f->resume_lr = 0;
        } else if (f->unwind_to > idx) {
            rt_fatal("unwind target below current frame");
        }
        /* else: keep unwinding towards an outer checkpoint / fiber restart */
    } else {
        rt_log("exception handler %08x returned without rfi (resume %08x)\n", vector, resume_pc);
        rt_fatal("handler returned");
    }
    f->npend = idx;
    return via_lr;
}

static int find_key(Fiber *f, uint32_t pc, uint32_t sp) {
    for (int k = f->npend - 1; k >= 0; k--) {
        if (f->pend_sp[k] != sp) continue;
        if (f->pend_pc[k] == pc) { f->resume_lr = 0; return k; }
        if (f->pend_sc[k] && f->pend_lr[k] == pc) { f->resume_lr = 1; return k; }
    }
    return -1;
}

void rt_rfi(PPCContext *c) {
    uint32_t pc = c->srr0, sp = c->r[1];
    c->msr = (c->msr & ~0x87c0ffffu) | (c->srr1 & 0x87c0ffffu);
    Fiber *self = g_cur;
    if (rt_verbose()) rt_log("rfi -> %08x sp=%08x (fiber %d, npend=%d)\n", pc, sp, self->id, self->npend);

    int k = find_key(self, pc, sp);
    if (k >= 0) {
        self->unwind_to = k;
        c->unwind = 1;
        return;
    }
    Fiber *to = NULL;
    for (Fiber *f = g_fibers; f && !to; f = f->next) {
        if (f == self || f->free) continue;
        k = find_key(f, pc, sp);
        if (k >= 0) { f->unwind_to = k; to = f; }
    }
    if (!to) to = fiber_spawn(pc);
    if (self->npend == 0) self->free = 1;   /* nobody can ever resume us: recyclable */
    baton_to(to);
    /* resumed: either an rfi targeted one of our checkpoints, or we were recycled */
    if (self->restart) {
        self->unwind_to = -1;
    }
    c->unwind = 1;
}

int rt_sc(PPCContext *c, uint32_t next_pc) {
    static int sclog = -1;
    if (sclog < 0) sclog = getenv("RT_SC_LOG") != NULL;
    if (sclog) rt_log("SC r5=%x r3=%08x r4=%08x lr=%08x\n", c->r[5], c->r[3], c->r[4], c->lr);
    return take_exception(c, 0xc00, next_pc, 1);
}

/* ============================================================ time */
static int64_t g_slice;                     /* budget handed out at last sync */
#define MAX_SLICE 20000

static void sync_time(PPCContext *c) {
    c->cycles += (uint64_t)(g_slice - c->budget);
    g_slice = c->budget = 0;
}

static void rearm(PPCContext *c) {
    uint64_t next = rt_sched_next();
    int64_t d = (int64_t)(next - c->cycles);
    if (d > MAX_SLICE) d = MAX_SLICE;
    if (d < 1) d = 1;
    g_slice = c->budget = d;
}

void rt_eat_cycles(uint32_t n) { g_ctx.budget -= n; }

uint64_t rt_now(void) { return g_ctx.cycles + (uint64_t)(g_slice - g_ctx.budget); }

/* Idle loop skipping (profile hint "idle", recomp/recomp.py). The game's idle task polls a table
 * in RAM that only interrupts and other tasks change, and those run only at a time checkpoint:
 * until the slice ends, every pass of the loop repeats the previous one. This is called at the
 * end of each pass. If the pass that just ended found nothing to do (no "work" address reached:
 * the generated code clears `clean` there) and no checkpoint happened during it, the budget it
 * took is the cost P of a pass, and k whole passes are charged at once, always leaving more than
 * one to run. The last passes run for real and end the slice at the same instruction, cycle and
 * register state as without skipping, so the game runs the same; the host only runs fewer passes.
 * RT_IDLE=0 turns it off (to compare). */
void rt_idle(PPCContext *c, RtIdle *s) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RT_IDLE"); on = !e || atoi(e) != 0; }
    if (on && s->clean && s->cycles == c->cycles && s->slice == g_slice) {
        int64_t p = s->budget - c->budget;
        if (p > 0) {
            int64_t k = (c->budget - 1) / p - 1;   /* leaves a budget in (P, 2P]: no check fires early */
            if (k > 0) c->budget -= k * p;
        }
    }
    s->budget = c->budget;
    s->slice = g_slice;
    s->cycles = c->cycles;
    s->clean = 1;
}

void rt_shorten_slice(uint64_t until) {
    int64_t d = (int64_t)(until - rt_now());
    if (d < 1) d = 1;
    if (d < g_ctx.budget) {
        g_slice -= g_ctx.budget - d;     /* keeps rt_now() unchanged */
        g_ctx.budget = d;
    }
}

static int dec_pending;

static void dec_fire(void *arg) {
    (void)arg;
    dec_pending = 1;
}

static void deliver(PPCContext *c, uint32_t pc) {
    for (int guard = 0; guard < 4 && (c->msr & MSR_EE); guard++) {
        if (rt_irq_line()) {
            take_exception(c, 0x500, pc, 0);
        } else if (dec_pending) {
            dec_pending = 0;
            take_exception(c, 0x900, pc, 0);
        } else {
            break;
        }
        if (c->unwind) return;
    }
}

/* virtual-time profiler (RT_PROFILE=start_seconds): cycles attributed to checkpoint PCs */
#define PROF_SIZE 8192
static uint32_t g_prof_pc[PROF_SIZE];
static uint64_t g_prof_cyc[PROF_SIZE];
static double g_prof_start = -2;

static void prof_add(uint32_t pc, uint64_t cyc) {
    unsigned h = (pc * 2654435761u) >> 19;
    for (unsigned k = 0; k < PROF_SIZE; k++, h = (h + 1) & (PROF_SIZE - 1)) {
        if (g_prof_pc[h] == pc || g_prof_cyc[h] == 0) { g_prof_pc[h] = pc; g_prof_cyc[h] += cyc; return; }
    }
}

void rt_profile_dump(void) {
    if (g_prof_start < 0) return;
    uint64_t tot = 0;
    for (int i = 0; i < PROF_SIZE; i++) tot += g_prof_cyc[i];
    for (int n = 0; n < 40; n++) {
        int best = -1;
        for (int i = 0; i < PROF_SIZE; i++) if (g_prof_cyc[i] && (best < 0 || g_prof_cyc[i] > g_prof_cyc[best])) best = i;
        if (best < 0) break;
        rt_log("  prof %08x %6.2f%%\n", g_prof_pc[best], 100.0 * (double)g_prof_cyc[best] / (double)tot);
        g_prof_cyc[best] = 0;
    }
}

void rt_check(PPCContext *c, uint32_t pc) {
    if (UNLIKELY(g_prof_start != -1)) {
        if (g_prof_start == -2) g_prof_start = getenv("RT_PROFILE") ? atof(getenv("RT_PROFILE")) : -1;
        if (g_prof_start >= 0 && (double)c->cycles / CPU_HZ >= g_prof_start) prof_add(pc, (uint64_t)(g_slice - c->budget));
    }
    sync_time(c);
    rt_sched_run(c->cycles);
    rearm(c);
    deliver(c, pc);
}

void rt_mtmsr(PPCContext *c, uint32_t v, uint32_t next_pc) {
    uint32_t old = c->msr;
    c->msr = v;
    if ((v & MSR_EE) && !(old & MSR_EE)) {
        sync_time(c);
        rt_sched_run(c->cycles);
        rearm(c);
        deliver(c, next_pc);
    }
}

/* timebase: bus clock / 4 ; bus = 2 x 33.8688 MHz, core = 6 x 33.8688 MHz */
#define CYCLES_PER_TB 12
uint64_t rt_timebase(void) { return g_ctx.tb_base + rt_now() / CYCLES_PER_TB; }

uint32_t rt_mftb(PPCContext *c, int tbr) {
    (void)c;
    uint64_t tb = rt_timebase();
    return tbr == 269 ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

static uint32_t dec_value(PPCContext *c) {
    int64_t left = (int64_t)(c->dec_event - rt_now()) / CYCLES_PER_TB;
    return (uint32_t)(int32_t)left;
}

static void dec_write(PPCContext *c, uint32_t v) {
    c->dec_event = rt_now() + (uint64_t)((int64_t)(int32_t)v + 1) * CYCLES_PER_TB;
    rt_sched_cancel(dec_fire, NULL);
    if ((int32_t)v >= 0) rt_sched_at(c->dec_event, dec_fire, NULL);
    dec_pending = 0;
}

/* ============================================================ SPRs */
uint32_t rt_mfspr(PPCContext *c, int spr) {
    switch (spr) {
    case 22: return dec_value(c);
    case 268: return rt_mftb(c, 268);
    case 269: return rt_mftb(c, 269);
    case 287: return 0x00810101;       /* PVR: MPC8240 (603e core) */
    default: return c->spr[spr & 1023];
    }
}

void rt_mtspr(PPCContext *c, int spr, uint32_t v) {
    switch (spr) {
    case 22: dec_write(c, v); return;
    case 284: c->tb_base = (c->tb_base & ~0xffffffffull) | v; return;               /* TBL */
    case 285: c->tb_base = (c->tb_base & 0xffffffffull) | ((uint64_t)v << 32); return;  /* TBU */
    default: c->spr[spr & 1023] = v; return;
    }
}

uint32_t rt_cr_pack(PPCContext *c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint32_t)(c->cr[i] & 15) << (28 - 4 * i);
    return v;
}

void rt_cr_unpack(PPCContext *c, uint32_t v, uint32_t crm) {
    for (int i = 0; i < 8; i++)
        if (crm & (0x80u >> i)) c->cr[i] = (v >> (28 - 4 * i)) & 15;
}

uint32_t rt_xer_pack(PPCContext *c) {
    return ((uint32_t)c->xer_so << 31) | ((uint32_t)c->xer_ov << 30) | ((uint32_t)c->xer_ca << 29) | c->xer_bc;
}

void rt_xer_unpack(PPCContext *c, uint32_t v) {
    c->xer_so = (v >> 31) & 1; c->xer_ov = (v >> 30) & 1; c->xer_ca = (v >> 29) & 1; c->xer_bc = v & 0x7f;
}

/* ============================================================ ALU */
uint32_t rt_divw(PPCContext *c, uint32_t a, uint32_t b, int oe) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    int ovf = (sb == 0) || (a == 0x80000000u && sb == -1);
    if (oe) { c->xer_ov = (uint8_t)ovf; c->xer_so |= (uint8_t)ovf; }
    if (ovf) return (sa < 0 && sb == 0) ? 0xffffffffu : 0;
    return (uint32_t)(sa / sb);
}

uint32_t rt_divwu(PPCContext *c, uint32_t a, uint32_t b, int oe) {
    if (oe) { c->xer_ov = (b == 0); c->xer_so |= c->xer_ov; }
    return b ? a / b : 0;
}

uint32_t rt_sraw(PPCContext *c, uint32_t v, uint32_t n) {
    int32_t s = (int32_t)v;
    if (n >= 32) { c->xer_ca = s < 0; return (uint32_t)(s >> 31); }
    if (n == 0) { c->xer_ca = 0; return v; }
    c->xer_ca = (s < 0) && (v & ((1u << n) - 1));
    return (uint32_t)(s >> n);
}

void rt_dcbz(PPCContext *c, uint32_t ea) {
    (void)c;
    ea &= ~31u;
    if (ea < RAM_LIMIT) memset(g_ram + (ea & RAM_MASK), 0, 32);
    else for (int i = 0; i < 32; i += 4) ST32(ea + i, 0);
}

void rt_lswi(PPCContext *c, uint32_t ea, int rd, int nb) {
    int r = (rd - 1) & 31;
    for (int n = 0; n < nb; n++) {
        if ((n & 3) == 0) { r = (r + 1) & 31; c->r[r] = 0; }
        c->r[r] |= LD8(ea + n) << (24 - 8 * (n & 3));
    }
}

void rt_stswi(PPCContext *c, uint32_t ea, int rs, int nb) {
    int r = (rs - 1) & 31;
    for (int n = 0; n < nb; n++) {
        if ((n & 3) == 0) r = (r + 1) & 31;
        ST8(ea + n, (c->r[r] >> (24 - 8 * (n & 3))) & 0xff);
    }
}

/* ============================================================ FPU */
double rt_fctiw(PPCContext *c, double v, int trunc) {
    int32_t r;
    if (v != v) r = (int32_t)0x80000000;
    else if (v >= 2147483647.0) r = 0x7fffffff;
    else if (v <= -2147483648.0) r = (int32_t)0x80000000;
    else if (trunc) r = (int32_t)v;
    else {
        switch (c->fpscr & 3) {
        case 0: r = (int32_t)nearbyint(v); break;   /* host in round-to-nearest */
        case 1: r = (int32_t)v; break;
        case 2: r = (int32_t)ceil(v); break;
        default: r = (int32_t)floor(v); break;
        }
    }
    return BITS_FPR(0xfff8000000000000ull | (uint32_t)r);
}

void rt_mtfsf(PPCContext *c, uint32_t fm, uint32_t v) {
    uint32_t mask = 0;
    for (int i = 0; i < 8; i++)
        if (fm & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
    c->fpscr = (c->fpscr & ~mask) | (v & mask);
    rt_fpscr_changed(c);
}

/* PPC FPSCR[RN] -> host rounding mode (per host thread, re-applied on fiber switches) */
static void apply_rounding(uint32_t fpscr) {
    static const int modes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    fesetround(modes[fpscr & 3]);
}

void rt_fpscr_changed(PPCContext *c) { apply_rounding(c->fpscr); }

/* ============================================================ faults */
void rt_trap(PPCContext *c, uint32_t pc) {
    rt_log("FATAL: trap at %08x (lr=%08x r3=%08x)\n", pc, c->lr, c->r[3]);
    rt_fatal("trap");
}

void rt_bad_insn(PPCContext *c, uint32_t pc, uint32_t word) {
    rt_log("FATAL: undecodable instruction %08x at %08x (lr=%08x)\n", word, pc, c->lr);
    rt_fatal("bad insn");
}

void rt_unimpl(PPCContext *c, uint32_t pc, uint32_t word) {
    rt_log("FATAL: unimplemented instruction %08x at %08x (lr=%08x)\n", word, pc, c->lr);
    rt_fatal("unimpl");
}

void rt_fallthrough(PPCContext *c, uint32_t pc) {
    rt_call(c, pc);   /* fell off analysed code: continue through dispatch */
}

#ifdef RT_TRACE
uint32_t g_trace_pc[65536], g_trace_sp[65536];
unsigned g_trace_pos;
#endif

void rt_profile_dump(void);
void rt_dump_state(void) {
    rt_profile_dump();
    PPCContext *c = &g_ctx;
#ifdef RT_TRACE
    rt_log("  last blocks (oldest first):\n");
    unsigned depth = getenv("RT_TRACE_DEPTH") ? (unsigned)atoi(getenv("RT_TRACE_DEPTH")) : 48;
    if (depth > 65535) depth = 65535;
    uint32_t splo = getenv("RT_TRACE_SPLO") ? (uint32_t)strtoul(getenv("RT_TRACE_SPLO"), NULL, 16) : 0;
    uint32_t sphi = getenv("RT_TRACE_SPHI") ? (uint32_t)strtoul(getenv("RT_TRACE_SPHI"), NULL, 16) : 0xffffffffu;
    for (unsigned i = depth; i > 0; i--) {
        unsigned p = (g_trace_pos - i) & 65535;
        if (g_trace_sp[p] < splo || g_trace_sp[p] > sphi) continue;
        rt_log("    %08x sp=%08x\n", g_trace_pc[p], g_trace_sp[p]);
    }
#endif
    rt_log("  cycles=%llu msr=%08x lr=%08x ctr=%08x srr0=%08x srr1=%08x cr=%08x\n",
           (unsigned long long)rt_now(), c->msr, c->lr, c->ctr, c->srr0, c->srr1, rt_cr_pack(c));
    for (int i = 0; i < 32; i += 8)
        rt_log("  r%-2d %08x %08x %08x %08x %08x %08x %08x %08x\n", i, c->r[i], c->r[i + 1], c->r[i + 2],
               c->r[i + 3], c->r[i + 4], c->r[i + 5], c->r[i + 6], c->r[i + 7]);
    rt_log("  exceptions: ext=%llu dec=%llu sc=%llu  fibers=%d\n", (unsigned long long)g_exc_count[5],
           (unsigned long long)g_exc_count[9], (unsigned long long)g_exc_count[0xc], g_nfibers);
}
