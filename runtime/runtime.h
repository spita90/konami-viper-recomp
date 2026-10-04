/* Internal runtime interfaces (not used by generated code). */
#pragma once
#include "ppc_rt.h"
#include <stdio.h>

extern PPCContext g_ctx;

/* logging / errors */
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(const char *why) __attribute__((noreturn));
int rt_verbose(void);
void rt_dump_state(void);

/* virtual time: CPU cycles of the 203.2128 MHz core */
#define CPU_HZ 203212800.0
#define US_TO_CYC(us) ((uint64_t)((us) * (CPU_HZ / 1e6)))
uint64_t rt_now(void);
uint64_t rt_timebase(void);
void rt_shorten_slice(uint64_t until);

typedef void (*SchedCb)(void *arg);
void rt_sched_at(uint64_t cycle, SchedCb cb, void *arg);
void rt_sched_cancel(SchedCb cb, void *arg);
uint64_t rt_sched_next(void);
void rt_sched_run(uint64_t now);

/* dispatch / fibers */
void rt_register_module(const RtModuleInfo *m);
RtFn rt_lookup(uint32_t addr);
void rt_start(uint32_t pc);

/* interrupt controller */
enum {
    EPIC_IRQ0 = 0, EPIC_IRQ1, EPIC_IRQ2, EPIC_IRQ3, EPIC_IRQ4,
    EPIC_I2C = 16, EPIC_DMA0, EPIC_DMA1, EPIC_MSG,
    EPIC_GT0 = 20, EPIC_GT1, EPIC_GT2, EPIC_GT3, EPIC_NUM
};
void epic_raise(int irq);
int rt_irq_line(void);

/* devices */
typedef struct HwConfig {
    const char *cf_image;       /* raw CF card image (chdman extracthd output) */
    const char *nvram_path;     /* M48T58 contents (8 KiB) */
    const char *ds2430_path;    /* 40-byte DS2430A dump */
    const char *bios_path;      /* 941b01.u25 (only mapped for reads) */
    const char *nvram_save;     /* where the NVRAM is persisted (loaded instead of nvram_path if present) */
} HwConfig;

void hw_init(const HwConfig *cfg);
uint32_t hw_boot_param(void);    /* r31 at kernel entry, as the BIOS builds it */
uint8_t *hw_nvram(void);         /* M48T58 image (0x2000 bytes), guest thread only */
void hw_nvram_options_fix(uint8_t *nv);   /* recomputes the TEST MODE option block checksum */

/* enhanced (conversion) mode, runtime/enhanced.c */
extern int g_enhanced;
void enh_on_frame(const uint32_t *frame, int w, int h);
int enh_in_attract(void);
enum { ENH_UP, ENH_DOWN, ENH_LEFT, ENH_RIGHT, ENH_OK, ENH_BACK };
void enh_init(const char *work, const char *settings); /* fonts, port settings */
int enh_texture_filter(void); /* 0 original, 1 nearest */
int enh_want_fullscreen(void);
void enh_set_fullscreen(int on);
int enh_want_window(int *r);                           /* both modes: x, y, w, h; 0 if none */
void enh_set_window(const int *r);
int enh_menu_active(void);                             /* attract menu on screen */
void enh_menu_action(int action);
int enh_start_held(void);                              /* START GAME: hold START for the game */
int enh_quit_requested(void);
void enh_draw_overlay(uint32_t *fb, int w, int h);     /* menu over a 0xAARRGGBB frame */
int enh_turbo(void);                                   /* boot/apply: run unpaced, muted */
int enh_restart_requested(void);                       /* new settings written: restart */
void enh_set_headless(int on);
int enh_escape(void);                                  /* Esc: pause / back; 0 = not handled */
int enh_paused(void);
int enh_inputs_owned(void);                            /* the enhanced layer drives IN3/IN4 */
int enh_name_entry_active(void);                       /* rankings name entry: letters from the keyboard */
int enh_name_type(int ch);                             /* a letter, '\b' DEL, '\r' END; 1 if accepted */
void enh_name_step(int dir);                           /* previous (-1) / next (+1) letter on the wheel */
int enh_wheel_select_active(void);                     /* a choice from the wheel (course, transmission): */
void enh_wheel_select_step(int dir);                   /* Left/Right step (-1/+1, left to right) */
double enh_wheel_select_pos(void);                     /* and the wheel is held here (-1..1) */
void hw_shutdown(void);
void nvram_save(void);
void rt_pace_vblank(void);
void audio_frontend_push(const uint8_t *blk);
int frontend_run(int scale, int scale_explicit);     /* --scale given: its size, not the saved one */
uint32_t hw_read(uint32_t ea, int size);
void hw_write(uint32_t ea, int size, uint32_t v);

/* voodoo 3 (MAME core, runtime/voodoo/) - offsets in bytes, LE register values */
void voodoo_init(void);
uint32_t voodoo_reg_read(uint32_t off);
void voodoo_reg_write(uint32_t off, uint32_t v, uint32_t mask);
uint32_t voodoo_lfb_read(uint32_t off);
void voodoo_lfb_write(uint32_t off, uint32_t v, uint32_t mask);
uint32_t voodoo_io_read(uint32_t off);
void voodoo_io_write(uint32_t off, uint32_t v, uint32_t mask);
uint64_t voodoo_get_frame(uint32_t *dst, int max_pixels, int *w, int *h);
const uint8_t *voodoo_vram(uint32_t *size);          /* VRAM, to read (no sync with the renderer) */
uint8_t *voodoo_vram_for_write(void);                /* VRAM, to write: waits for the renderer */
void voodoo_stats(void);
void rt_eat_cycles(uint32_t n);

/* helpers for little-endian peripherals on the big-endian bus */
static inline uint32_t le_bus_read(uint32_t reg, int k, int size) {
    if (size == 4) return bswap32(reg);
    if (size == 2) return (((reg >> (8 * k)) & 0xff) << 8) | ((reg >> (8 * (k + 1))) & 0xff);
    return (reg >> (8 * k)) & 0xff;
}
static inline uint32_t le_bus_write(uint32_t old, int k, int size, uint32_t v) {
    if (size == 4) return bswap32(v);
    if (size == 2) {
        old &= ~((0xffu << (8 * k)) | (0xffu << (8 * (k + 1))));
        return old | (((v >> 8) & 0xff) << (8 * k)) | ((v & 0xff) << (8 * (k + 1)));
    }
    old &= ~(0xffu << (8 * k));
    return old | ((v & 0xff) << (8 * k));
}

/* Controller settings and sensor access stay on the frontend thread. */
int frontend_gyro_enabled(void);
int frontend_gyro_available(void);
int frontend_gyro_sensitivity(void);
void frontend_gyro_set_enabled(int on);
void frontend_gyro_set_sensitivity(int percent);
void frontend_gyro_recenter(void);
void enh_controller_settings_changed(void);
double frontend_gyro_position(void); /* normalized steering, also live while paused */
int frontend_gyro_ready(void);

double frontend_stick_position(void); /* raw normalized left stick */
double frontend_steering_position(void); /* actual steering sent to guest */
