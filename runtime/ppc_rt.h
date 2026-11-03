/*
 * Runtime ABI for statically recompiled Konami Viper (MPC8240 / PPC603e) code.
 *
 * Guest RAM is kept in guest (big-endian) byte order; accessors byte-swap.
 * All guest state lives in PPCContext; generated code never caches registers
 * across calls, so one context can be shared by every guest task/fiber.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct PPCContext {
    uint32_t r[32];
    double f[32];
    uint8_t cr[8];          /* 4-bit fields: LT=8 GT=4 EQ=2 SO=1 */
    uint32_t lr, ctr;
    uint8_t xer_so, xer_ov, xer_ca, xer_bc;
    uint32_t msr, srr0, srr1;
    uint32_t sprg[4];
    uint32_t sr[16];
    uint32_t fpscr;
    uint32_t reserve;
    uint32_t spr[1024];     /* everything not special-cased */

    /* runtime bookkeeping */
    int64_t budget;         /* cycles left before the next device event */
    uint64_t cycles;        /* virtual CPU cycles elapsed (at last sync) */
    uint64_t tb_base;       /* timebase offset (written by mttbl/mttbu) */
    uint64_t dec_event;     /* cycle at which DEC crosses zero */
    int unwind;             /* non-zero while unwinding host frames after rfi */
} PPCContext;

typedef void (*RtFn)(PPCContext *c);

typedef struct RtFunc {
    uint32_t addr;
    RtFn fn;
    uint32_t first_word;    /* used to tell overlays at the same address apart */
} RtFunc;

typedef struct RtModuleInfo {
    const char *name;
    uint32_t base, end, text_lo, text_hi;
    const RtFunc *funcs;
    unsigned nfuncs;
    uint32_t sig[8];        /* first words of the image: identifies which overlay is resident */
} RtModuleInfo;

/* ------------------------------------------------------------------ memory */
#define RAM_SIZE 0x01000000u
#define RAM_MASK 0x00ffffffu
#define RAM_LIMIT 0x02000000u   /* 16MB mirrored once */

extern uint8_t *g_ram;

uint32_t rt_mmio_r32(uint32_t ea);
uint32_t rt_mmio_r16(uint32_t ea);
uint32_t rt_mmio_r8(uint32_t ea);
void rt_mmio_w32(uint32_t ea, uint32_t v);
void rt_mmio_w16(uint32_t ea, uint32_t v);
void rt_mmio_w8(uint32_t ea, uint32_t v);

static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }

static inline uint32_t LD32(uint32_t ea) {
    if (LIKELY(ea < RAM_LIMIT)) { uint32_t v; memcpy(&v, g_ram + (ea & RAM_MASK), 4); return bswap32(v); }
    return rt_mmio_r32(ea);
}
static inline uint32_t LD16(uint32_t ea) {
    if (LIKELY(ea < RAM_LIMIT)) { uint16_t v; memcpy(&v, g_ram + (ea & RAM_MASK), 2); return bswap16(v); }
    return rt_mmio_r16(ea);
}
static inline uint32_t LD8(uint32_t ea) {
    if (LIKELY(ea < RAM_LIMIT)) return g_ram[ea & RAM_MASK];
    return rt_mmio_r8(ea);
}
static inline void ST32(uint32_t ea, uint32_t v) {
    if (LIKELY(ea < RAM_LIMIT)) { v = bswap32(v); memcpy(g_ram + (ea & RAM_MASK), &v, 4); return; }
    rt_mmio_w32(ea, v);
}
static inline void ST16(uint32_t ea, uint32_t v) {
    if (LIKELY(ea < RAM_LIMIT)) { uint16_t s = bswap16((uint16_t)v); memcpy(g_ram + (ea & RAM_MASK), &s, 2); return; }
    rt_mmio_w16(ea, v & 0xffff);
}
static inline void ST8(uint32_t ea, uint32_t v) {
    if (LIKELY(ea < RAM_LIMIT)) { g_ram[ea & RAM_MASK] = (uint8_t)v; return; }
    rt_mmio_w8(ea, v & 0xff);
}
static inline uint32_t LD32LE(uint32_t ea) { return bswap32(LD32(ea)); }
static inline uint32_t LD16LE(uint32_t ea) { return bswap16((uint16_t)LD16(ea)); }
static inline void ST32LE(uint32_t ea, uint32_t v) { ST32(ea, bswap32(v)); }
static inline void ST16LE(uint32_t ea, uint32_t v) { ST16(ea, bswap16((uint16_t)v)); }

static inline double LDF32(uint32_t ea) { uint32_t b = LD32(ea); float f; memcpy(&f, &b, 4); return (double)f; }
static inline void STF32(uint32_t ea, double d) { float f = (float)d; uint32_t b; memcpy(&b, &f, 4); ST32(ea, b); }
static inline double LDF64(uint32_t ea) {
    uint64_t b = ((uint64_t)LD32(ea) << 32) | LD32(ea + 4); double d; memcpy(&d, &b, 8); return d;
}
static inline void STF64(uint32_t ea, double d) {
    uint64_t b; memcpy(&b, &d, 8); ST32(ea, (uint32_t)(b >> 32)); ST32(ea + 4, (uint32_t)b);
}
static inline uint64_t FPR_BITS(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static inline double BITS_FPR(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

/* ------------------------------------------------------------------ ALU helpers */
static inline uint32_t ROTL(uint32_t v, uint32_t s) { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }
static inline uint32_t CLZ32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32u; }
#define CMPS(a, b) ((uint8_t)((a) < (b) ? 8 : (a) > (b) ? 4 : 2))
#define CMPU(a, b) ((uint8_t)((a) < (b) ? 8 : (a) > (b) ? 4 : 2))
static inline uint8_t CMPF(double a, double b) {
    if (a < b) return 8;
    if (a > b) return 4;
    if (a == b) return 2;
    return 1; /* unordered */
}
#define ROUND_S(x) ((double)(float)(x))

/* ------------------------------------------------------------------ runtime services */
void rt_check(PPCContext *c, uint32_t pc);          /* time/irq checkpoint */
#define CHK(c, pc, n) do { if (UNLIKELY(((c)->budget -= (n)) <= 0)) { rt_check((c), (pc)); if ((c)->unwind) return; } } while (0)
#define RETURN(c) return

#ifdef RT_TRACE
extern uint32_t g_trace_pc[65536], g_trace_sp[65536];
extern unsigned g_trace_pos;
extern int g_bp_any;
void rt_trace_bp(PPCContext *c, uint32_t pc);
#define TRACE(c, pc) do { unsigned _p = g_trace_pos++ & 65535; g_trace_pc[_p] = (pc); g_trace_sp[_p] = (c)->r[1]; \
                          if (UNLIKELY(g_bp_any)) rt_trace_bp((c), (pc)); } while (0)
#else
#define TRACE(c, pc) do { } while (0)
#endif

void rt_call(PPCContext *c, uint32_t target);       /* indirect call through dispatch */
void rt_hook(PPCContext *c, uint32_t pc);           /* profile hooks (runtime/enhanced.c) */
/* idle loops (profile "idle" hints): the end of a pass of the game's polling loop, where whole
 * passes that would find nothing to do are charged without running them (runtime/cpu.c) */
typedef struct RtIdle { int64_t budget, slice; uint64_t cycles; int clean; } RtIdle;
void rt_idle(PPCContext *c, RtIdle *s);
int rt_sc(PPCContext *c, uint32_t next_pc);   /* 1: resumed at LR -> return */
void rt_rfi(PPCContext *c);
void rt_mtmsr(PPCContext *c, uint32_t v, uint32_t next_pc);
void rt_trap(PPCContext *c, uint32_t pc);
void rt_bad_insn(PPCContext *c, uint32_t pc, uint32_t word);
void rt_unimpl(PPCContext *c, uint32_t pc, uint32_t word);
void rt_fallthrough(PPCContext *c, uint32_t pc);

uint32_t rt_mfspr(PPCContext *c, int spr);
void rt_mtspr(PPCContext *c, int spr, uint32_t v);
uint32_t rt_mftb(PPCContext *c, int tbr);
uint32_t rt_cr_pack(PPCContext *c);
void rt_cr_unpack(PPCContext *c, uint32_t v, uint32_t crm);
uint32_t rt_xer_pack(PPCContext *c);
void rt_xer_unpack(PPCContext *c, uint32_t v);
uint32_t rt_divw(PPCContext *c, uint32_t a, uint32_t b, int oe);
uint32_t rt_divwu(PPCContext *c, uint32_t a, uint32_t b, int oe);
uint32_t rt_sraw(PPCContext *c, uint32_t v, uint32_t n);
void rt_dcbz(PPCContext *c, uint32_t ea);
void rt_lswi(PPCContext *c, uint32_t ea, int rd, int nb);
void rt_stswi(PPCContext *c, uint32_t ea, int rs, int nb);
double rt_fctiw(PPCContext *c, double v, int trunc);
void rt_mtfsf(PPCContext *c, uint32_t fm, uint32_t v);
void rt_fpscr_changed(PPCContext *c);

#ifdef __cplusplus
}
#endif
