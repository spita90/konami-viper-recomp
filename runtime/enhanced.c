/*
 * Enhanced ("conversion") mode: optional additions on top of the faithful port, enabled with
 * --enhanced. The original mode is never affected: everything here is gated on g_enhanced, and
 * the enhanced mode keeps its own NVRAM (<binary>_enhanced_nvram.bin).
 *
 * What the game data needs (addresses, scripts) comes from the "enhanced" section of
 * games/<id>/game.json, through game_config.h:
 *   - setup: a scripted TEST MODE pass run on first launch after the calibration (free play);
 *   - blank_strings: game strings emptied in RAM, e.g. the "FREE PLAY" and "PRESS START BUTTON"
 *     captions. The game module is loaded by the kernel at runtime, so the strings are checked
 *     every frame and emptied whenever their original text is found (also after a reload);
 *   - hooks: addresses where the recompiled code calls rt_hook(). "attract" is the free-play
 *     branch of the credit display (a Konami library routine shared by the games): the game
 *     draws it only while no game is in progress, so it tells the attract mode apart.
 *     "projection" and "viewport" follow the gl library's writes of its projection slots and
 *     viewport, for the widescreen option; "name_index" and "name_confirm" sit in the rankings'
 *     name entry, typed on the keyboard (see name_entry); the wheel selects' hooks (named in
 *     the profile, e.g. "course_select") run every frame of their screen (see wheel selects);
 *   - widescreen: where the gl library keeps that state.
 */
#include "runtime.h"
#include "game_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

int g_enhanced;

/* attract detection: the frame at which the "attract" hook last ran */
#define ATTRACT_GRACE_FRAMES 30
static uint64_t g_frame, g_attract_frame;
static int g_attract, g_enh_log = -1;

int enh_in_attract(void) { return g_attract; }

/* port settings (<binary>_enhanced_settings.ini, see below) */
static struct { int fullscreen, show_fps, scale, aspect, texture_filter, win[4]; } g_set = { 0, 0, 1, 0, 0, { 0 } };

/* ================================================================== widescreen */
/* The games draw a 512x384 picture through Konami's gl library, which keeps its state at fixed
 * addresses: two projection slots (perspective for the 3D, orthographic for the 2D), each with
 * six matrix terms (row 0 first) and the frustum they come from (left, right, bottom, top, near,
 * far), and the viewport (x scale, x centre, y scale, y centre). The library culls objects
 * against that frustum and clips to it.
 * A picture k times wider (Hor+: same vertical field of view) keeps every pixel where it was:
 * row 0 of each projection is divided by k and the viewport x scale multiplied by k, which
 * cancel out on screen, while the frustum is widened k times around its centre. The game then
 * draws the same picture plus what lies left and right of it, at x from -M to 512 + M; the
 * Voodoo core renders the displayed buffers with a margin of M pixels on each side
 * (voodoo_set_wide). The HUD stays where it was, in the 4:3 centre.
 * The hooks follow every write of that state, so a new value is widened once; a change of the
 * option rescales what is already there. */
static const int k_wide_margin[] = { 0, 51, 85, 199 };      /* 4:3, 16:10, 16:9, 21:9 at 384 lines */
#define N_ASPECTS 4
void voodoo_set_wide(int margin);
static double g_wide_k = 1.0;                /* wanted: picture width / 512 */
static double g_slot_k[2], g_vp_k;           /* applied to each projection slot / the viewport (0: unseen) */

static void widen_slot(int s, double f) {    /* f: new factor / applied factor */
    uint32_t m = GAME_ENH_WIDE_PROJ_MATRIX + 24 * s, fr = GAME_ENH_WIDE_PROJ_FRUSTUM + 24 * s;
    double l = LDF32(fr), r = LDF32(fr + 4), mid = (l + r) / 2, half = (r - l) / 2;
    STF32(m, LDF32(m) / f);
    STF32(m + 4, LDF32(m + 4) / f);
    STF32(fr, mid - half * f);
    STF32(fr + 4, mid + half * f);
}

static void set_aspect(int a) {              /* the Voodoo margin follows from the next frame */
    if (GAME_ENH_WIDE_VIEWPORT) voodoo_set_wide(k_wide_margin[a]);
}

static void wide_tick(int aspect) {         /* guest thread, every frame: follow the option */
    if (!GAME_ENH_WIDE_VIEWPORT) return;
    g_wide_k = (512.0 + 2 * k_wide_margin[aspect]) / 512.0;
    for (int s = 0; s < 2; s++)
        if (g_slot_k[s] && g_slot_k[s] != g_wide_k) { widen_slot(s, g_wide_k / g_slot_k[s]); g_slot_k[s] = g_wide_k; }
    if (g_vp_k && g_vp_k != g_wide_k) {
        STF32(GAME_ENH_WIDE_VIEWPORT, LDF32(GAME_ENH_WIDE_VIEWPORT) * g_wide_k / g_vp_k);
        g_vp_k = g_wide_k;
    }
}

/* ================================================================== name entry */
/* The rankings' name entry picks each letter with the steering wheel: the game turns the wheel
 * position into an index on its wheel of characters (the profile's chars, then DEL and END), and
 * a pedal confirms it. In the enhanced mode the keyboard types the letters instead: the
 * "name_index" hook replaces the index the game took from the wheel with the chosen one, and the
 * "name_confirm" hook makes the confirmation check succeed once for each typed key (DEL and END
 * included: Backspace and Enter). Left/Right step through the wheel, and the game's own
 * confirmation (a pedal) still accepts the letter shown, for a gamepad.
 * The frontend queues the keys; the guest thread takes them at the hooks, one per call. */
#define NAME_GRACE_FRAMES 10
#define NAME_QUEUE 32
enum { NAME_STEP_LEFT = -1, NAME_STEP_RIGHT = -2 };
static volatile uint64_t g_name_frame;       /* the frame at which the "name_index" hook last ran */
static int g_name_idx, g_name_confirm;       /* guest thread: the index shown, a typed key to confirm */
static volatile int g_name_queue[NAME_QUEUE];
static _Atomic unsigned g_name_w, g_name_r;  /* written by the frontend / by the guest thread */

static int name_count(void) { return GAME_ENH_NAME_CHARS ? (int)strlen(GAME_ENH_NAME_CHARS) + 2 : 0; }

int enh_name_entry_active(void) {
    return g_enhanced && g_name_frame && g_frame - g_name_frame <= NAME_GRACE_FRAMES;
}

static void name_push(int v) {
    unsigned w = g_name_w;
    if (w - g_name_r >= NAME_QUEUE) return;          /* full: drop the key */
    g_name_queue[w % NAME_QUEUE] = v;
    g_name_w = w + 1;
}

/* a typed character ('\b' DEL, '\r' END): 1 if the wheel has it */
int enh_name_type(int ch) {
    if (!enh_name_entry_active()) return 0;
    const char *chars = GAME_ENH_NAME_CHARS;
    int n = name_count();
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    int idx = ch == '\b' ? n - 2 : ch == '\r' ? n - 1 : -1;
    const char *p = ch > 0 && idx < 0 ? strchr(chars, ch) : NULL;
    if (p) idx = (int)(p - chars);
    if (idx < 0) return 0;
    name_push(idx);
    return 1;
}

void enh_name_step(int dir) {
    if (enh_name_entry_active()) name_push(dir < 0 ? NAME_STEP_LEFT : NAME_STEP_RIGHT);
}

static void name_index_hook(PPCContext *c) {
    int n = name_count();
    if (!n) return;
    if (!enh_name_entry_active()) {             /* a new name entry: start from the game's letter */
        uint32_t v = c->r[GAME_ENH_NAME_INDEX_REG];
        g_name_idx = v < (uint32_t)n ? (int)v : 0;
        g_name_confirm = 0;
        g_name_r = g_name_w;
        if (g_enh_log) rt_log("enhanced: name entry\n");
    }
    g_name_frame = g_frame;
    while (!g_name_confirm && g_name_r != g_name_w) {
        unsigned r = g_name_r;
        int v = g_name_queue[r % NAME_QUEUE];
        g_name_r = r + 1;
        if (v == NAME_STEP_LEFT) g_name_idx = (g_name_idx + n - 1) % n;
        else if (v == NAME_STEP_RIGHT) g_name_idx = (g_name_idx + 1) % n;
        else { g_name_idx = v; g_name_confirm = 1; }
    }
    c->r[GAME_ENH_NAME_INDEX_REG] = (uint32_t)g_name_idx;
    if (GAME_ENH_NAME_FIELD_REG >= 0) ST16(c->r[GAME_ENH_NAME_FIELD_REG] + GAME_ENH_NAME_FIELD_OFF, (uint32_t)g_name_idx);
}

static void name_confirm_hook(PPCContext *c) {
    if (!g_name_confirm) return;
    c->r[GAME_ENH_NAME_CONFIRM_REG] = 1;
    g_name_confirm = 0;
}

/* ================================================================== wheel selects */
/* Some selection screens take the choice from the wheel position (zones of the steering range):
 * the course select, and GTI Club 2's transmission select. With a key the choice springs back as
 * soon as it is released. In the enhanced mode, while such a screen's hook runs, Left/Right step
 * through the profile's wheel positions (one inside each choice's zone, from left to right) and
 * the frontend holds the wheel there: the game itself still makes the choice, with its sounds
 * and animations. Each screen starts at the position nearest the centre, as the wheel at rest. */
typedef struct { const char *hook; int count; double pos[8]; } WheelSelect;
static const WheelSelect k_wsel[] = GAME_ENH_WHEEL_SELECTS;
static volatile uint64_t g_wsel_frame;      /* the frame at which a wheel select's hook last ran */
static volatile int g_wsel, g_wsel_idx;     /* which one (k_wsel), the chosen position */

int enh_wheel_select_active(void) {
    return g_enhanced && g_wsel_frame && g_frame - g_wsel_frame <= NAME_GRACE_FRAMES;
}

void enh_wheel_select_step(int dir) {
    int i = g_wsel_idx + (dir < 0 ? -1 : 1);
    if (enh_wheel_select_active() && i >= 0 && i < k_wsel[g_wsel].count) g_wsel_idx = i;
}

double enh_wheel_select_pos(void) { return k_wsel[g_wsel].pos[g_wsel_idx]; }

static void wheel_select_hook(int w) {
    if (!enh_wheel_select_active() || g_wsel != w) {      /* a new screen: the wheel at rest */
        const WheelSelect *s = &k_wsel[w];
        int c = 0;
        for (int i = 1; i < s->count; i++)
            if (fabs(s->pos[i]) < fabs(s->pos[c])) c = i;
        g_wsel_idx = c;
        g_wsel = w;
        if (g_enh_log) rt_log("enhanced: %s\n", s->hook);
    }
    g_wsel_frame = g_frame;
}

/* ================================================================== live NETWORK ID change */
/* Link play (net.c) gives a node its NETWORK ID, and takes it back to 1, without a restart, the
 * way the game's own TEST MODE item does: the ID bits of the NVRAM option block change, in the
 * NVRAM and in the game's RAM copy of it (both checksums fixed: GTI Club 2 compares them in the
 * attract mode), then the game's set-ID routine (network.hot.set_id, argument ID - 1) updates
 * its copy and has the link task recompute its slot. It runs from the net_set_id hook, at the
 * entry of the per-frame packet send (a game task, between calls), in the attract mode only;
 * the registers are kept as they were. */
static void menu_to_main(void);

static void options_ram_fix(uint32_t base) {
    uint32_t sum = 0;
    for (int o = GAME_NVRAM_OPT_START; o < GAME_NVRAM_OPT_CSUM; o += 2) sum += LD16(base + (uint32_t)o);
    uint16_t cs = (uint16_t)(0xffff - sum);
    ST8(base + GAME_NVRAM_OPT_CSUM, cs >> 8);
    ST8(base + GAME_NVRAM_OPT_CSUM + 1, cs & 0xff);
}

static void net_set_id_hook(PPCContext *c) {
    int id = net_hot_target();
    if (!id || !GAME_NET_HOT_SET_ID || !GAME_NETWORK_ID_ADDR || !enh_lobby_ready()) return;
    uint8_t *nv = hw_nvram();
    uint32_t ram = GAME_NET_HOT_OPTIONS_RAM + GAME_NETWORK_ID_ADDR;
    nv[GAME_NETWORK_ID_ADDR] = (uint8_t)((nv[GAME_NETWORK_ID_ADDR] & 0x3f) | (id - 1) << 6);
    hw_nvram_options_fix(nv);
    ST8(ram, (LD8(ram) & 0x3f) | (uint32_t)(id - 1) << 6);
    options_ram_fix(GAME_NET_HOT_OPTIONS_RAM);
    PPCContext saved = *c;
    c->r[3] = (uint32_t)(id - 1);
    rt_call(c, GAME_NET_HOT_SET_ID);
    *c = saved;
    rt_log("enhanced: NETWORK ID %d set live\n", id);
    net_hot_switched(id);
    if (id == 1) menu_to_main();                 /* out of the session: the main menu */
}

typedef struct { uint32_t addr; const char *name; } Hook;
static const Hook k_hooks[] = GAME_ENH_HOOKS;
enum { HOOK_NONE, HOOK_ATTRACT, HOOK_PROJECTION, HOOK_VIEWPORT, HOOK_NAME_INDEX, HOOK_NAME_CONFIRM, HOOK_WHEEL_SELECT, HOOK_NET_SET_ID };
#define NHOOKS (sizeof k_hooks / sizeof k_hooks[0])

/* the kind of the hook at pc, and for a wheel select its entry in k_wsel (*arg); the names are
 * resolved once, so a call only compares addresses */
static int hook_kind(uint32_t pc, int *arg) {
    static signed char kind[NHOOKS], karg[NHOOKS];
    static int resolved;
    if (!resolved) {
        static const char *const names[] = { "", "attract", "projection", "viewport", "name_index", "name_confirm",
                                             "", "net_set_id" };
        for (size_t i = 0; k_hooks[i].name; i++) {
            for (int k = 1; k < (int)(sizeof names / sizeof names[0]); k++)
                if (!strcmp(k_hooks[i].name, names[k])) kind[i] = (signed char)k;
            for (int w = 0; k_wsel[w].hook; w++)
                if (!strcmp(k_hooks[i].name, k_wsel[w].hook)) { kind[i] = HOOK_WHEEL_SELECT; karg[i] = (signed char)w; }
        }
        resolved = 1;
    }
    for (size_t i = 0; k_hooks[i].name; i++)
        if (k_hooks[i].addr == pc) { *arg = karg[i]; return kind[i]; }
    return HOOK_NONE;
}

void rt_hook(PPCContext *c, uint32_t pc) {
    int arg = 0;
    switch (hook_kind(pc, &arg)) {
    case HOOK_NAME_INDEX: if (g_enhanced) name_index_hook(c); break;
    case HOOK_NAME_CONFIRM: if (g_enhanced) name_confirm_hook(c); break;
    case HOOK_WHEEL_SELECT: if (g_enhanced) wheel_select_hook(arg); break;
    case HOOK_NET_SET_ID: if (g_enhanced) net_set_id_hook(c); break;
    case HOOK_ATTRACT: g_attract_frame = g_frame ? g_frame : 1; break;
    case HOOK_PROJECTION: {                  /* the current slot has just been written */
        uint32_t s = LD8(GAME_ENH_WIDE_PROJ_SLOT);
        if (!g_enhanced || s > 1) break;
        g_slot_k[s] = 1.0;
        if (g_wide_k != 1.0) { widen_slot((int)s, g_wide_k); g_slot_k[s] = g_wide_k; }
        break;
    }
    case HOOK_VIEWPORT:
        if (!g_enhanced) break;
        g_vp_k = g_wide_k;
        if (g_wide_k != 1.0) STF32(GAME_ENH_WIDE_VIEWPORT, LDF32(GAME_ENH_WIDE_VIEWPORT) * g_wide_k);
        break;
    default: break;
    }
}

typedef struct { uint32_t addr; const char *text; } BlankString;
static const BlankString k_blank[] = GAME_ENH_BLANK_STRINGS;

/* RT_ENH_BLANK="addr:text,addr:text" adds strings at run time (for finding new ones) */
static BlankString g_extra[16];
static int g_nextra = -1;

static void parse_extra(void) {
    g_nextra = 0;
    const char *e = getenv("RT_ENH_BLANK");
    while (e && *e && g_nextra < 16) {
        char *colon;
        uint32_t addr = (uint32_t)strtoul(e, &colon, 16);
        if (*colon != ':') break;
        const char *text = colon + 1, *end = strchr(text, ',');
        size_t n = end ? (size_t)(end - text) : strlen(text);
        char *copy = malloc(n + 1);
        memcpy(copy, text, n);
        copy[n] = 0;
        g_extra[g_nextra++] = (BlankString){ addr, copy };
        e = end ? end + 1 : text + n;
    }
}

static void blank(const BlankString *b) {
    size_t n = strlen(b->text);
    if (!n || b->addr + n >= RAM_SIZE) return;
    if (memcmp(g_ram + b->addr, b->text, n) == 0) g_ram[b->addr] = 0;
}

/* Text textures with typos (Thrill Drive 2's Italian "per Tasmissone Maniale"): the profile's
 * text_fixes rebuild a band of rows of the texture in VRAM from columns of the same band (and an
 * n turned upside down for a u). A band is recognised by the CRC-32 of its original pixels,
 * checked every 16 frames: the textures are loaded once, at boot, and a patched band no longer
 * matches. Only in the enhanced mode. */
typedef struct { int16_t x0, x1, y0, y1; } FixSpan;     /* y0 < 0: plain columns x0..x1 */
typedef struct { uint32_t addr, stride; int y0, y1; uint32_t crc; int fit0, fit1, nspans; FixSpan span[24]; } TextFix;
static const TextFix k_text_fix[] = GAME_ENH_TEXT_FIXES;

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xffffffffu;
    while (n--) c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void text_fix_tick(void) {
    if (!k_text_fix[0].addr || g_frame % 16) return;
    uint32_t size;
    const uint8_t *vram = voodoo_vram(&size);
    for (const TextFix *f = k_text_fix; f->addr; f++) {
        size_t band = (size_t)(f->y1 - f->y0) * f->stride;
        if (!vram || f->addr + (size_t)f->y1 * f->stride > size) continue;
        if (crc32_buf(vram + f->addr + (size_t)f->y0 * f->stride, band) != f->crc) continue;
        enum { MAXW = 512, MAXH = 64 };
        static uint8_t src[MAXH * 256], line[MAXH * MAXW];
        if (f->stride > 256 || f->y1 - f->y0 > MAXH) continue;
        uint8_t *dst = voodoo_vram_for_write() + f->addr + (size_t)f->y0 * f->stride;
        memcpy(src, dst, band);
        memset(line, 0, sizeof line);
        int x = 0;                               /* the new line, left to right */
        for (int i = 0; i < f->nspans; i++) {
            const FixSpan *s = &f->span[i];
            int w = s->x1 - s->x0;
            if (x + w > MAXW) break;
            for (int y = 0; y < f->y1 - f->y0; y++) {
                uint8_t *d = line + (size_t)y * MAXW + x;
                if (s->y0 < 0) memcpy(d, src + (size_t)y * f->stride + s->x0, (size_t)w);
                else if (y + f->y0 >= s->y0 && y + f->y0 < s->y1)   /* turned by 180 degrees in its rectangle */
                    for (int k = 0; k < w; k++)
                        d[k] = src[(size_t)(s->y1 - 1 - (y + f->y0) + s->y0 - f->y0) * f->stride + s->x1 - 1 - k];
            }
            x += w;
        }
        /* into columns fit0..fit1, where the game draws the line from: squeezed (linearly) if wider */
        int fw = f->fit1 - f->fit0;
        memset(dst, 0, band);
        for (int j = 0; j < fw && j < x; j++)
            for (int y = 0; y < f->y1 - f->y0; y++) {
                const uint8_t *l = line + (size_t)y * MAXW;
                uint8_t *d = dst + (size_t)y * f->stride + f->fit0 + j;
                if (x <= fw) { *d = l[j]; continue; }
                int p = (int)(((2 * j + 1) * x - fw) * 128 / fw);      /* source position, 1/256 column */
                int c = p < 0 ? 0 : p >> 8, t = p < 0 ? 0 : p & 255;
                *d = (uint8_t)((l[c] * (256 - t) + (c + 1 < x ? l[c + 1] : 0) * t) >> 8);
            }
        if (g_enh_log) rt_log("enhanced: text texture fixed at %06x\n", f->addr);
    }
}

/* RT_NVRAM_POKE="seconds:addr=value,..." writes NVRAM bytes (and fixes the checksum) at run time */
static void nvram_poke_tick(void) {
    static const char *next = (const char *)-1;
    if (next == (const char *)-1) next = getenv("RT_NVRAM_POKE");
    while (next && *next) {
        char *p;
        double t = strtod(next, &p);
        if (*p != ':' || (double)rt_now() / CPU_HZ < t) return;
        uint32_t addr = (uint32_t)strtoul(p + 1, &p, 16);
        uint32_t val = *p == '=' ? (uint32_t)strtoul(p + 1, &p, 16) : 0;
        if (addr < 0x1ff0) { hw_nvram()[addr] = (uint8_t)val; hw_nvram_options_fix(hw_nvram()); rt_log("enhanced: NVRAM %04x = %02x\n", addr, val); }
        next = *p == ',' ? p + 1 : NULL;
    }
}

static void menu_tick(void);
static void net_tick(void);
static void scripted_menu(void);
static void fps_tick(const uint32_t *buf, int w, int h);
static int count_game_options(void);

/* called at every published frame, on the guest thread */
void enh_on_frame(const uint32_t *buf, int w, int h) {
    if (!g_enhanced) return;
    g_frame++;
    fps_tick(buf, w, h);
    if (g_enh_log < 0) g_enh_log = getenv("RT_ENH_LOG") != NULL;
    int attract = g_attract_frame && g_frame - g_attract_frame <= ATTRACT_GRACE_FRAMES;
    if (attract != g_attract && g_enh_log) rt_log("enhanced: %s\n", attract ? "attract mode" : "game in progress");
    g_attract = attract;
    wide_tick(g_set.aspect);
    menu_tick();
    net_tick();
    scripted_menu();
    nvram_poke_tick();
    text_fix_tick();
    if (g_nextra < 0) parse_extra();
    for (const BlankString *b = k_blank; b->text; b++) blank(b);
    for (int i = 0; i < g_nextra; i++) blank(&g_extra[i]);
}

/* ================================================================== game font (A8 texture) */
/* The pages (A8 textures of one game file) are stacked into one atlas. Each size is a grid of
 * fixed cells with one entry per row of characters (a space marks an unused cell); the glyphs
 * themselves are proportional, so each one gets its ink width. Sizes are drawn at their scale,
 * sampling the atlas bilinearly (Thrill Drive 2 has a single size, drawn at three scales). */
typedef struct { int cw, ch; float scale; } FontSize;
typedef struct { int size, y; const char *chars; } FontRow;
static const FontSize k_font_sizes[] = GAME_ENH_FONT_SIZES;
static const FontRow k_font_rows[] = GAME_ENH_FONT_ROWS;
static const uint32_t k_font_pages[] = GAME_ENH_FONT_PAGES;
enum { FONT_LARGE, FONT_MEDIUM, FONT_SMALL, FONT_NSIZES };

typedef struct { int16_t x, y, w, h, twin; } Glyph;  /* ink bounds in the atlas; w = 0: missing;
                                                       twin: drawn again that many cell rows higher */
static uint8_t *g_font;                             /* GAME_ENH_FONT_W x (pages * PAGE_H) alpha */
static int g_font_h;
static Glyph g_glyph[FONT_NSIZES][128];

/* ================================================================== port settings */
/* <binary>_enhanced_settings.ini next to the executable (classic mode: <binary>_settings.ini,
 * the window only): options of the port itself (not of the game, which keeps its own in the
 * NVRAM). One "key = value" per line. */
void voodoo_set_scale(int n);
void voodoo_set_texture_filter(int mode);
static char g_settings_path[1024];
static const char *const k_win_key[4] = { "window_x", "window_y", "window_width", "window_height" };

static void settings_load(void) {
    FILE *f = fopen(g_settings_path, "r");
    if (!f) return;
    char line[256], key[64];
    int v;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " %63[a-z_] = %d", key, &v) == 2) {
            if (!strcmp(key, "fullscreen")) g_set.fullscreen = v != 0;
            else if (!strcmp(key, "texture_filter")) g_set.texture_filter = v == 1;
            else if (!strcmp(key, "show_fps")) g_set.show_fps = v != 0;
            else if (!strcmp(key, "render_scale")) g_set.scale = v < 1 ? 1 : v > 2 ? 2 : v;
            else if (!strcmp(key, "aspect")) g_set.aspect = v < 0 || v >= N_ASPECTS ? 0 : v;
            else for (int i = 0; i < 4; i++) if (!strcmp(key, k_win_key[i])) g_set.win[i] = v;
        }
    fclose(f);
}

static void settings_save(void) {
    FILE *f = fopen(g_settings_path, "w");
    if (!f) { rt_log("enhanced: cannot write %s\n", g_settings_path); return; }
    fprintf(f, "# " GAME_TITLE "%s: port settings\n", g_enhanced ? ", enhanced mode" : "");
    if (g_enhanced) {
        fprintf(f, "fullscreen = %d\nshow_fps = %d\nrender_scale = %d\n", g_set.fullscreen, g_set.show_fps, g_set.scale);
        fprintf(f, "# 0 = 4:3, 1 = 16:10, 2 = 16:9, 3 = 21:9\naspect = %d\n", g_set.aspect);
        fprintf(f, "texture_filter = %d\n", g_set.texture_filter);
    }
    if (g_set.win[2])
        for (int i = 0; i < 4; i++) fprintf(f, "%s = %d\n", k_win_key[i], g_set.win[i]);
    fclose(f);
}

int enh_texture_filter(void) { return g_enhanced ? g_set.texture_filter : 0; }
int enh_want_fullscreen(void) { return g_enhanced && g_set.fullscreen; }
void enh_set_fullscreen(int on) { if (g_enhanced && g_set.fullscreen != !!on) { g_set.fullscreen = !!on; settings_save(); } }

/* the window (both modes): x, y, width, height of the last normal window, 0 if none saved */
int enh_want_window(int *r) {
    const int *v = g_set.win;
    if (v[2] < 320 || v[3] < 240 || v[2] > 16384 || v[3] > 16384 ||
        abs(v[0]) > 1000000 || abs(v[1]) > 1000000) return 0;
    memcpy(r, v, sizeof g_set.win);
    return 1;
}
void enh_set_window(const int *r) {
    if (!memcmp(g_set.win, r, sizeof g_set.win)) return;
    memcpy(g_set.win, r, sizeof g_set.win);
    settings_save();
}

void enh_init(const char *work, const char *settings) {
    snprintf(g_settings_path, sizeof g_settings_path, "%s", settings);
    settings_load();                     /* also in classic mode: its own file, the window */
    if (!g_enhanced) return;
    count_game_options();
    voodoo_set_scale(g_set.scale);       /* the only place the render scale is set */
    set_aspect(g_set.aspect);
    voodoo_set_texture_filter(enh_texture_filter());
    const char *file = GAME_ENH_FONT_FILE;
    if (!file) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/fs/%s", work, file);
    int npages = (int)(sizeof k_font_pages / sizeof k_font_pages[0]);
    size_t page = (size_t)GAME_ENH_FONT_W * GAME_ENH_FONT_PAGE_H;
    uint8_t *atlas = malloc(page * npages);
    FILE *f = fopen(path, "rb");
    int ok = f != NULL;
    for (int i = 0; ok && i < npages; i++)
        ok = !fseek(f, (long)k_font_pages[i], SEEK_SET) && fread(atlas + page * i, 1, page, f) == page;
    if (f) fclose(f);
    if (!ok) { rt_log("enhanced: cannot read the menu font from %s\n", path); free(atlas); return; }
    g_font_h = GAME_ENH_FONT_PAGE_H * npages;
    for (const FontRow *r = k_font_rows; r->size >= 0; r++) {
        const FontSize *fs = &k_font_sizes[r->size];
        int col = 0;
        for (const char *p = r->chars; *p; p++, col++) {
            if (*p == ' ' || (unsigned char)*p >= 128) continue;
            int cx = col * fs->cw, lo = fs->cw, hi = -1;
            for (int x = 0; x < fs->cw && cx + x < GAME_ENH_FONT_W; x++)
                for (int y = 0; y < fs->ch && r->y + y < g_font_h; y++)
                    if (atlas[(r->y + y) * GAME_ENH_FONT_W + cx + x] > 40) { if (x < lo) lo = x; if (x > hi) hi = x; break; }
            if (hi >= lo) g_glyph[r->size][(unsigned char)*p] = (Glyph){ (int16_t)(cx + lo), (int16_t)r->y, (int16_t)(hi - lo + 1), (int16_t)fs->ch, 0 };
        }
    }
    for (int z = 0; z < FONT_NSIZES; z++)      /* no colon (Thrill Drive 2): two full stops */
        if (!g_glyph[z][':'].w && g_glyph[z]['.'].w) {
            g_glyph[z][':'] = g_glyph[z]['.'];
            g_glyph[z][':'].twin = (int16_t)(k_font_sizes[z].ch * 3 / 8);
        }
    g_font = atlas;
}

static int font_px(int z, int v) { return (int)(v * k_font_sizes[z].scale + 0.5f); }
static int font_height(int z) { return font_px(z, k_font_sizes[z].ch); }
static int glyph_space(int z) { return font_px(z, k_font_sizes[z].cw / 2); }
static int glyph_gap(int z) { return font_px(z, k_font_sizes[z].cw / 8 + 1); }
static int glyph_width(int z, const Glyph *g) { return font_px(z, g->w); }

static int text_width(int z, const char *s) {
    int w = 0;
    for (; *s; s++) {
        const Glyph *g = &g_glyph[z][(unsigned char)*s & 127];
        w += g->w ? glyph_width(z, g) + glyph_gap(z) : glyph_space(z);
    }
    return w;
}

static void blend(uint32_t *px, uint32_t rgb, int a) {
    uint32_t d = *px;
    int r = (((rgb >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    int g = (((rgb >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    int b = ((rgb & 255) * a + (d & 255) * (255 - a)) / 255;
    *px = 0xff000000u | (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)b;
}

/* alpha of glyph g at (u, v) in glyph pixels, bilinear */
static int glyph_alpha(const Glyph *g, float u, float v) {
    int x0 = (int)floorf(u), y0 = (int)floorf(v);   /* floor: u, v start at -1/4 at 2X */
    float fx = u - x0, fy = v - y0;
    int a[4];
    for (int k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1);
        a[k] = (x < 0 || y < 0 || x >= g->w || y >= g->h) ? 0 : g_font[(g->y + y) * GAME_ENH_FONT_W + g->x + x];
    }
    return (int)((a[0] * (1 - fx) + a[1] * fx) * (1 - fy) + (a[2] * (1 - fx) + a[3] * fx) * fy + 0.5f);
}

/* The overlay is laid out in logical units, 384 lines high (512 wide, or wider in widescreen);
 * on a scaled frame (resolution option) the primitives map them to the real pixels, g_ui real
 * pixels per logical one. */
static float g_ui = 1.0f;
static int g_fbw, g_fbh;

static void draw_text(uint32_t *fb, int w, int h, int z, int x, int y, const char *s, uint32_t rgb) {
    (void)w; (void)h;
    float inv = 1.0f / (k_font_sizes[z].scale * g_ui);
    float fx = x * g_ui;
    int py0 = (int)(y * g_ui + 0.5f);
    for (; *s; s++) {
        const Glyph *g = &g_glyph[z][(unsigned char)*s & 127];
        if (!g->w) { fx += glyph_space(z) * g_ui; continue; }
        int gw = (int)(glyph_width(z, g) * g_ui + 0.5f), gh = (int)(font_px(z, g->h) * g_ui + 0.5f);
        int px0 = (int)(fx + 0.5f), off0 = (int)(2 * g_ui + 0.5f);
        for (int pass = 0; pass < 4; pass++) {      /* drop shadow, then the glyph (and its twin) */
            int off = pass & 1 ? 0 : off0, up = pass & 2 ? (int)(font_px(z, g->twin) * g_ui + 0.5f) : 0;
            uint32_t col = pass & 1 ? rgb : 0x000000;
            if (up == 0 && pass >= 2) continue;
            for (int gy = 0; gy < gh; gy++) {
                int py = py0 + gy + off - up;
                if (py < 0 || py >= g_fbh) continue;
                for (int gx = 0; gx < gw; gx++) {
                    int px = px0 + gx + off;
                    if (px < 0 || px >= g_fbw) continue;
                    int a = inv == 1.0f ? g_font[(g->y + gy) * GAME_ENH_FONT_W + g->x + gx]
                                        : glyph_alpha(g, (gx + 0.5f) * inv - 0.5f, (gy + 0.5f) * inv - 0.5f);
                    if (!(pass & 1)) a = a * 3 / 5;
                    if (a) blend(&fb[py * g_fbw + px], col, a);
                }
            }
        }
        fx += (glyph_width(z, g) + glyph_gap(z)) * g_ui;
    }
}

static void draw_centered(uint32_t *fb, int w, int h, int z, int y, const char *s, uint32_t rgb) {
    draw_text(fb, w, h, z, (w - text_width(z, s)) / 2, y, s, rgb);
}

static void dim_rect(uint32_t *fb, int w, int h, int x0, int y0, int x1, int y1, int a) {
    (void)w; (void)h;
    int X0 = (int)(x0 * g_ui), Y0 = (int)(y0 * g_ui), X1 = (int)(x1 * g_ui), Y1 = (int)(y1 * g_ui);
    for (int y = Y0 < 0 ? 0 : Y0; y < Y1 && y < g_fbh; y++)
        for (int x = X0 < 0 ? 0 : X0; x < X1 && x < g_fbw; x++) blend(&fb[y * g_fbw + x], 0x000000, a);
}

/* ================================================================== fps counter */
/* frames the game drew (Voodoo buffer swaps) per real second: 30 on these games at full speed,
 * fewer on a host too slow for it (per emulated second it would always read 30). Distinct
 * pictures would undercount static screens and slow fades. */
static volatile int g_fps;
unsigned long long voodoo_swap_count(void);

static void fps_tick(const uint32_t *buf, int w, int h) {
    (void)buf; (void)w; (void)h;
    static unsigned long long last_swaps;
    static double t0 = -1, prev;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (t0 < 0 || t - prev > 1.0) {                 /* start, or no frame for a second (pause) */
        t0 = t;
        last_swaps = voodoo_swap_count();
    } else if (t - t0 >= 1.0) {
        unsigned long long n = voodoo_swap_count();
        g_fps = (int)((double)(n - last_swaps) / (t - t0) + 0.5);
        last_swaps = n;
        t0 = t;
    }
    prev = t;
}

/* ================================================================== game options (NVRAM fields) */
/* The TEST MODE settings the OPTIONS pages edit: a field of `bits` at `shift` in the byte (size 1)
 * or big-endian word (size 2) at `addr` of the option block. Values run from min to max; `values`
 * holds "english|italian" labels, one per line (NULL: shown as numbers). */
typedef struct {
    int page; const char *label_en, *label_it; uint32_t addr; int size, shift, bits, min, max, language;
    const char *values;
} GameOption;
static const GameOption k_game_options[] = GAME_ENH_GAME_OPTIONS;
#define MAX_GAME_OPTIONS 16
static int g_n_game_options;
static volatile int g_opt_value[MAX_GAME_OPTIONS];   /* staged values, edited by the menu */
static volatile int g_opt_dirty;

static int count_game_options(void) {
    while (g_n_game_options < MAX_GAME_OPTIONS && k_game_options[g_n_game_options].page >= 0) g_n_game_options++;
    return g_n_game_options;
}

static uint32_t field_get(const uint8_t *nv, const GameOption *o) {
    uint32_t v = o->size == 2 ? (uint32_t)(nv[o->addr] << 8 | nv[o->addr + 1]) : nv[o->addr];
    return (v >> o->shift) & ((1u << o->bits) - 1);
}

static void field_set(uint8_t *nv, const GameOption *o, uint32_t val) {
    uint32_t mask = ((1u << o->bits) - 1) << o->shift;
    uint32_t v = o->size == 2 ? (uint32_t)(nv[o->addr] << 8 | nv[o->addr + 1]) : nv[o->addr];
    v = (v & ~mask) | ((val << o->shift) & mask);
    if (o->size == 2) { nv[o->addr] = (uint8_t)(v >> 8); nv[o->addr + 1] = (uint8_t)v; }
    else nv[o->addr] = (uint8_t)v;
}

/* the label of value v in language lang (0 English, 1 Italian) */
static const char *option_value_text(const GameOption *o, int v, int lang, char *buf, size_t n) {
    if (!o->values) { snprintf(buf, n, "%d", v); return buf; }
    const char *p = o->values;
    for (int i = o->min; i < v && p; i++) { p = strchr(p, '\n'); if (p) p++; }
    if (!p) { snprintf(buf, n, "%d", v); return buf; }
    const char *bar = strchr(p, '|'), *end = strchr(p, '\n');
    if (!end) end = p + strlen(p);
    const char *s0 = lang && bar && bar < end ? bar + 1 : p, *s1 = lang || !bar || bar > end ? end : bar;
    snprintf(buf, n, "%.*s", (int)(s1 - s0), s0);
    return buf;
}

static void options_read(void) {
    const uint8_t *nv = hw_nvram();
    for (int i = 0; i < g_n_game_options; i++) {
        int v = (int)field_get(nv, &k_game_options[i]);
        if (v < k_game_options[i].min || v > k_game_options[i].max) v = k_game_options[i].min;
        g_opt_value[i] = v;
    }
    g_opt_dirty = 0;
}

/* ================================================================== texts (English / Italian) */
/* The menus follow the game's language option (value 2 = Italian). The fonts have no accented
 * letters, so the Italian texts avoid them. */
enum { T_START, T_OPTIONS, T_CREDITS, T_QUIT, T_GAME, T_SOUND, T_DISPLAY, T_BACK, T_WINDOW, T_FULLSCREEN,
       T_SHOW_FPS, T_OFF, T_ON, T_LOADING, T_APPLYING, T_ORIGINAL_GAME, T_RECOMPILATION, T_VOODOO,
       T_PRESS_START_BACK, T_PAUSE, T_RESUME, T_MAIN_MENU, T_RESOLUTION, T_ASPECT, T_TEXTURE_FILTER,
       T_MULTIPLAYER, T_HOST, T_JOIN, T_CLOSE_SESSION, T_LEAVE_SESSION, T_SESSION_CODE, T_SHARE, T_OPENING,
       T_PORT_FAIL, T_PORT_HAND, T_BEHIND_NAT, T_ENTER_CODE, T_TO_JOIN, T_TO_PASTE, T_JOINING, T_FULL,
       T_NO_ANSWER, T_CLOSED, T_BAD_CODE, T_PLAYER, T_YOU, T_FREE, T_LINKED, T_LOST, T_ARRIVING, T_IN_SESSION,
       T_COUNT };
static const char *const k_text[T_COUNT][2] = {
    { "START GAME", "INIZIA PARTITA" }, { "OPTIONS", "OPZIONI" }, { "CREDITS", "RICONOSCIMENTI" },
    { "QUIT", "ESCI" }, { "GAME", "GIOCO" }, { "SOUND", "AUDIO" }, { "DISPLAY", "SCHERMO" },
    { "BACK", "INDIETRO" }, { "WINDOW", "FINESTRA" }, { "FULLSCREEN", "SCHERMO INTERO" },
    { "SHOW FPS", "MOSTRA FPS" }, { "OFF", "NO" }, { "ON", "SI" }, { "LOADING", "CARICAMENTO" },
    { "APPLYING SETTINGS", "APPLICAZIONE IMPOSTAZIONI" }, { "ORIGINAL GAME", "GIOCO ORIGINALE" },
    { "STATIC RECOMPILATION", "RICOMPILAZIONE STATICA" }, { "VOODOO GRAPHICS CORE", "GRAFICA VOODOO" },
    { "PRESS START TO GO BACK", "PREMI START PER TORNARE" }, { "PAUSE", "PAUSA" },
    { "RESUME", "RIPRENDI" }, { "MAIN MENU", "MENU PRINCIPALE" }, { "RESOLUTION", "RISOLUZIONE" },
    { "ASPECT RATIO", "FORMATO" },
    { "TEXTURE FILTER", "FILTRO TEXTURE" },
    { "MULTIPLAYER", "MULTIGIOCATORE" }, { "HOST A GAME", "OSPITA UNA PARTITA" },
    { "JOIN A GAME", "UNISCITI A UNA PARTITA" }, { "CLOSE THE SESSION", "CHIUDI LA SESSIONE" },
    { "LEAVE THE SESSION", "ESCI DALLA SESSIONE" }, { "SESSION CODE", "CODICE DELLA SESSIONE" },
    { "SEND IT TO THE OTHER PLAYERS", "MANDALO AGLI ALTRI GIOCATORI" },
    { "OPENING THE PORT ON THE ROUTER", "APERTURA DELLA PORTA SUL ROUTER" },
    { "THE ROUTER DID NOT OPEN THE PORT", "IL ROUTER NON HA APERTO LA PORTA" },
    { "OPEN UDP PORT 24700 BY HAND", "APRI A MANO LA PORTA UDP 24700" },
    { "ANOTHER ROUTER OR THE PROVIDER IS IN BETWEEN", "IN MEZZO CI SONO UN ALTRO ROUTER O IL PROVIDER" },
    { "ENTER THE SESSION CODE", "INSERISCI IL CODICE DELLA SESSIONE" },
    { "ENTER TO JOIN", "INVIO PER UNIRTI" }, { "CTRL V OR CMD V TO PASTE", "CTRL V O CMD V PER INCOLLARE" },
    { "JOINING", "CONNESSIONE IN CORSO" }, { "THE SESSION IS FULL", "LA SESSIONE E COMPLETA" },
    { "NO ANSWER FROM THE HOST", "NESSUNA RISPOSTA DALL HOST" },
    { "THE HOST CLOSED THE SESSION", "LA SESSIONE E STATA CHIUSA" },
    { "INVALID CODE", "CODICE NON VALIDO" }, { "PLAYER", "GIOCATORE" }, { "YOU", "TU" },
    { "FREE", "LIBERO" }, { "LINKED", "COLLEGATO" }, { "LOST", "PERSO" }, { "JOINING", "IN ARRIVO" },
    { "IN THE SESSION AS PLAYER", "IN SESSIONE COME GIOCATORE" },
};
static const char *const k_texture_filter_name[] = { "ORIGINAL", "NEAREST" };
static const char *const k_aspect_name[N_ASPECTS] = { "4:3", "16:10", "16:9", "21:9" };

static int menu_language(void) {
    for (int i = 0; i < g_n_game_options; i++)
        if (k_game_options[i].language) return g_opt_value[i] == 2;
    return 0;
}
#define T(id) k_text[id][menu_language()]

/* ================================================================== attract menu */
/* Shown over the attract mode. The frontend (host main thread) sends the actions and draws
 * the overlay; the guest thread updates the attract state. Plain ints are enough: each field
 * has a single writer. */
enum { SCREEN_MAIN, SCREEN_OPTIONS, SCREEN_PAGE, SCREEN_CREDITS, SCREEN_NET, SCREEN_NET_JOIN };
enum { PAGE_GAME, PAGE_SOUND, PAGE_DISPLAY, N_PAGES };
static const int k_main_items[] = { T_START, T_MULTIPLAYER, T_OPTIONS, T_CREDITS, T_QUIT };
#define N_MAIN_ITEMS 5
#define MENU_GRACE_FRAMES 300      /* the Konami logo gap in the attract loop lasts about 4 s */
#define START_HOLD_FRAMES 12

static volatile int g_screen, g_cursor, g_quit;
static volatile int g_opt_cursor, g_page, g_page_cursor;
static volatile int g_start_hold;          /* frames START is still held for the game */
static volatile int g_starting;            /* START GAME chosen, waiting for the game to begin */
static volatile uint64_t g_starting_frame;
static volatile int g_apply;               /* 1: write the staged options, 2: written, restart */
static volatile int g_paused, g_pause_cursor;
static volatile int g_returning;           /* MAIN MENU from the pause: back to the attract */
static uint64_t g_return_t0;
static volatile int g_booted;              /* the attract hook has run once since the start */
static int g_headless;

void enh_set_headless(int on) { g_headless = on; }

/* until the game reaches the attract mode, and while applying settings, the frontend runs the
 * emulation unpaced and muted behind a LOADING screen */
#define BOOT_TURBO_LIMIT 60          /* emulated seconds: never keep a LOADING screen forever */
int enh_turbo(void) {
#ifdef GAME_ENH_HOOK_ATTRACT
    if (!g_enhanced || !g_font) return 0;
    if (g_apply || g_returning) return 1;
    return !g_booted && rt_now() < (uint64_t)BOOT_TURBO_LIMIT * CPU_HZ;
#else
    return 0;
#endif
}
int enh_restart_requested(void) { return g_apply == 2; }

/* ------------------------------------------------------------------ pause (Esc in play) */
int enh_paused(void) { return g_paused; }
int enh_inputs_owned(void) { return g_returning; }   /* the return script drives IN3/IN4 */

/* Esc from the frontend: 1 if the enhanced mode handled it (pause, or back in a submenu) */
int enh_escape(void) {
    if (!g_enhanced || !g_font || g_apply || g_returning || !g_booted) return 0;
    if (g_paused) { g_paused = 0; return 1; }
    if (enh_menu_active()) {
        if (g_screen == SCREEN_MAIN) return 0;      /* the main menu: Esc quits, as before */
        enh_menu_action(ENH_BACK);
        return 1;
    }
    if (enh_turbo() || g_starting) return 1;
    if (net_active() && !net_session_over()) return 1;   /* link play: no pause, as on linked cabinets */
    g_paused = 1;
    g_pause_cursor = 0;
    return 1;
}

static void pause_action(int action) {
    switch (action) {
    case ENH_UP: case ENH_DOWN: g_pause_cursor ^= 1; break;
    case ENH_BACK: g_paused = 0; break;
    case ENH_OK:
        if (g_pause_cursor == 0) g_paused = 0;
        else if (GAME_ENH_TEST_GAME_MODE >= 0) { g_returning = 1; g_return_t0 = 0; g_paused = 0; }
        else { g_apply = 2; g_paused = 0; }            /* no known route: reboot the game */
        break;
    default: break;
    }
}

int enh_menu_active(void) {
    if (!g_enhanced || !g_font || g_starting || g_apply || g_returning || g_paused || net_game_halted()) return 0;
    return g_attract_frame && g_frame - g_attract_frame <= MENU_GRACE_FRAMES;
}

int enh_start_held(void) { return g_start_hold > 0; }

/* link play: this game can take a START from the lobby (the menu would be up: the attract mode,
 * with the menu's grace over its gaps, not starting, applying or paused) */
int enh_lobby_ready(void) { return g_booted && enh_menu_active(); }
int enh_quit_requested(void) { return g_quit; }

/* the rows of an options page: game options of that page, or the port options (DISPLAY) */
static int page_rows(int page, int *rows) {
    int n = 0;
    if (page == PAGE_DISPLAY) {
        rows[n++] = -1;
        rows[n++] = -3;
        if (GAME_ENH_WIDE_VIEWPORT) rows[n++] = -4;
        rows[n++] = -10;
        rows[n++] = -2;
        return n;
    }
    for (int i = 0; i < g_n_game_options; i++)
        if (k_game_options[i].page == page) rows[n++] = i;
    return n;
}

static void page_change(int row, int dir) {
    if (row == -10) { g_set.texture_filter = !g_set.texture_filter; voodoo_set_texture_filter(g_set.texture_filter); settings_save(); return; }
    if (row == -1) { g_set.fullscreen = !g_set.fullscreen; settings_save(); return; }
    if (row == -2) { g_set.show_fps = !g_set.show_fps; settings_save(); return; }
    if (row == -3) { g_set.scale = g_set.scale == 1 ? 2 : 1; voodoo_set_scale(g_set.scale); settings_save(); return; }
    if (row == -4) { g_set.aspect = (g_set.aspect + dir + N_ASPECTS) % N_ASPECTS; set_aspect(g_set.aspect); settings_save(); return; }
    const GameOption *o = &k_game_options[row];
    int v = g_opt_value[row] + dir, span = o->max - o->min + 1;
    g_opt_value[row] = o->min + ((v - o->min) % span + span) % span;
    g_opt_dirty = 1;
}

static void leave_options(void) {
    g_screen = SCREEN_MAIN;
    if (g_opt_dirty) g_apply = 1;            /* the guest thread writes them and restarts */
}

/* ------------------------------------------------------------------ MULTIPLAYER (net.c) */
/* MULTIPLAYER: host a session (its code to send to the others, the players), join one with its
 * code (typed or pasted; the game then restarts with the ID the host gives). While a session is
 * open this lobby replaces the main menu: START GAME (the race the others join, as on linked
 * cabinets: the host only, with one other player at least; the others' games then press START
 * by themselves) and CLOSE / LEAVE THE SESSION; leaving the lobby (Esc, back) closes or leaves it
 * (a member then restarts with ID 1, back to the main menu).
 * The menu only files requests; the guest thread carries them out (net_tick). */
static volatile int g_net_cursor, g_code_bad;
static char g_code[12];                     /* the code being typed, without spaces */
static volatile int g_code_len;

/* the items of the MULTIPLAYER screen for the state st */
static int net_items(const NetStatus *st, int *items) {
    int n = 0;
    if (st->role == NET_HOST) { items[n++] = T_START; items[n++] = T_CLOSE_SESSION; return n; }   /* the lobby */
    if (st->role == NET_MEMBER) { items[n++] = T_LEAVE_SESSION; return n; }
    if (st->role == NET_OFF) { items[n++] = T_HOST; items[n++] = T_JOIN; }
    items[n++] = T_BACK;
    return n;
}

static void net_menu_action(int action) {
    if (g_screen == SCREEN_NET_JOIN) {
        if (action == ENH_BACK) g_screen = SCREEN_NET;
        return;                                  /* the keys come through enh_code_key */
    }
    NetStatus st;
    net_status(&st);
    int items[4], n = net_items(&st, items);
    if (g_net_cursor >= n) g_net_cursor = 0;
    switch (action) {
    case ENH_UP: g_net_cursor = (g_net_cursor + n - 1) % n; break;
    case ENH_DOWN: g_net_cursor = (g_net_cursor + 1) % n; break;
    case ENH_BACK:
        if (st.role == NET_HOST || st.role == NET_MEMBER) { net_request_stop(); g_net_cursor = 0; }   /* out of the lobby */
        else g_screen = SCREEN_MAIN;
        break;
    case ENH_OK:
        switch (items[g_net_cursor]) {
        case T_START:                           /* the host, with one player linked at least */
            if (net_linked_count() < 1) break;
            net_request_start();
            g_starting = 1; g_starting_frame = g_frame; g_start_hold = START_HOLD_FRAMES;
            break;
        case T_HOST: net_request_host(); g_net_cursor = 0; break;
        case T_JOIN: g_code_len = 0; g_code[0] = 0; g_code_bad = 0; g_screen = SCREEN_NET_JOIN; break;
        case T_CLOSE_SESSION: case T_LEAVE_SESSION: net_request_stop(); g_net_cursor = 0; break;
        default: g_screen = SCREEN_MAIN; break;
        }
        break;
    default: break;
    }
}

static void menu_to_main(void) { g_screen = SCREEN_MAIN; g_cursor = 0; }

int enh_code_entry_active(void) { return enh_menu_active() && g_screen == SCREEN_NET_JOIN; }
int enh_text_input_active(void) { return enh_name_entry_active() || enh_code_entry_active(); }

/* a key of the code entry: a character (typed or pasted), '\b' delete, '\r' join */
/* Ctrl/Cmd+V: the clipboard replaces what was typed */
void enh_code_paste(const char *text) {
    if (!enh_code_entry_active()) return;
    g_code_len = 0; g_code[0] = 0; g_code_bad = 0;
    for (const char *p = text; p && *p; p++) if (*p != '\r' && *p != '\b') enh_code_key((unsigned char)*p);
}

void enh_code_key(int ch) {
    if (!enh_code_entry_active()) return;
    if (ch == '\b') { if (g_code_len) g_code[--g_code_len] = 0; g_code_bad = 0; return; }
    if (ch == '\r') {
        char hp[40];
        if (g_code_len != 7 && g_code_len != 10) return;            /* not typed in full yet */
        if (net_code_decode(g_code, hp, sizeof hp)) { g_code_bad = 1; return; }
        net_request_join(g_code);
        g_screen = SCREEN_NET;
        g_net_cursor = 0;
        return;
    }
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')) || g_code_len >= 10) return;   /* spaces, dashes: skipped */
    g_code[g_code_len] = (char)ch;
    g_code[++g_code_len] = 0;
    g_code_bad = 0;
}

void enh_menu_action(int action) {
    if (g_paused) { pause_action(action); return; }
    if (!enh_menu_active()) return;
    if (g_screen == SCREEN_PAGE) {
        int rows[MAX_GAME_OPTIONS + 2], n = page_rows(g_page, rows);
        switch (action) {
        case ENH_UP: g_page_cursor = (g_page_cursor + n) % (n + 1); break;
        case ENH_DOWN: g_page_cursor = (g_page_cursor + 1) % (n + 1); break;
        case ENH_BACK: g_screen = SCREEN_OPTIONS; break;
        case ENH_LEFT: case ENH_RIGHT: case ENH_OK:
            if (g_page_cursor == n) { if (action == ENH_OK) g_screen = SCREEN_OPTIONS; }
            else page_change(rows[g_page_cursor], action == ENH_LEFT ? -1 : 1);
            break;
        default: break;
        }
        return;
    }
    if (g_screen == SCREEN_OPTIONS) {
        switch (action) {
        case ENH_UP: g_opt_cursor = (g_opt_cursor + N_PAGES) % (N_PAGES + 1); break;
        case ENH_DOWN: g_opt_cursor = (g_opt_cursor + 1) % (N_PAGES + 1); break;
        case ENH_BACK: leave_options(); break;
        case ENH_OK:
            if (g_opt_cursor == N_PAGES) leave_options();
            else { g_page = g_opt_cursor; g_page_cursor = 0; g_screen = SCREEN_PAGE; }
            break;
        default: break;
        }
        return;
    }
    if (g_screen == SCREEN_CREDITS) {
        if (action == ENH_OK || action == ENH_BACK) g_screen = SCREEN_MAIN;
        return;
    }
    if (g_screen == SCREEN_NET || g_screen == SCREEN_NET_JOIN) { net_menu_action(action); return; }
    switch (action) {
    case ENH_UP: g_cursor = (g_cursor + N_MAIN_ITEMS - 1) % N_MAIN_ITEMS; break;
    case ENH_DOWN: g_cursor = (g_cursor + 1) % N_MAIN_ITEMS; break;
    case ENH_OK:
        if (g_cursor == 0) { g_starting = 1; g_starting_frame = g_frame; g_start_hold = START_HOLD_FRAMES; }
        else if (g_cursor == 1) { g_screen = SCREEN_NET; g_net_cursor = 0; }
        else if (g_cursor == 2) { g_screen = SCREEN_OPTIONS; g_opt_cursor = 0; options_read(); }
        else if (g_cursor == 3) g_screen = SCREEN_CREDITS;
        else g_quit = 1;
        break;
    default: break;
    }
}

/* per-frame menu bookkeeping, on the guest thread (called from enh_on_frame) */
extern uint8_t g_in[8];
void nvram_save(void);

static void menu_tick(void) {
    if (g_attract_frame && !g_booted) { g_booted = 1; options_read(); }
    if (g_screen == SCREEN_MAIN && net_active() && !net_session_over() && g_net_id) {
        g_screen = SCREEN_NET;                   /* a session is open: the lobby is the menu */
        g_net_cursor = 0;
    }
    /* START GAME presses START for a few frames (IN3 bit 4, active low); the SDL frontend
     * rewrites IN3 every loop and also honours enh_start_held(), headless runs rely on this */
    if (g_start_hold > 0) {
        g_in[3] &= (uint8_t)~0x10;
        if (--g_start_hold == 0) g_in[3] |= 0x10;
    }
    if (g_starting) {
        /* the game has begun once the attract hook stops; if it keeps running (START ignored,
         * e.g. during the boot screens), give the menu back after a few seconds */
        if (g_frame - g_attract_frame > 60) { g_starting = 0; g_screen = SCREEN_MAIN; g_cursor = 0; g_attract_frame = 0; }
        else if (g_frame - g_starting_frame > 240) g_starting = 0;
    }
    if (g_returning) {
        /* as on a cabinet: TEST opens TEST MODE, GAME MODE leaves it for the attract. All of it
         * runs fast-forwarded behind the loading screen; a reboot is the fallback. */
        uint64_t now = rt_now();
        if (!g_return_t0) { g_return_t0 = now; g_attract_frame = 0; }
        double t = (double)(now - g_return_t0) / CPU_HZ, t_start = 12.0 + GAME_ENH_TEST_GAME_MODE * 0.8 + 1.0;
        uint8_t in3 = 0xff, in4 = 0xff;
        if (t < 0.4) in3 &= (uint8_t)~0x02;                                   /* TEST */
        for (int k = 0; k < GAME_ENH_TEST_GAME_MODE; k++)
            if (t >= 12.0 + k * 0.8 && t < 12.3 + k * 0.8) in4 &= (uint8_t)~0x01;   /* SHIFT UP */
        if (t >= t_start && t < t_start + 0.3) in3 &= (uint8_t)~0x10;          /* START on GAME MODE */
        g_in[3] = in3;
        g_in[4] = in4;
        if (t > t_start + 0.5 && g_attract_frame) {
            g_returning = 0; g_screen = SCREEN_MAIN; g_cursor = 0;
            rt_log("enhanced: back to the attract mode\n");
        } else if (t > 60.0) {
            g_returning = 0; g_apply = 2;
            rt_log("enhanced: the attract mode did not come back, restarting\n");
            if (g_headless) rt_fatal("restart");
        }
    }
    if (g_apply == 1) {
        /* the game reads its settings only at boot: write them, save, then restart the process */
        uint8_t *nv = hw_nvram();
        for (int i = 0; i < g_n_game_options; i++) field_set(nv, &k_game_options[i], (uint32_t)g_opt_value[i]);
        hw_nvram_options_fix(nv);
        nvram_save();
        rt_log("enhanced: settings written, restarting\n");
        g_apply = 2;
        if (g_headless) rt_fatal("restart for the new settings");
    }
}

/* link play (net.c): the lobby's requests; a member joins the race the host starts; a node whose
 * session is over (left, closed by the host, host lost) takes NETWORK ID 1 back live in the
 * attract mode (net_set_id_hook), never in the middle of a race; the host keeps ID 1 and just
 * stops linking. The one restart left: a game stopped on NETWORK ERROR anyway (a fatal, sticky
 * state that waits for an attendant; the drop concealment of net.c is there so that it does not
 * happen), restarted with ID 1 and out of the session. */
static void net_tick(void) {
    net_requests(2);
    if (!net_active() || g_apply || g_returning) return;
    if (net_start_signal() && enh_lobby_ready()) {    /* the host started: join its race */
        net_start_taken();
        g_starting = 1; g_starting_frame = g_frame; g_start_hold = START_HOLD_FRAMES;
        rt_log("enhanced: starting the race of the session\n");
    }
    if (net_game_halted()) {                     /* NETWORK ERROR: the last resort, a restart */
        rt_log("enhanced: the game stopped on a link error, restarting it\n");
        net_shutdown();                          /* LEAVE / CLOSE, the router's port mapping */
        if (g_headless) rt_fatal("restart after a link error");
        g_apply = 2;
        return;
    }
    if (!net_session_over()) return;
    if (g_net_id <= 1) { net_off(); rt_log("enhanced: link play over\n"); return; }
    if (!net_hot_target()) net_hot_request(1);   /* back to ID 1, live, in the attract mode */
}

static void draw_menu(uint32_t *fb, int w, int h);

void enh_draw_overlay(uint32_t *fb, int w, int h) {
    if (!g_enhanced) return;
    g_fbw = w;
    g_fbh = h;
    g_ui = h >= 768 ? (float)h / 384.0f : 1.0f;     /* lay out in 384-high logical units */
    w = (int)(w / g_ui + 0.5f);
    h = (int)(h / g_ui + 0.5f);
    if (enh_turbo()) {                               /* booting or applying: cover it all */
        dim_rect(fb, w, h, 0, 0, w, h, 255);
        draw_centered(fb, w, h, FONT_MEDIUM, h / 2 - font_height(FONT_MEDIUM) / 2, T(g_apply == 1 || g_apply == 2 ? T_APPLYING : T_LOADING), 0xffffff);
        return;
    }
    if (g_paused) {
        const int z = FONT_MEDIUM, step = font_height(z) + 8;
        static const int items[2] = { T_RESUME, T_MAIN_MENU };
        dim_rect(fb, w, h, 0, 0, w, h, 160);
        draw_centered(fb, w, h, FONT_LARGE, h / 2 - 90, T(T_PAUSE), 0xffd800);
        for (int i = 0; i < 2; i++)
            draw_centered(fb, w, h, z, h / 2 - 10 + i * step, T(items[i]), i == g_pause_cursor ? 0xffd800 : 0xffffff);
    }
    if (enh_menu_active()) draw_menu(fb, w, h);
    if (g_set.show_fps && g_font) {                 /* on top of everything, also in play */
        char buf[16];
        snprintf(buf, sizeof buf, "%d FPS", g_fps);
        draw_text(fb, w, h, FONT_SMALL, w - text_width(FONT_SMALL, buf) - 8, 6, buf, 0x40ff40);
    }
}

/* MULTIPLAYER and its code entry, over the dimmed attract */
static void draw_net(uint32_t *fb, int w, int h) {
    const uint32_t white = 0xffffff, yellow = 0xffd800, grey = 0xc0c0c0, red = 0xff7070, green = 0x60ff60;
    const int zs = FONT_SMALL, zm = FONT_MEDIUM, zl = FONT_LARGE;
    NetStatus st;
    net_status(&st);
    dim_rect(fb, w, h, 0, 0, w, h, 200);
    draw_centered(fb, w, h, zl, 24, T(T_MULTIPLAYER), yellow);
    if (g_screen == SCREEN_NET_JOIN) {
        char shown[16];
        int n = 0, len = g_code_len, total = len > 7 ? 10 : 7;
        for (int i = 0; i < total; i++) {               /* typed so far, dots for the rest */
            if (i == total - (total == 7 ? 4 : 5)) shown[n++] = ' ';
            shown[n++] = i < len ? g_code[i] : '.';
        }
        shown[n] = 0;
        draw_centered(fb, w, h, zm, 104, T(T_ENTER_CODE), white);
        draw_centered(fb, w, h, zl, 150, shown, yellow);
        if (g_code_bad) draw_centered(fb, w, h, zm, 206, T(T_BAD_CODE), red);
        draw_centered(fb, w, h, zs, 290, T(T_TO_JOIN), grey);
        draw_centered(fb, w, h, zs, 290 + font_height(zs) + 6, T(T_TO_PASTE), grey);
        return;
    }
    /* top to bottom, each block below the one before it (the fonts differ a lot between games) */
    const int gap = 3;
    int y = 24 + font_height(zl) + 10;
    if (st.role == NET_HOST) {
        if (st.portmap == 2) {
            draw_centered(fb, w, h, zs, y, T(T_SESSION_CODE), grey);
            y += font_height(zs) + gap;
            draw_centered(fb, w, h, zm, y, st.code, yellow);
            y += font_height(zm) + gap;
            draw_centered(fb, w, h, zs, y, T(st.problem[0] ? T_BEHIND_NAT : T_SHARE), st.problem[0] ? red : grey);
            y += font_height(zs);
        } else if (st.portmap == 3) {
            draw_centered(fb, w, h, zs, y, T(T_PORT_FAIL), red);
            y += font_height(zs) + gap;
            draw_centered(fb, w, h, zs, y, T(T_PORT_HAND), grey);
            y += font_height(zs);
        } else {
            draw_centered(fb, w, h, zs, y, T(T_OPENING), white);
            y += font_height(zs);
        }
    } else if (st.role == NET_JOINING || st.role == NET_MEMBER) {
        char buf[64];
        if (st.role == NET_MEMBER) snprintf(buf, sizeof buf, "%s %d", T(T_IN_SESSION), st.id);
        else snprintf(buf, sizeof buf, "%s", T(T_JOINING));
        draw_centered(fb, w, h, zs, y, buf, white);
        y += font_height(zs) + gap;
        draw_centered(fb, w, h, zm, y, st.code, yellow);
        y += font_height(zm);
    } else if (st.result && st.result != NET_RES_LEFT) {
        int msg = st.result == NET_RES_FULL ? T_FULL : st.result == NET_RES_CLOSED ? T_CLOSED :
                  st.result == NET_RES_PORT ? T_PORT_FAIL : st.result == NET_RES_BADCODE ? T_BAD_CODE : T_NO_ANSWER;
        draw_centered(fb, w, h, zs, y, T(msg), red);
        y += font_height(zs);
    }
    y += 10;
    if (st.role == NET_HOST || st.role == NET_MEMBER) {      /* the four cabinets */
        int left = w / 2 - 150, right = w / 2 + 150;
        for (int id = 1; id <= 4; id++, y += font_height(zs) + gap) {
            static const int state_text[] = { T_FREE, T_YOU, T_ARRIVING, T_LINKED, T_LOST };
            int s = st.node[id];
            char buf[32];
            snprintf(buf, sizeof buf, "%s %d", T(T_PLAYER), id);
            uint32_t col = s == NET_NODE_SELF ? yellow : s == NET_NODE_LINKED ? green : s == NET_NODE_LOST ? red : grey;
            draw_text(fb, w, h, zs, left, y, buf, col);
            const char *v = T(state_text[s]);
            draw_text(fb, w, h, zs, right - text_width(zs, v), y, v, col);
        }
        y += 6;
    }
    /* the items at the bottom, never over what is above them */
    int items[4], n = net_items(&st, items), step = font_height(zm) + 2;
    int y0 = h - 10 - n * step;
    if (y0 < y) y0 = y;
    if (g_net_cursor >= n) g_net_cursor = 0;
    for (int i = 0; i < n; i++) {
        int off = items[i] == T_START && net_linked_count() < 1;   /* waiting for a player */
        draw_centered(fb, w, h, zm, y0 + i * step, T(items[i]), off ? 0x707070 : i == g_net_cursor ? yellow : white);
    }
}

static void draw_menu(uint32_t *fb, int w, int h) {
    const uint32_t white = 0xffffff, yellow = 0xffd800, grey = 0xc0c0c0;
    if (g_screen == SCREEN_NET || g_screen == SCREEN_NET_JOIN) { draw_net(fb, w, h); return; }
    int lang = menu_language();
    if (g_screen == SCREEN_MAIN) {
        /* a compact panel on the left, so the attract stays visible */
        const int z = FONT_MEDIUM, step = font_height(z) * 7 / 8, pad = 12, left = 24;
        int tw = 0;
        for (int i = 0; i < N_MAIN_ITEMS; i++) {
            int iw = text_width(z, T(k_main_items[i]));
            if (iw > tw) tw = iw;
        }
        int ph = N_MAIN_ITEMS * step + 2 * pad - (step - font_height(z));
        int y0 = h - ph - 40;          /* lower left */
        dim_rect(fb, w, h, left, y0, left + tw + 2 * pad, y0 + ph, 150);
        for (int i = 0; i < N_MAIN_ITEMS; i++)
            draw_text(fb, w, h, z, left + pad, y0 + pad + i * step, T(k_main_items[i]), i == g_cursor ? yellow : white);
    } else if (g_screen == SCREEN_OPTIONS) {
        static const int items[N_PAGES + 1] = { T_GAME, T_SOUND, T_DISPLAY, T_BACK };
        const int z = FONT_MEDIUM, step = font_height(z) + 8;
        dim_rect(fb, w, h, 0, 0, w, h, 190);
        draw_centered(fb, w, h, FONT_LARGE, 36, T(T_OPTIONS), yellow);
        for (int i = 0; i <= N_PAGES; i++)
            draw_centered(fb, w, h, z, 120 + i * step + (i == N_PAGES ? step / 2 : 0), T(items[i]), i == g_opt_cursor ? yellow : white);
    } else if (g_screen == SCREEN_PAGE) {
        static const int titles[N_PAGES] = { T_GAME, T_SOUND, T_DISPLAY };
        const int z = FONT_MEDIUM, step = font_height(z) + 4, left = 40, right = w - 40;
        int rows[MAX_GAME_OPTIONS + 2], n = page_rows(g_page, rows);
        dim_rect(fb, w, h, 0, 0, w, h, 190);
        draw_centered(fb, w, h, FONT_LARGE, 24, T(titles[g_page]), yellow);
        int y = 90;
        for (int i = 0; i < n; i++, y += step) {
            uint32_t col = i == g_page_cursor ? yellow : white;
            const char *label, *value;
            char buf[64];
            if (rows[i] == -1) { label = T(T_DISPLAY); value = T(g_set.fullscreen ? T_FULLSCREEN : T_WINDOW); }
            else if (rows[i] == -2) { label = T(T_SHOW_FPS); value = T(g_set.show_fps ? T_ON : T_OFF); }
            else if (rows[i] == -3) { label = T(T_RESOLUTION); value = g_set.scale == 2 ? "2X" : "1X"; }
            else if (rows[i] == -10) { label = T(T_TEXTURE_FILTER); value = k_texture_filter_name[g_set.texture_filter]; }
            else if (rows[i] == -4) { label = T(T_ASPECT); value = k_aspect_name[g_set.aspect]; }
            else {
                const GameOption *o = &k_game_options[rows[i]];
                label = lang ? o->label_it : o->label_en;
                value = option_value_text(o, g_opt_value[rows[i]], lang, buf, sizeof buf);
            }
            /* a value that does not fit next to its label falls back to the small font */
            int zv = text_width(z, label) + text_width(z, value) + 16 > right - left ? FONT_SMALL : z;
            draw_text(fb, w, h, z, left, y, label, col);
            draw_text(fb, w, h, zv, right - text_width(zv, value), y + (font_height(z) - font_height(zv)), value, col);
        }
        draw_text(fb, w, h, z, left, y + step / 2, T(T_BACK), g_page_cursor == n ? yellow : white);
    } else {
        static const int heads[] = { T_ORIGINAL_GAME, T_RECOMPILATION, T_VOODOO };
        static const char *const names[] = { "KONAMI", "SPITA90", "MAME" };
        dim_rect(fb, w, h, 0, 0, w, h, 190);
        draw_centered(fb, w, h, FONT_LARGE, 24, T(T_CREDITS), yellow);
        for (int i = 0, y = 90; i < 3; i++, y += 78) {
            draw_centered(fb, w, h, FONT_SMALL, y, T(heads[i]), grey);
            draw_centered(fb, w, h, FONT_MEDIUM, y + 26, names[i], white);
        }
        draw_centered(fb, w, h, FONT_SMALL, 340, T(T_PRESS_START_BACK), grey);
    }
}

/* RT_ENH_MENU="seconds:action,..." (up/down/left/right/ok/back/esc) drives the menu in headless
 * tests; "name=TEXT" types TEXT in the name entry ('<' for DEL, '>' for END), "code=TEXT" in the
 * MULTIPLAYER code entry ('>' joins) */
static void scripted_menu(void) {
    static const char *next = (const char *)-1;
    if (next == (const char *)-1) next = getenv("RT_ENH_MENU");
    while (next && *next) {
        char *colon;
        double t = strtod(next, &colon);
        if (*colon != ':' || (double)rt_now() / CPU_HZ < t) return;
        const char *a = colon + 1;
        if (!strncmp(a, "esc", 3)) { enh_escape(); const char *c = strchr(a, ','); next = c ? c + 1 : NULL; continue; }
        if (!strncmp(a, "name=", 5)) {
            for (a += 5; *a && *a != ','; a++) enh_name_type(*a == '<' ? '\b' : *a == '>' ? '\r' : *a);
            next = *a ? a + 1 : NULL;
            continue;
        }
        if (!strncmp(a, "code=", 5)) {                 /* the MULTIPLAYER code entry; '>' joins */
            for (a += 5; *a && *a != ','; a++) enh_code_key(*a == '>' ? '\r' : *a);
            next = *a ? a + 1 : NULL;
            continue;
        }
        int action = !strncmp(a, "up", 2) ? ENH_UP : !strncmp(a, "down", 4) ? ENH_DOWN :
                     !strncmp(a, "left", 4) ? ENH_LEFT : !strncmp(a, "right", 5) ? ENH_RIGHT :
                     !strncmp(a, "ok", 2) ? ENH_OK : ENH_BACK;
        enh_menu_action(action);
        const char *comma = strchr(a, ',');
        next = comma ? comma + 1 : NULL;
    }
}
