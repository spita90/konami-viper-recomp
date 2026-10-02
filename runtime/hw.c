/*
 * Konami Viper board devices (behaviour follows MAME src/mame/konami/viper.cpp).
 *
 * Bus conventions: hw_read/hw_write receive big-endian bus values of 1/2/4 bytes at
 * byte address ea.  MAME's 64-bit handlers with ACCESSING_BITS_16_31 correspond to a
 * 16-bit lane at byte offset +4 of each 8-byte group.
 */
#include "runtime.h"
#include "game_config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PCI_CLOCK_HZ 33868800.0
#define SDRAM_HZ (PCI_CLOCK_HZ * 3)
#define TIMER_HZ (SDRAM_HZ / 8)
#define CYC_PER_TIMER_TICK (CPU_HZ / TIMER_HZ)      /* = 16 */

static const HwConfig *g_cfg;
static uint8_t *g_bios;

static void unmapped(const char *what, uint32_t ea, int size, uint32_t v, int wr) {
    static uint32_t seen[256];
    static int nseen, total;
    for (int i = 0; i < nseen; i++) if (seen[i] == (ea & ~0xfffu)) return;
    if (nseen < 256) seen[nseen++] = ea & ~0xfffu;
    if (++total > 64) return;
#ifdef RT_TRACE
    uint32_t blk = g_trace_pc[(g_trace_pos - 1) & 65535];
#else
    uint32_t blk = 0;
#endif
    if (wr) rt_log("[%s] unmapped write%d %08x = %08x (lr=%08x blk=%08x r1=%08x)\n", what, size * 8, ea, v, g_ctx.lr, blk, g_ctx.r[1]);
    else rt_log("[%s] unmapped read%d %08x (lr=%08x blk=%08x r1=%08x)\n", what, size * 8, ea, g_ctx.lr, blk, g_ctx.r[1]);
}

/* ================================================================== EPIC */
typedef struct { uint32_t vector; int priority, mask, active, pending; } EpicIrq;
static struct {
    EpicIrq irq[EPIC_NUM];
    int active_irq;
    uint32_t iack, svr, eicr, pctpr;
    uint32_t gt_base[4];
    int gt_enable[4];
    uint32_t raw[0x40000 / 4];     /* backing store for unimplemented registers */
    int line;
} ep;

static uint64_t g_irq_raised[EPIC_NUM], g_irq_taken[EPIC_NUM];

static void epic_update(void) {
    if (ep.active_irq >= 0) return;   /* wait until the current irq is EOI'd */
    int best = -1, prio = -1;
    for (int i = EPIC_NUM - 1; i >= 0; i--) {
        EpicIrq *q = &ep.irq[i];
        if (q->pending && !q->mask && q->priority > (int)ep.pctpr && q->priority > prio) {
            best = i;
            prio = q->priority;
        }
    }
    if (best >= 0) {
        ep.active_irq = best;
        g_irq_taken[best]++;
        ep.irq[best].pending = 0;
        ep.irq[best].active = 1;
        ep.iack = ep.irq[best].vector;
        ep.line = 1;
    } else {
        ep.line = 0;
    }
}


void epic_raise(int irq) {
    g_irq_raised[irq]++;
    ep.irq[irq].pending = 1;
    epic_update();
}

int rt_irq_line(void) { return ep.line; }

static void gt_fire(void *arg);

static void gt_arm(int n) {
    rt_sched_cancel(gt_fire, (void *)(intptr_t)n);
    if (ep.gt_enable[n] && ep.gt_base[n])
        rt_sched_at(rt_now() + (uint64_t)(ep.gt_base[n] * CYC_PER_TIMER_TICK), gt_fire, (void *)(intptr_t)n);
}

static void gt_fire(void *arg) {
    int n = (int)(intptr_t)arg;
    gt_arm(n);
    epic_raise(EPIC_GT0 + n);
}

static uint32_t vpr_get(EpicIrq *q) {
    return (q->mask ? 0x80000000u : 0) | (q->active ? 0x40000000u : 0) | ((uint32_t)q->priority << 16) | q->vector;
}

static void vpr_set(EpicIrq *q, uint32_t v) {
    q->mask = (v >> 31) & 1;
    q->priority = (v >> 16) & 15;
    q->vector = v & 0xff;
    epic_update();
}

/* EPIC register (little-endian value) access, off relative to EUMB base */
static uint32_t epic_reg_read(uint32_t off) {
    if (off == 0x41080) return 0x00010000;                      /* EVI: step 1 */
    if (off == 0x41000) return 0x00170002;                      /* FRR: 24 sources */
    if (off == 0x410e0) return ep.svr;
    if (off >= 0x41110 && off < 0x41210 && ((off - 0x41110) & 0x3f) == 0x00)
        return (ep.gt_enable[(off - 0x41110) >> 6] ? 0 : 0x80000000u) | ep.gt_base[(off - 0x41110) >> 6];
    if (off >= 0x41120 && off < 0x41220 && ((off - 0x41120) & 0x3f) == 0x00)
        return vpr_get(&ep.irq[EPIC_GT0 + ((off - 0x41120) >> 6)]);
    if (off >= 0x50200 && off < 0x50400 && ((off - 0x50200) & 0x1f) == 0)
        return vpr_get(&ep.irq[(off - 0x50200) >> 5]);
    if (off == 0x51020) return vpr_get(&ep.irq[EPIC_I2C]);
    if (off == 0x60080) return ep.pctpr;
    if (off == 0x600a0) {                                       /* IACK */
        epic_update();
        return ep.active_irq >= 0 ? ep.iack : ep.svr;
    }
    if (off >= 0x40000 && off < 0x80000) return ep.raw[(off - 0x40000) >> 2];
    return 0;
}

static void epic_reg_write(uint32_t off, uint32_t v) {
    if (off >= 0x40000 && off < 0x80000) ep.raw[(off - 0x40000) >> 2] = v;
    if (off == 0x41020) { ep.raw[(off - 0x40000) >> 2] = v & ~0x80000000u; return; }  /* GCR reset completes at once */
    if (off == 0x41030) { ep.eicr = v; if (v & (1u << 27)) rt_log("EPIC: serial interrupt mode requested\n"); return; }
    if (off == 0x410e0) { ep.svr = v & 0xff; return; }
    if (off >= 0x41110 && off < 0x41210 && ((off - 0x41110) & 0x3f) == 0x00) {
        int n = (off - 0x41110) >> 6;
        ep.gt_enable[n] = !(v & 0x80000000u);
        ep.gt_base[n] = v & 0x7fffffff;
        gt_arm(n);
        return;
    }
    if (off >= 0x41120 && off < 0x41220 && ((off - 0x41120) & 0x3f) == 0x00) {
        vpr_set(&ep.irq[EPIC_GT0 + ((off - 0x41120) >> 6)], v);
        return;
    }
    if (off >= 0x50200 && off < 0x50400 && ((off - 0x50200) & 0x1f) == 0) {
        vpr_set(&ep.irq[(off - 0x50200) >> 5], v);
        return;
    }
    if (off == 0x51020) { vpr_set(&ep.irq[EPIC_I2C], v); return; }
    if (off == 0x60080) { ep.pctpr = v & 15; epic_update(); return; }
    if (off == 0x600b0) {                                       /* EOI */
        if (ep.active_irq >= 0) ep.irq[ep.active_irq].active = 0;
        ep.active_irq = -1;
        epic_update();
        return;
    }
}

static void epic_init(void) {
    memset(&ep, 0, sizeof ep);
    for (int i = 0; i < EPIC_NUM; i++) ep.irq[i].mask = 1;
    ep.active_irq = -1;
    ep.pctpr = 15;
}

/* ================================================================== I2C (ADC0838 on the bus) */
static struct { uint8_t adr, fdr, dffsr, cr, sr, addr_latch; int state, rw; } i2c;
enum { I2C_ADDR = 1, I2C_DATA };
/* Analog controls as signed positions, -255..+255 (0 = centre).  The game reads each one from the
 * ADC0838 in differential mode, once per polarity (see i2c_read), and rebuilds 0x100 +/- d. */
int16_t g_analog[4] = GAME_ANALOG_REST;

/* K-type force-feedback wheel motor (0xffe20000, written by the game): bit7 motor on, bit4
 * direction (1 = right), bits3-0 torque. Motor-equipped cabinets (MOTOR TYPE: K-TYPE, e.g. the
 * gticlub2 NVRAM) calibrate the steering by ramping the torque up once per second until the
 * wheel starts to turn; a wheel that never moves ends in "STEERING WHEEL : ERROR".
 * With RT_FFB_WHEEL=1 (set by the first-run calibration) the virtual wheel follows the motor:
 * above a breakaway torque it turns at a speed proportional to the excess torque, up to the end
 * stops. Otherwise the motor is ignored and the frontend alone positions the wheel. */
#define FFB_BREAKAWAY 3          /* lowest torque step that turns the wheel */
#define FFB_SPEED 60.0           /* ADC position units per second per torque step above it */
#define FFB_END_STOP 200         /* matches the frontend's steering range */
static uint8_t g_motor;
static uint64_t g_motor_t;
static double g_ffb_pos;
static int g_ffb_wheel = -1;

static void ffb_update(void) {
    uint64_t now = rt_now();
    double dt = (double)(now - g_motor_t) / CPU_HZ;
    g_motor_t = now;
    if (g_ffb_wheel < 0) g_ffb_wheel = getenv("RT_FFB_WHEEL") != NULL;
    int torque = g_motor & 15;
    if (!g_ffb_wheel || !(g_motor & 0x80) || torque < FFB_BREAKAWAY) { g_ffb_pos = g_analog[0]; return; }
    double v = (torque - FFB_BREAKAWAY + 1) * FFB_SPEED * dt;
    g_ffb_pos += (g_motor & 0x10) ? v : -v;
    if (g_ffb_pos > FFB_END_STOP) g_ffb_pos = FFB_END_STOP;
    if (g_ffb_pos < -FFB_END_STOP) g_ffb_pos = -FFB_END_STOP;
    g_analog[0] = (int16_t)g_ffb_pos;
}

static void motor_write(uint8_t v) { ffb_update(); g_motor = v; }

static void i2c_done(void *arg) {
    (void)arg;
    i2c.sr |= 0x80;
    if (i2c.cr & 0x40) { epic_raise(EPIC_I2C); i2c.sr |= 0x02; }
}

static void i2c_kick(void) {
    rt_sched_cancel(i2c_done, NULL);
    rt_sched_at(rt_now() + (uint64_t)(CPU_HZ / (SDRAM_HZ / 512 / 10)), i2c_done, NULL);
}

static uint8_t i2c_read(uint32_t off) {
    switch (off) {
    case 0x3000: return i2c.adr;
    case 0x3004: return i2c.fdr;
    case 0x3005: return i2c.dffsr;
    case 0x3008: return i2c.cr;
    case 0x300c: return i2c.sr;
    case 0x3010: {
        uint8_t res = 0;
        if (!(i2c.cr & 0x80)) return 0;
        if (i2c.state == I2C_ADDR) { i2c.state = I2C_DATA; i2c_kick(); return 0; }
        i2c.state = I2C_ADDR;
        i2c.sr |= 0x80;
        if (i2c.rw && (i2c.addr_latch & 0xf0) == 0x10) {
            /* the address byte is the ADC0838 mux word: bit3 SGL/DIF, bit2 ODD/SIGN, bits1-0 SELECT */
            if (i2c.addr_latch == 0x1c) return 0x80;          /* single-ended supply monitor: 5.0 V */
            if ((i2c.addr_latch & 3) == 0) ffb_update();
            int pos = g_analog[i2c.addr_latch & 3];
            int d = (i2c.addr_latch & 4) ? -pos : pos;        /* CH+ - CH-, or reversed */
            res = (uint8_t)(d < 0 ? 0 : d > 255 ? 255 : d);
            static int i2clog = -1;
            if (i2clog < 0) i2clog = getenv("RT_I2C_LOG") != NULL;
            if (i2clog) rt_log("i2c read addr %02x -> %02x (lr=%08x)\n", i2c.addr_latch, res, g_ctx.lr);
        }
        return res;
    }
    }
    return 0;
}

static void i2c_write(uint32_t off, uint8_t v) {
    switch (off) {
    case 0x3000: i2c.adr = v; break;
    case 0x3004: i2c.fdr = v & 0x3f; break;
    case 0x3005: i2c.dffsr = v & 0x3f; break;
    case 0x3008:
        if ((!(i2c.cr & 0x80) && (v & 0x80)) || ((i2c.cr ^ v) & 0x10)) i2c.state = I2C_ADDR;
        i2c.cr = v;
        break;
    case 0x300c: i2c.sr = v; break;
    case 0x3010:
        if (!(i2c.cr & 0x80)) break;
        if (i2c.state == I2C_ADDR) {
            i2c.rw = v & 1;
            i2c.addr_latch = (v >> 1) & 0x7f;
            i2c.state = I2C_DATA;
        } else {
            i2c.state = I2C_ADDR;
        }
        i2c_kick();
        break;
    }
}

/* EUMB window (0x80000000-0x800fffff) */
static uint32_t eumb_read(uint32_t off, int size) {
    if (off >= 0x3000 && off < 0x3020) {
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v = (v << 8) | i2c_read(off + i);
        return v;
    }
    if (off >= 0x40000 && off < 0x80000) {
        uint32_t k = off & 3;
        return le_bus_read(epic_reg_read(off & ~3u), (int)k, size);
    }
    unmapped("eumb", 0x80000000u + off, size, 0, 0);
    return 0;
}

static void eumb_write(uint32_t off, int size, uint32_t v) {
    if (off >= 0x3000 && off < 0x3020) {
        for (int i = 0; i < size; i++) i2c_write(off + i, (v >> (8 * (size - 1 - i))) & 0xff);
        return;
    }
    if (off >= 0x40000 && off < 0x80000) {
        uint32_t r = off & ~3u;
        uint32_t old = (r >= 0x40000 && r < 0x80000) ? ep.raw[(r - 0x40000) >> 2] : 0;
        if (size != 4) old = epic_reg_read(r);
        epic_reg_write(r, le_bus_write(old, (int)(off & 3), size, v));
        return;
    }
    unmapped("eumb", 0x80000000u + off, size, v, 1);
}

/* ================================================================== PCI configuration */
static uint32_t pci_addr;
static uint32_t pci_host[64], pci_vd[64];

static uint32_t *pci_dev_regs(int *which) {
    int dev = (pci_addr >> 11) & 31, bus = (pci_addr >> 16) & 0xff;
    if (!(pci_addr & 0x80000000u) || bus) return NULL;
    if (dev == 0) { *which = 0; return pci_host; }
    if (dev == 12) { *which = 12; return pci_vd; }
    return NULL;
}

static uint32_t pci_cfg_read(void) {
    int which;
    uint32_t *r = pci_dev_regs(&which);
    if (!r) return 0xffffffffu;
    return r[(pci_addr & 0xfc) >> 2];
}

static void pci_cfg_write(uint32_t v) {
    int which;
    uint32_t *r = pci_dev_regs(&which);
    if (!r) return;
    int reg = pci_addr & 0xfc;
    if (which == 12) {
        switch (reg) {
        case 0x00: case 0x08: return;                                   /* read-only IDs */
        case 0x10: v = (v == 0xffffffffu) ? 0xfe000000u : (v & 0xfe000000u); break;   /* 32MB regs */
        case 0x14: v = (v == 0xffffffffu) ? 0xfe000008u : ((v & 0xfe000000u) | 8); break; /* 32MB LFB */
        case 0x18: v = (v == 0xffffffffu) ? 0xffffff01u : ((v & 0xffffff00u) | 1); break; /* 256B IO */
        default: break;
        }
        if (reg == 0x10 || reg == 0x14 || reg == 0x18)
            rt_log("PCI: voodoo BAR%d = %08x\n", (reg - 0x10) / 4, v);
    }
    r[reg >> 2] = v;
}

static void pci_init(void) {
    memset(pci_host, 0, sizeof pci_host);
    memset(pci_vd, 0, sizeof pci_vd);
    pci_host[0] = 0x00031057;        /* Motorola MPC8240 */
    pci_host[2] = 0x06000000;
    pci_vd[0] = 0x0005121a;          /* 3dfx Voodoo 3 */
    pci_vd[2] = 0x03000000;
}

/* ================================================================== CF card / ATA */
#define SECTOR 512
static struct {
    FILE *img;
    uint32_t nsect;
    int ide_mode;
    uint8_t feature, count, sector, cyl_lo, cyl_hi, head, status, error, devctl;
    uint8_t buf[SECTOR * 256];
    int buf_pos, buf_len;            /* in bytes */
    int remaining;                   /* sectors left in the current command */
    uint32_t lba;
    int writing;
    uint16_t ident[256];
} cf;

enum { ST_ERR = 1, ST_DRQ = 8, ST_DSC = 0x10, ST_DRDY = 0x40, ST_BSY = 0x80 };

static uint32_t cf_cur_lba(void) {
    if (cf.head & 0x40) return ((uint32_t)(cf.head & 15) << 24) | ((uint32_t)cf.cyl_hi << 16) | ((uint32_t)cf.cyl_lo << 8) | cf.sector;
    uint32_t cyl = ((uint32_t)cf.cyl_hi << 8) | cf.cyl_lo;
    return (cyl * 4 + (cf.head & 15)) * 32 + cf.sector - 1;
}

static void cf_set_lba(uint32_t lba) {
    if (cf.head & 0x40) {
        cf.sector = lba & 0xff; cf.cyl_lo = (lba >> 8) & 0xff; cf.cyl_hi = (lba >> 16) & 0xff;
        cf.head = (cf.head & 0xf0) | ((lba >> 24) & 15);
    } else {
        uint32_t s = lba % 32 + 1, t = lba / 32, h = t % 4, c = t / 4;
        cf.sector = (uint8_t)s; cf.head = (cf.head & 0xf0) | (uint8_t)h; cf.cyl_lo = c & 0xff; cf.cyl_hi = (c >> 8) & 0xff;
    }
}

static void cf_load_sector(void) {
    memset(cf.buf, 0, SECTOR);
    if (cf.img && cf.lba < cf.nsect) {
        fseek(cf.img, (long)cf.lba * SECTOR, SEEK_SET);
        if (fread(cf.buf, 1, SECTOR, cf.img) != SECTOR) cf.status |= ST_ERR;
    }
    cf.buf_pos = 0;
    cf.buf_len = SECTOR;
}

static void cf_build_ident(void) {
    memset(cf.ident, 0, sizeof cf.ident);
    cf.ident[0] = 0x848a;                       /* CFA */
    cf.ident[1] = 490; cf.ident[3] = 4; cf.ident[6] = 32;
    const char *model = "RECOMP CF CARD";
    for (int i = 0; i < 20; i++) {
        char a = model[2 * i] ? model[2 * i] : ' ';
        char b = (a != ' ' || model[2 * i]) && model[2 * i + 1] ? model[2 * i + 1] : ' ';
        cf.ident[27 + i] = (uint16_t)((a << 8) | b);
    }
    cf.ident[47] = 1;                          /* max multiple */
    cf.ident[49] = 0x0200;                     /* LBA */
    cf.ident[51] = 0x0200;                     /* per MAME: Viper BIOS expects these */
    cf.ident[53] = 1;
    cf.ident[54] = 490; cf.ident[55] = 4; cf.ident[56] = 32;
    cf.ident[57] = cf.nsect & 0xffff; cf.ident[58] = cf.nsect >> 16;
    cf.ident[60] = cf.nsect & 0xffff; cf.ident[61] = cf.nsect >> 16;
    cf.ident[67] = 0x00f0;
}

static void cf_command(uint8_t cmd) {
    cf.error = 0;
    cf.status = ST_DRDY | ST_DSC;
    switch (cmd) {
    case 0xec:                                  /* IDENTIFY DEVICE */
        for (int i = 0; i < 256; i++) { cf.buf[2 * i] = cf.ident[i] & 0xff; cf.buf[2 * i + 1] = cf.ident[i] >> 8; }
        cf.buf_pos = 0; cf.buf_len = SECTOR; cf.remaining = 1; cf.writing = 0;
        cf.status |= ST_DRQ;
        break;
    case 0x20: case 0x21: case 0xc4:            /* READ SECTORS / READ MULTIPLE */
        cf.lba = cf_cur_lba();
        cf.remaining = cf.count ? cf.count : 256;
        {   /* RT_CF_LOG=1: log every read command, e.g. to see which game files are loaded */
            static int log = -1;
            if (log < 0) log = getenv("RT_CF_LOG") != NULL;
            if (log) rt_log("CF: read lba %u count %d\n", cf.lba, cf.remaining);
        }
        cf.writing = 0;
        cf_load_sector();
        cf.status |= ST_DRQ;
        break;
    case 0x30: case 0x31: case 0xc5:            /* WRITE SECTORS (kept in memory only) */
        cf.lba = cf_cur_lba();
        cf.remaining = cf.count ? cf.count : 256;
        cf.writing = 1; cf.buf_pos = 0; cf.buf_len = SECTOR;
        cf.status |= ST_DRQ;
        rt_log("CF: write command at lba %u ignored (read-only image)\n", cf.lba);
        break;
    case 0xef: case 0xc6: case 0x91: case 0x10: case 0xe0: case 0xe1: case 0xe5: case 0xe7:
        break;                                  /* set features / multiple / params / recalibrate / power */
    default:
        rt_log("CF: unknown ATA command %02x\n", cmd);
        cf.status |= ST_ERR; cf.error = 4;
        break;
    }
}

static uint16_t cf_data_read(void) {
    if (!(cf.status & ST_DRQ) || cf.writing) return 0;
    uint16_t v = (uint16_t)(cf.buf[cf.buf_pos] | (cf.buf[cf.buf_pos + 1] << 8));
    cf.buf_pos += 2;
    if (cf.buf_pos >= cf.buf_len) {
        if (--cf.remaining > 0) {
            cf.lba++;
            cf_set_lba(cf.lba);
            cf_load_sector();
        } else {
            cf.status &= ~ST_DRQ;
            cf_set_lba(cf.lba + 1);
        }
    }
    return v;
}

static void cf_data_write(uint16_t v) {
    if (!(cf.status & ST_DRQ) || !cf.writing) return;
    cf.buf_pos += 2;
    (void)v;
    if (cf.buf_pos >= cf.buf_len) {
        cf.buf_pos = 0;
        if (--cf.remaining <= 0) cf.status &= ~ST_DRQ;
    }
}

static uint16_t cf_task_read(int reg) {
    switch (reg) {
    case 0: return cf_data_read();
    case 1: return cf.error;
    case 2: return cf.count;
    case 3: return cf.sector;
    case 4: return cf.cyl_lo;
    case 5: return cf.cyl_hi;
    case 6: return cf.head;
    case 7: return cf.status;
    }
    return 0;
}

static void cf_task_write(int reg, uint16_t v) {
    switch (reg) {
    case 0: cf_data_write(v); break;
    case 1: cf.feature = (uint8_t)v; break;
    case 2: cf.count = (uint8_t)v; break;
    case 3: cf.sector = (uint8_t)v; break;
    case 4: cf.cyl_lo = (uint8_t)v; break;
    case 5: cf.cyl_hi = (uint8_t)v; break;
    case 6: cf.head = (uint8_t)v; break;
    case 7: cf_command((uint8_t)v); break;
    }
}

static const uint8_t cf_tuples[] = { 0x01, 0x01, 0xd0, 0x1a, 0xff, 0x03, 0x00, 0x00, 0x01, 0x00, 0x00 };

/* 16-bit lane at +4 of each 8-byte group; returns lane value for a given access */
static int lane16(uint32_t off, int size, uint32_t *shift) {
    int k = off & 7;
    if (size == 2 && k == 4) { *shift = 0; return 1; }
    if (size == 4 && k == 4) { *shift = 16; return 1; }
    if (size == 1 && (k == 4 || k == 5)) { *shift = (k == 4) ? 8 : 0; return 1; }
    return 0;
}

static uint32_t cf_regs_read(uint32_t off, int size) {
    uint32_t sh;
    if (!lane16(off, size, &sh)) return 0;
    uint32_t idx = off >> 3;
    uint32_t v = 0;
    if (cf.ide_mode) {
        switch (idx & 15) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: v = cf_task_read(idx & 7); break;
        case 0xd: v = cf.error; break;
        case 0xe: v = cf.status; break;       /* alt status */
        case 0xf: v = 0; break;
        default: break;
        }
    } else {
        if ((idx >> 1) < sizeof cf_tuples) v = cf_tuples[idx >> 1];
    }
    if (size == 1) return (sh == 8) ? (v >> 8) & 0xff : v & 0xff;
    return (v & 0xffff) << sh;
}

static void cf_regs_write(uint32_t off, int size, uint32_t v) {
    uint32_t sh;
    if (!lane16(off, size, &sh)) return;
    uint32_t d = (size == 1) ? (sh == 8 ? v << 8 : v) : (v >> sh) & 0xffff;
    uint32_t idx = off >> 3;
    if (idx < 0x10) {
        switch (idx & 15) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: cf_task_write(idx & 7, (uint16_t)d); break;
        case 0xd: cf.feature = (uint8_t)d; break;
        case 0xe: cf.devctl = (uint8_t)d; if (d & 4) { cf.status = ST_DRDY | ST_DSC; } break;
        default: break;
        }
    } else if (idx == 0x100) {
        if (d & 0x80) {
            cf.ide_mode = 1;
            cf.status = ST_DRDY | ST_DSC;
            cf.head = 0; cf.count = 1; cf.sector = 1; cf.cyl_lo = cf.cyl_hi = 0;
        }
    }
}

static uint32_t cf_data_read_bus(uint32_t off, int size) {
    uint32_t sh;
    if (!lane16(off, size, &sh) || (off >> 3 & 15) != 8) return 0;
    return (uint32_t)cf_data_read() << sh;
}

static void cf_data_write_bus(uint32_t off, int size, uint32_t v) {
    uint32_t sh;
    if (!lane16(off, size, &sh) || (off >> 3 & 15) != 8) return;
    cf_data_write((uint16_t)(v >> sh));
}

static void cf_init(const char *path) {
    memset(&cf, 0, sizeof cf);
    cf.img = path ? fopen(path, "rb") : NULL;
    if (!cf.img) { rt_log("CF: cannot open image %s\n", path ? path : "(null)"); return; }
    fseek(cf.img, 0, SEEK_END);
    cf.nsect = (uint32_t)(ftell(cf.img) / SECTOR);
    cf.status = ST_DRDY | ST_DSC;
    cf.ide_mode = 1;              /* the BIOS booted from the card, which stays in IDE mode */
    cf.head = 0xa0;
    cf_build_ident();
    rt_log("CF: %s, %u sectors\n", path, cf.nsect);
}

/* ================================================================== "unknown serial" @ff300000 */
static struct { int bit_w; uint16_t cmd, data, data_r; uint8_t regs[0x80]; } us;

static uint32_t userial_read(uint32_t off, int size) {
    uint32_t sh;
    if (!lane16(off, size, &sh)) return 0;
    uint32_t bit = us.data_r & 1;
    us.data_r >>= 1;
    return (bit << 1) << sh;     /* bit 17 of the 64-bit word == bit 1 of the lane */
}

static void userial_write(uint32_t off, int size, uint32_t v) {
    uint32_t sh;
    if (!lane16(off, size, &sh)) return;
    uint32_t d = (v >> sh) & 0xffff;
    if (!(d & 1)) return;
    int bit = (d >> 1) & 1;
    if (us.bit_w < 8) { if (us.bit_w > 0) us.cmd <<= 1; us.cmd |= bit; }
    else { if (us.bit_w > 8) us.data <<= 1; us.data |= bit; }
    us.bit_w++;
    if (us.bit_w == 8 && !(us.cmd & 0x80)) {
        uint8_t x = us.regs[us.cmd & 0x7f], r = 0;
        for (int i = 0; i < 8; i++) if (x & (1 << i)) r |= 0x80 >> i;
        us.data_r = r;
    }
    if (us.bit_w == 16) {
        if (us.cmd & 0x80) us.regs[us.cmd & 0x7f] = (uint8_t)us.data;
        us.bit_w = 0; us.cmd = 0; us.data = 0;
    }
}

/* ================================================================== M48T58 timekeeper (as MAME timekpr.cpp) */
static uint8_t g_nvram[0x2000];
static struct { uint8_t control, seconds, minutes, hours, day, date, month, year; } rtc;
enum { RTC_CONTROL = 0x1ff8, RTC_SECONDS, RTC_MINUTES, RTC_HOURS, RTC_DAY, RTC_DATE, RTC_MONTH, RTC_YEAR };
#define CONTROL_W 0x80
#define CONTROL_R 0x40
#define SECONDS_ST 0x80
#define DAY_CEB 0x20
#define DAY_CB 0x10
#define DATE_BL 0x40

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

static int inc_bcd(uint8_t *data, int mask, int min, int max) {
    int bcdv = (*data + 1) & mask, carry = 0;
    if ((bcdv & 0x0f) > 9) { bcdv &= 0xf0; bcdv += 0x10; }
    if (bcdv > max) { bcdv = min; carry = 1; }
    *data = (uint8_t)((*data & ~mask) | (bcdv & mask));
    return carry;
}

static void rtc_to_ram(void) {
    g_nvram[RTC_CONTROL] = rtc.control; g_nvram[RTC_SECONDS] = rtc.seconds; g_nvram[RTC_MINUTES] = rtc.minutes;
    g_nvram[RTC_HOURS] = rtc.hours; g_nvram[RTC_DAY] = rtc.day; g_nvram[RTC_DATE] = rtc.date;
    g_nvram[RTC_MONTH] = rtc.month; g_nvram[RTC_YEAR] = rtc.year;
}

static void rtc_from_ram(void) {
    rtc.control = g_nvram[RTC_CONTROL]; rtc.seconds = g_nvram[RTC_SECONDS]; rtc.minutes = g_nvram[RTC_MINUTES];
    rtc.hours = g_nvram[RTC_HOURS]; rtc.day = g_nvram[RTC_DAY]; rtc.date = g_nvram[RTC_DATE];
    rtc.month = g_nvram[RTC_MONTH]; rtc.year = g_nvram[RTC_YEAR];
}

static void rtc_tick(void *arg) {
    (void)arg;
    rt_sched_at(rt_now() + (uint64_t)CPU_HZ, rtc_tick, NULL);
    if ((rtc.seconds & SECONDS_ST) || (rtc.control & CONTROL_W)) return;
    int carry = inc_bcd(&rtc.seconds, 0x7f, 0, 0x59);
    if (carry) carry = inc_bcd(&rtc.minutes, 0x7f, 0, 0x59);
    if (carry) carry = inc_bcd(&rtc.hours, 0x3f, 0, 0x23);
    if (carry) {
        static const uint8_t dim[] = {0x31, 0x28, 0x31, 0x30, 0x31, 0x30, 0x31, 0x31, 0x30, 0x31, 0x30, 0x31};
        inc_bcd(&rtc.day, 0x07, 1, 7);
        int month = from_bcd(rtc.month), year = from_bcd(rtc.year);
        uint8_t maxd = (month == 2 && year % 4 == 0) ? 0x29 : (month >= 1 && month <= 12) ? dim[month - 1] : 0x31;
        carry = inc_bcd(&rtc.date, 0x3f, 1, maxd);
    }
    if (carry) carry = inc_bcd(&rtc.month, 0x1f, 1, 0x12);
    if (carry) carry = inc_bcd(&rtc.year, 0xff, 0, 0x99);
    if (carry && (rtc.day & DAY_CEB)) rtc.day ^= DAY_CB;
    if (!(rtc.control & CONTROL_R)) rtc_to_ram();
}

static void rtc_init(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    rtc.control = 0;
    rtc.seconds = bcd(tm.tm_sec); rtc.minutes = bcd(tm.tm_min); rtc.hours = bcd(tm.tm_hour);
    rtc.day = bcd(tm.tm_wday + 1); rtc.date = bcd(tm.tm_mday); rtc.month = bcd(tm.tm_mon + 1);
    rtc.year = bcd((tm.tm_year + 1900) % 100);
    rtc_to_ram();
    rt_sched_at(rt_now() + (uint64_t)CPU_HZ, rtc_tick, NULL);
}

/* the NVRAM image, for the enhanced mode's settings (guest thread only) */
uint8_t *hw_nvram(void) { return g_nvram; }

/* The TEST MODE options are a block of the NVRAM: the big-endian 16-bit words from
 * GAME_NVRAM_OPT_START up to the checksum word GAME_NVRAM_OPT_CSUM (included) sum to 0xffff. */
void hw_nvram_options_fix(uint8_t *nv) {
    if (!GAME_NVRAM_OPT_CSUM) return;
    uint32_t sum = 0;
    for (int o = GAME_NVRAM_OPT_START; o < GAME_NVRAM_OPT_CSUM; o += 2) sum += (uint32_t)(nv[o] << 8 | nv[o + 1]);
    uint16_t cs = (uint16_t)(0xffff - sum);
    nv[GAME_NVRAM_OPT_CSUM] = (uint8_t)(cs >> 8);
    nv[GAME_NVRAM_OPT_CSUM + 1] = (uint8_t)cs;
}

/* The profile's nvram_force: TEST MODE option bits set at every boot, in both modes (the NETWORK
 * ID: 1, a single cabinet; some dumps come from cabinet 2 of a linked set, and the race HUD then
 * says PLAYER 2). Only an option block whose checksum is valid is touched; an empty NVRAM gets
 * the game's factory settings. */
static void nvram_force(void) {
    static const struct { int addr, mask, value; } k_force[] = GAME_NVRAM_FORCE;
    if (k_force[0].addr < 0 || !GAME_NVRAM_OPT_CSUM) return;
    uint32_t sum = 0;
    for (int o = GAME_NVRAM_OPT_START; o <= GAME_NVRAM_OPT_CSUM; o += 2) sum += (uint32_t)(g_nvram[o] << 8 | g_nvram[o + 1]);
    if ((sum & 0xffff) != 0xffff) return;
    int changed = 0;
    for (int i = 0; k_force[i].addr >= 0; i++) {
        uint8_t v = (uint8_t)((g_nvram[k_force[i].addr] & ~k_force[i].mask) | k_force[i].value);
        if (v != g_nvram[k_force[i].addr]) { g_nvram[k_force[i].addr] = v; changed = 1; }
    }
    if (changed) { hw_nvram_options_fix(g_nvram); rt_log("NVRAM: profile settings applied (nvram_force)\n"); }
}

static uint8_t nvram_read(uint32_t off) {
    uint8_t r = g_nvram[off];
    if (off == RTC_DATE) r &= ~DATE_BL;
    return r;
}

static void nvram_write(uint32_t off, uint8_t v) {
    if (off == RTC_CONTROL) {
        if ((rtc.control & CONTROL_W) && !(v & CONTROL_W)) rtc_from_ram();
        if ((rtc.control & CONTROL_R) && !(v & CONTROL_W)) rtc_to_ram();
        rtc.control = v;
    } else if (off == RTC_DAY) {
        rtc.day = (rtc.day & ~DAY_CEB) | (v & DAY_CEB);
    }
    g_nvram[off] = v;
}

/* ================================================================== DS2430A 1-Wire EEPROM */
enum { DS_PRESENCE, DS_ROM_CMD, DS_ROM_READ, DS_ROM_MATCH, DS_ROM_SEARCH, DS_ROM_SEARCH_C, DS_ROM_SEARCH_W,
       DS_MEM_CMD, DS_MEM_READ, DS_MEM_WRITE, DS_MEM_COPY, DS_DONE };
static struct {
    int data_in, data_out;
    uint8_t shift, command;
    int bits;
    uint64_t pulse_start;
    int state;
    uint8_t eeprom[32], rom[8], scratch[32], app[8], start;
} ds;

#define T_RSTL US_TO_CYC(480)
#define T_RSTH US_TO_CYC(480)
#define T_PDL US_TO_CYC(120)
#define T_PDH US_TO_CYC(16)
#define T_SLOT US_TO_CYC(60)
#define T_REC US_TO_CYC(1)
#define T_LOW0 US_TO_CYC(60)
#define T_RELEASE US_TO_CYC(30)
#define T_COPY US_TO_CYC(10000)
#define T_SLOT_READ (US_TO_CYC(15) + T_RELEASE + T_REC)

static void ds_timer(void *arg);
static void ds_arm(uint64_t d) { rt_sched_cancel(ds_timer, NULL); rt_sched_at(rt_now() + d, ds_timer, NULL); }

static int ds_set_state(int s) { if (ds.state != s) { ds.state = s; ds.bits = 0; return 1; } return 0; }

static uint8_t ds_read_mem(uint8_t cmd, int idx) {
    switch (cmd) {
    case 0x66: return 0xff;
    case 0xaa: case 0xf0: return ds.scratch[(ds.start + idx) & 31];
    case 0xc3: return ds.app[(ds.start + idx) & 7];
    }
    return 0xff;
}

static int ds_next_state(int prev, uint8_t cmd, int idx, uint8_t data) {
    if (prev == DS_ROM_CMD) {
        switch (cmd) {
        case 0x33: return DS_ROM_READ;
        case 0x55: return DS_ROM_MATCH;
        case 0xcc: return DS_MEM_CMD;
        case 0xf0: return DS_ROM_SEARCH;
        default: return DS_DONE;
        }
    }
    if (prev == DS_ROM_READ && idx == 7) return DS_DONE;
    if ((prev == DS_ROM_MATCH || prev == DS_ROM_SEARCH_W) && idx == 7) return DS_MEM_CMD;
    if (prev == DS_MEM_CMD) {
        switch (cmd) {
        case 0x0f: case 0x99:
            if (idx == 1) { ds.start = data; return DS_MEM_WRITE; }
            return DS_MEM_CMD;
        case 0x55: case 0x5a:
            if (idx == 1) return data == 0xa5 ? DS_MEM_COPY : DS_DONE;
            return DS_MEM_CMD;
        case 0x66:
            if (idx == 1) return data == 0 ? DS_MEM_READ : DS_DONE;
            return DS_MEM_CMD;
        case 0xf0:
            if (idx == 0) memcpy(ds.scratch, ds.eeprom, 32);
            /* fallthrough */
        case 0xaa: case 0xc3:
            if (idx == 1) { ds.start = data; return DS_MEM_READ; }
            return DS_MEM_CMD;
        default:
            rt_log("DS2430: unknown memory command %02x\n", cmd);
            return DS_DONE;
        }
    }
    if (prev == DS_MEM_READ && cmd == 0x66) return DS_DONE;
    if (prev == DS_MEM_WRITE) {
        if (cmd == 0x0f) ds.scratch[(ds.start + idx) & 31] = data;
        else if (cmd == 0x99) ds.app[(ds.start + idx) & 7] = data;
    }
    return prev;
}

static void ds_timer(void *arg) {
    (void)arg;
    switch (ds.state) {
    case DS_PRESENCE:
        ds.data_out = !ds.data_out;
        if (ds.data_out) ds_set_state(DS_ROM_CMD);
        else ds_arm(T_PDL);
        break;
    case DS_MEM_READ: case DS_ROM_READ: case DS_ROM_SEARCH: case DS_ROM_SEARCH_C: case DS_DONE:
        ds.data_out = 1;
        break;
    case DS_MEM_COPY:
        if (ds.command == 0x55) memcpy(ds.eeprom, ds.scratch, 32);
        ds.state = DS_DONE;
        break;
    default: break;
    }
}

static void ds_pulse_start(uint64_t t) {
    if (ds.pulse_start > t) return;
    ds.pulse_start = t;
    switch (ds.state) {
    case DS_MEM_READ: case DS_ROM_READ: case DS_ROM_SEARCH:
        if ((ds.bits & 7) == 0)
            ds.shift = ds.state == DS_MEM_READ ? ds_read_mem(ds.command, ds.bits >> 3) : ds.rom[(ds.bits >> 3) & 7];
        if (!(ds.shift & 1)) { ds.data_out = 0; ds_arm(T_RELEASE); }
        break;
    case DS_ROM_MATCH:
        if ((ds.bits & 7) == 0) ds.shift = ds.rom[(ds.bits >> 3) & 7];
        break;
    case DS_ROM_SEARCH_C:
        if (ds.shift & 1) { ds.data_out = 0; ds_arm(T_RELEASE); }
        break;
    case DS_MEM_COPY:
        ds_set_state(DS_DONE);
        break;
    default: break;
    }
}

static void ds_pulse_end(uint64_t t) {
    if (ds.pulse_start >= t) return;
    uint64_t w = t - ds.pulse_start;
    if (w >= T_RSTL) {
        ds_set_state(DS_PRESENCE);
        ds_arm(T_PDH);
        ds.pulse_start = t + T_RSTH;
        return;
    }
    if (w < T_REC) return;
    switch (ds.state) {
    case DS_ROM_CMD: case DS_MEM_CMD: case DS_MEM_WRITE:
        ds.shift >>= 1;
        if (w < T_LOW0) ds.shift |= 0x80;
        if ((ds.bits & 7) == 7) {
            if ((ds.bits >> 3) == 0 && (ds.state == DS_ROM_CMD || ds.state == DS_MEM_CMD)) ds.command = ds.shift;
            int nx = ds_next_state(ds.state, ds.command, ds.bits >> 3, ds.shift);
            if (ds_set_state(nx)) {
                if (nx == DS_MEM_COPY) { ds_arm(T_COPY); break; }
            } else ds.bits++;
        } else ds.bits++;
        ds.pulse_start += T_SLOT;
        break;
    case DS_MEM_READ: case DS_ROM_READ:
        ds.shift >>= 1;
        if ((ds.bits & 7) != 7 || !ds_set_state(ds_next_state(ds.state, ds.command, ds.bits >> 3, 0))) ds.bits++;
        ds.pulse_start += T_SLOT_READ;
        break;
    case DS_ROM_SEARCH: ds.state = DS_ROM_SEARCH_C; ds.pulse_start += T_SLOT_READ; break;
    case DS_ROM_SEARCH_C: ds.state = DS_ROM_SEARCH_W; ds.pulse_start += T_SLOT_READ; break;
    case DS_ROM_MATCH: case DS_ROM_SEARCH_W:
        if ((w >= T_LOW0) == (ds.shift & 1)) { ds_set_state(DS_DONE); }
        else {
            ds.shift >>= 1;
            if ((ds.bits & 7) != 7 || !ds_set_state(ds_next_state(ds.state, ds.command, ds.bits >> 3, 0))) {
                ds.bits++;
                if (ds.state == DS_ROM_SEARCH_W) ds.state = DS_ROM_SEARCH;
            }
        }
        ds.pulse_start += T_SLOT;
        break;
    default: break;
    }
}

static void ds_data_w(int s) {
    if (ds.data_in && !s) { ds.data_in = 0; ds_pulse_start(rt_now()); }
    else if (!ds.data_in && s) { ds.data_in = 1; ds_pulse_end(rt_now()); }
}

static int ds_data_r(void) { return ds.data_in && ds.data_out; }

static void ds_init(const char *path) {
    memset(&ds, 0, sizeof ds);
    ds.data_in = ds.data_out = 1;
    ds.state = DS_DONE;
    memset(ds.eeprom, 0xff, 32);
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) {
        if (fread(ds.eeprom, 1, 32, f) != 32 || fread(ds.rom, 1, 8, f) != 8) rt_log("DS2430: short file\n");
        fclose(f);
    } else {
        static const uint8_t fake[8] = {0x14, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00};
        memcpy(ds.rom, fake, 8);
    }
}

/* ================================================================== inputs / outputs */
uint8_t g_in[8] = GAME_INPUT_DEFAULTS;
static int g_sound_irq_enabled;

static uint8_t io_read(uint32_t off) {
    if (off == 2) return (uint8_t)((g_in[2] & ~0x20) | (ds_data_r() ? 0x20 : 0));
    return g_in[off & 7];
}

/* The BIOS passes the IN2 byte it read at boot (DIP switches, DS2430 line) in the top byte of
 * r31 (MAME: 0x63ffffff for thrild2 with IN2=0x43, 0x61ffffff for gticlub2ea with IN2=0x41). */
uint32_t hw_boot_param(void) { return ((uint32_t)io_read(2) << 24) | 0x00ffffffu; }

static void io_write(uint32_t off, uint8_t v) {
    if (off == 0) {
        g_sound_irq_enabled = (v >> 5) & 1;
        return;
    }
}

/* ================================================================== sound (IRQ3 every 256 samples) */
static uint32_t g_sound_buf = 0xfff800;
void audio_push_block(const uint8_t *ram_block);

static void sound_tick(void *arg) {
    (void)arg;
    rt_sched_at(rt_now() + (uint64_t)(CPU_HZ * 256.0 / 44100.0), sound_tick, NULL);
    if (!g_sound_irq_enabled) return;
    epic_raise(EPIC_IRQ3);
    audio_push_block(g_ram + g_sound_buf);
    g_sound_buf ^= 0x800;
}

/* ================================================================== UART (PC16552, debug console) */
static uint8_t uart_regs[2][8];
static char uart_line[2][256];
static int uart_len[2];

static uint8_t uart_read(uint32_t off) {
    int ch = (off >> 3) & 1, r = off & 7;
    if (r == 5) return 0x60;             /* THR empty, transmitter empty */
    if (r == 2) return 0x01;             /* no interrupt pending */
    return uart_regs[ch][r];
}

static void uart_write(uint32_t off, uint8_t v) {
    int ch = (off >> 3) & 1, r = off & 7;
    if (r == 0 && !(uart_regs[ch][3] & 0x80)) {
        if (v == '\n' || uart_len[ch] >= 255) {
            uart_line[ch][uart_len[ch]] = 0;
            rt_log("[uart%d] %s\n", ch, uart_line[ch]);
            uart_len[ch] = 0;
        } else if (v != '\r') {
            uart_line[ch][uart_len[ch]++] = (char)v;
        }
        return;
    }
    uart_regs[ch][r] = v;
}

/* ================================================================== K056230 LANC (network, no peers) */
static struct { uint8_t ram[0x2000]; uint8_t status, control, unk[2]; int irq_enable, irq; } lanc = { .status = 0x08 };

static uint8_t lanc_reg_read(uint32_t off) {
    switch (off & 7) {
    case 2: return lanc.status;
    case 3: case 4: return lanc.unk[(off & 7) - 3];
    }
    return 0;
}

static void lanc_reg_write(uint32_t off, uint8_t v) {
    switch (off & 7) {
    case 1:
        if (!(v & 1)) { lanc.status = 0; lanc.irq = 0; }
        else if (lanc.irq_enable) lanc.irq = 1;
        if (v & 8) lanc.status = 0x10;
        if (lanc.irq) epic_raise(EPIC_IRQ1);
        lanc.control = v;
        break;
    case 3: case 4: lanc.unk[(off & 7) - 3] = v; break;
    case 5: lanc.irq_enable = v & 1; break;
    }
}

static uint8_t lanc_ram_read(uint32_t off) { return lanc.ram[off & 0x1fff]; }
static void lanc_ram_write(uint32_t off, uint8_t v) { lanc.ram[off & 0x1fff] = v; }

/* ================================================================== bus router */
static uint32_t bytes_read(uint8_t (*fn)(uint32_t), uint32_t off, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | fn(off + i);
    return v;
}

static void bytes_write(void (*fn)(uint32_t, uint8_t), uint32_t off, int size, uint32_t v) {
    for (int i = 0; i < size; i++) fn(off + i, (v >> (8 * (size - 1 - i))) & 0xff);
}

static long g_mmio_log = -1;
static uint32_t g_mmio_lo = 0, g_mmio_hi = 0xffffffffu;
static int mmio_logging_ea(uint32_t ea) {
    if (g_mmio_log < 0) {
        const char *e = getenv("RT_MMIO_LOG"), *r = getenv("RT_MMIO_RANGE");
        g_mmio_log = e ? atol(e) : 0;
        if (r) sscanf(r, "%x-%x", &g_mmio_lo, &g_mmio_hi);
    }
    if (ea < g_mmio_lo || ea > g_mmio_hi) return 0;
    return g_mmio_log > 0 ? (g_mmio_log--, 1) : 0;
}
#define mmio_logging() mmio_logging_ea(ea)
static uint32_t hw_read_(uint32_t ea, int size);
static void hw_write_(uint32_t ea, int size, uint32_t v);
uint32_t hw_read(uint32_t ea, int size) {
    uint32_t v = hw_read_(ea, size);
    if (mmio_logging()) rt_log("mmio R%d %08x -> %0*x (lr=%08x)\n", size * 8, ea, size * 2, v, g_ctx.lr);
    return v;
}
void hw_write(uint32_t ea, int size, uint32_t v) {
    if (mmio_logging()) rt_log("mmio W%d %08x <- %0*x (lr=%08x)\n", size * 8, ea, size * 2, v, g_ctx.lr);
    hw_write_(ea, size, v);
}

static uint32_t hw_read_(uint32_t ea, int size) {
    if (ea >= 0x80000000u && ea < 0x80100000u) return eumb_read(ea - 0x80000000u, size);
    if (ea >= 0x82000000u && ea < 0x84000000u) {
        uint32_t off = ea - 0x82000000u;
        return le_bus_read(voodoo_reg_read(off & ~3u), off & 3, size);
    }
    if (ea >= 0x84000000u && ea < 0x86000000u) {
        uint32_t off = ea - 0x84000000u;
        return le_bus_read(voodoo_lfb_read(off & ~3u), off & 3, size);
    }
    if (ea >= 0xfe800000u && ea < 0xfe800100u) {
        uint32_t off = ea - 0xfe800000u;
        return le_bus_read(voodoo_io_read(off & ~3u), off & 3, size);
    }
    if (ea >= 0xfec00000u && ea < 0xfee00000u) return le_bus_read(pci_addr, ea & 3, size);
    if (ea >= 0xfee00000u && ea < 0xfef00000u) return le_bus_read(pci_cfg_read(), ea & 3, size);
    if (ea >= 0xff000000u && ea < 0xff001000u) return cf_data_read_bus(ea & 0xfff, size);
    if (ea >= 0xff200000u && ea < 0xff201000u) return cf_regs_read(ea & 0xfff, size);
    if (ea >= 0xff300000u && ea < 0xff301000u) return userial_read(ea & 0xfff, size);
    if (ea >= 0xffe00000u && ea < 0xffe00010u) return bytes_read(uart_read, ea & 15, size);
    if (ea >= 0xffe10000u && ea < 0xffe10008u) return bytes_read(io_read, ea & 7, size);
    if (ea >= 0xffe30000u && ea < 0xffe32000u) return bytes_read(nvram_read, ea & 0x1fff, size);
    if (ea >= 0xffe70000u && ea < 0xffe70008u) { ds_data_w(0); return 0; }
    if (ea >= 0xffe78000u && ea < 0xffe78008u) return 0;
    if (ea >= 0xffe28000u && ea < 0xffe28008u) return 0;
    if (ea >= 0xffe40000u && ea < 0xffe40008u) return 0;
    if (ea >= 0xffe60000u && ea < 0xffe60008u) return 0;
    if (ea >= 0xffe98000u && ea < 0xffe98008u) return bytes_read(lanc_reg_read, ea & 7, size);
    if (ea >= 0xffe9a000u && ea < 0xffe9c000u) return bytes_read(lanc_ram_read, ea & 0x1fff, size);
    if (ea >= 0xffea0000u && ea < 0xffea0008u) return size == 1 ? 0xff : 0xffffffffu >> (32 - 8 * size);
    if (ea >= 0xfff00000u && ea < 0xfff40000u && g_bios) {
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v = (v << 8) | g_bios[(ea - 0xfff00000u + i) & 0x3ffff];
        return v;
    }
    unmapped("bus", ea, size, 0, 0);
    return 0;
}

static void hw_write_(uint32_t ea, int size, uint32_t v) {
    if (ea >= 0x80000000u && ea < 0x80100000u) { eumb_write(ea - 0x80000000u, size, v); return; }
    if ((ea >= 0x82000000u && ea < 0x86000000u) || (ea >= 0xfe800000u && ea < 0xfe800100u)) {
        uint32_t k = ea & 3;
        uint32_t lv = le_bus_write(0, (int)k, size, v), lm = le_bus_write(0, (int)k, size, 0xffffffffu);
        if (ea < 0x84000000u) voodoo_reg_write((ea - 0x82000000u) & ~3u, lv, lm);
        else if (ea < 0x86000000u) voodoo_lfb_write((ea - 0x84000000u) & ~3u, lv, lm);
        else voodoo_io_write((ea - 0xfe800000u) & ~3u, lv, lm);
        return;
    }
    if (ea >= 0xfec00000u && ea < 0xfee00000u) { pci_addr = le_bus_write(pci_addr, ea & 3, size, v); return; }
    if (ea >= 0xfee00000u && ea < 0xfef00000u) { pci_cfg_write(le_bus_write(pci_cfg_read(), ea & 3, size, v)); return; }
    if (ea >= 0xff000000u && ea < 0xff001000u) { cf_data_write_bus(ea & 0xfff, size, v); return; }
    if (ea >= 0xff200000u && ea < 0xff201000u) { cf_regs_write(ea & 0xfff, size, v); return; }
    if (ea >= 0xff300000u && ea < 0xff301000u) { userial_write(ea & 0xfff, size, v); return; }
    if (ea >= 0xffe00000u && ea < 0xffe00010u) { bytes_write(uart_write, ea & 15, size, v); return; }
    if (ea >= 0xffe10000u && ea < 0xffe10008u) { bytes_write(io_write, ea & 7, size, v); return; }
    if (ea >= 0xffe30000u && ea < 0xffe32000u) { bytes_write(nvram_write, ea & 0x1fff, size, v); return; }
    if (ea >= 0xffe50000u && ea < 0xffe50008u) { if ((ea & 7) == 0) cf.ide_mode = 0; return; }
    if (ea >= 0xffe70000u && ea < 0xffe70008u) { ds_data_w(1); return; }
    if (ea >= 0xffe78000u && ea < 0xffe78008u) return;
    if (ea >= 0xffe88000u && ea < 0xffe88008u) { if ((ea & 7) == 0) ds_data_w(0); return; }
    if (ea >= 0xffe80000u && ea < 0xffe80008u) return;
    if (ea >= 0xffe08000u && ea < 0xffe08008u) return;        /* watchdog */
    if (ea >= 0xffe20000u && ea < 0xffe20008u) { if ((ea & 7) == 0) motor_write((uint8_t)(v >> (8 * (size - 1)))); return; }
    if (ea >= 0xffe28000u && ea < 0xffe28008u) return;
    if (ea >= 0xffe40000u && ea < 0xffe40008u) return;
    if (ea >= 0xffe60000u && ea < 0xffe60008u) return;
    if (ea >= 0xffe98000u && ea < 0xffe98008u) { bytes_write(lanc_reg_write, ea & 7, size, v); return; }
    if (ea >= 0xffe9a000u && ea < 0xffe9c000u) { bytes_write(lanc_ram_write, ea & 0x1fff, size, v); return; }
    if (ea >= 0xffea0000u && ea < 0xffea0008u) return;
    if (ea >= 0xffea8000u && ea < 0xffea8008u) return;        /* sound DMA trigger? */
    unmapped("bus", ea, size, v, 1);
}

void rt_mmio_w32(uint32_t ea, uint32_t v) { hw_write(ea, 4, v); }
void rt_mmio_w16(uint32_t ea, uint32_t v) { hw_write(ea, 2, v); }
void rt_mmio_w8(uint32_t ea, uint32_t v) { hw_write(ea, 1, v); }
uint32_t rt_mmio_r32(uint32_t ea) { return hw_read(ea, 4); }
uint32_t rt_mmio_r16(uint32_t ea) { return hw_read(ea, 2); }
uint32_t rt_mmio_r8(uint32_t ea) { return hw_read(ea, 1); }

void epic_dump(void) {
    for (int i = 0; i < EPIC_NUM; i++)
        if (g_irq_raised[i] || !ep.irq[i].mask)
            rt_log("  epic irq %2d: raised=%llu taken=%llu mask=%d prio=%d vec=%02x\n", i, (unsigned long long)g_irq_raised[i],
                   (unsigned long long)g_irq_taken[i], ep.irq[i].mask, ep.irq[i].priority, ep.irq[i].vector);
    rt_log("  epic pctpr=%u active=%d; i2c cr=%02x sr=%02x; sound_irq_enable=%d\n", ep.pctpr, ep.active_irq, i2c.cr, i2c.sr, g_sound_irq_enabled);
}

/* ================================================================== init */
void hw_init(const HwConfig *cfg) {
    g_cfg = cfg;
    epic_init();
    pci_init();
    cf_init(cfg->cf_image);
    ds_init(cfg->ds2430_path);
    memset(&i2c, 0, sizeof i2c);
    i2c.state = I2C_ADDR;
    memset(g_nvram, 0, sizeof g_nvram);
    FILE *f = cfg->nvram_save ? fopen(cfg->nvram_save, "rb") : NULL;
    if (f) rt_log("NVRAM: loading saved state %s\n", cfg->nvram_save);
    else f = cfg->nvram_path ? fopen(cfg->nvram_path, "rb") : NULL;
    if (f) {
        if (fread(g_nvram, 1, sizeof g_nvram, f) != sizeof g_nvram) rt_log("NVRAM: short file\n");
        fclose(f);
        nvram_force();
    } else {
        rt_log("NVRAM: %s not found, starting from an empty NVRAM\n", cfg->nvram_path ? cfg->nvram_path : "(none)");
    }
    rtc_init();
    for (int k = 0; k < 4; k++) {           /* test override: RT_AN0..3=hex */
        char nm[8]; snprintf(nm, sizeof nm, "RT_AN%d", k);
        if (getenv(nm)) g_analog[k] = (int16_t)strtol(getenv(nm), NULL, 0);
    }
    if (cfg->bios_path && (f = fopen(cfg->bios_path, "rb"))) {
        g_bios = (uint8_t *)malloc(0x40000);
        if (fread(g_bios, 1, 0x40000, f) != 0x40000) rt_log("BIOS: short file\n");
        fclose(f);
    }
    voodoo_init();
    rt_sched_at(rt_now() + (uint64_t)(CPU_HZ * 256.0 / 44100.0), sound_tick, NULL);
}

void nvram_save(void) {
    if (!g_cfg || !g_cfg->nvram_save) return;
    FILE *f = fopen(g_cfg->nvram_save, "wb");
    if (f) { fwrite(g_nvram, 1, sizeof g_nvram, f); fclose(f); }
}

void hw_shutdown(void) { nvram_save(); }
