/*
 * EXTRA modes (enhanced mode, main menu EXTRA): alternative rules on top of a normal race.
 * The game runs as always; the modes read and write a few words of its state every frame:
 *   - the own state struct (the link packet, filled every frame also without a link): its flags
 *     word tells the race apart (mode 4, the profile's race substate);
 *   - the player's car object (position, speed);
 *   - the time left on the race clock (writing 0 ends the race as a TIME OVER);
 *   - GTI Club 2's gravity constant (the per-frame vertical acceleration of an airborne car).
 * Where they are comes from the "modes" section of the game profile (GAME_MODE_*). A version
 * without it (not yet verified) offers no EXTRA page.
 *
 * A mode chosen in EXTRA applies to the game it starts and ends with it, in the attract mode.
 * Nothing runs without a mode: modes_tick returns at its first test. Everything here runs on the
 * guest thread except modes_draw (frontend thread), which reads the HUD the tick publishes.
 */
#include "runtime.h"
#include "game_config.h"
#include "modes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if GAME_HAS_MODES

static const char *const k_mode_key[MODE_COUNT] = { "", "gravity", "speed", "bomb", "police", "elimination" };
static const char *const k_mode_text[MODE_COUNT][2][3] = {    /* name, two lines of description */
    { { "", "", "" }, { "", "", "" } },
    { { "GRAVITY", "EVERY 10 SECONDS THE GRAVITY GOES LIGHT OR HEAVY", "FROM 0.1 TO 4 G, AND MOMENTS AT ZERO G" },
      { "GRAVITA", "OGNI 10 SECONDI LA GRAVITA DIVENTA LEGGERA O PESANTE", "DA 0.1 A 4 G, E MOMENTI A ZERO G" } },
    { { "SPEED", "", "" }, { "SPEED", "", "" } },               /* the description: speed_text */
    { { "PASS THE BOMB", "A CAR HAS A BOMB: TOUCH ANOTHER TO PASS IT", "WHEN IT EXPLODES THAT CAR STOPS FOR 4 SECONDS" },
      { "PASSA LA BOMBA", "UNA AUTO HA LA BOMBA: TOCCANE UNA PER PASSARLA", "QUANDO ESPLODE QUELLA AUTO SI FERMA 4 SECONDI" } },
    { { "POLICE", "YOU ARE THE POLICE: THE OTHER CARS ARE ON THE RUN", "STAY RIGHT ON ONE FOR 3 SECONDS TO ARREST IT" },
      { "POLIZIA", "SEI LA POLIZIA: LE ALTRE AUTO SONO IN FUGA", "STAI ATTACCATO A UNA PER 3 SECONDI PER ARRESTARLA" } },
    { { "ELIMINATION", "EVERY 45 SECONDS THE LAST CAR IS OUT", "BE THE LAST ONE LEFT" },
      { "ELIMINAZIONE", "OGNI 45 SECONDI L ULTIMA AUTO E FUORI", "RESTA L ULTIMO IN GARA" } },
};

/* the modes this game offers, in menu order */
static int g_avail[MODE_COUNT], g_navail = -1;

int modes_list(const int **ids) {
    if (g_navail < 0) {
        static const char *const list[] = GAME_MODE_LIST;
        g_navail = 0;
        for (int i = 0; list[i]; i++)
            for (int m = 1; m < MODE_COUNT; m++)
                if (!strcmp(list[i], k_mode_key[m])) g_avail[g_navail++] = m;
    }
    *ids = g_avail;
    return g_navail;
}

static const char *speed_text(int line, int lang);

const char *modes_text(int mode, int line, int lang) {
    if (mode == MODE_SPEED && line > 0) return speed_text(line, lang);
    return mode > 0 && mode < MODE_COUNT && line >= 0 && line < 3 ? k_mode_text[mode][lang ? 1 : 0][line] : "";
}

/* ------------------------------------------------------------------ state */
static volatile int g_selected;            /* menu thread: the mode of the next game */
static int g_mode;                         /* the mode of the game in progress (0: none) */
static int g_racing, g_go;                 /* in the race; its clock has started */
static double g_race_t0, g_go_t, g_now;    /* emulated seconds */
static int32_t g_time_at_race;             /* the clock at the start of the race state */
volatile int g_mode_hold;                  /* pedals released, brake held (hw.c) */
static int g_car_select_seen;              /* frames left of the car select (modes_car_select) */

void modes_select(int mode) { g_selected = mode; }

/* the HUD, published by the tick for modes_draw: two buffers, the reader takes the last full one */
typedef struct {
    int mode;
    char line[2][48];                      /* small, at the top centre */
    uint32_t line_rgb[2];
    char big[32];                          /* large, in the middle (an announcement) */
    uint32_t big_rgb;
    int flash;                             /* a white flash, 0..255 */
} Hud;
static Hud g_hud[2];
static volatile int g_hud_cur;
static Hud *hud_begin(void) { Hud *h = &g_hud[!g_hud_cur]; memset(h, 0, sizeof *h); h->mode = g_mode; return h; }
static void hud_end(void) { g_hud_cur = !g_hud_cur; }

static uint32_t ptr_at(uint32_t slot) {     /* a pointer the game keeps in a TOC slot, or 0 */
    uint32_t p = LD32(slot);
    return p && p < RAM_SIZE - 0x1000 ? p : 0;
}

static int race_now(void) {
    uint32_t st = ptr_at(GAME_MODE_STATE);
    if (!st) return 0;
    uint32_t fl = LD32(st + 4);
    return ((fl >> 27) & 15) == 4 && (int)((fl >> 23) & 15) == GAME_MODE_RACE_SUBSTATE;
}

/* the race clock: a 32-bit word (time_left), or a field of the state struct (time_state: offset,
 * shift, bits); 0 is a TIME OVER */
#ifdef GAME_MODE_TIME_STATE
static const uint32_t k_time_field[3] = GAME_MODE_TIME_STATE;
static int32_t time_left_raw(void) {
    uint32_t st = ptr_at(GAME_MODE_STATE);
    return st ? (int32_t)((LD32(st + k_time_field[0]) >> k_time_field[1]) & ((1u << k_time_field[2]) - 1)) : 0;
}
static void set_time_left(int32_t v) {
    uint32_t st = ptr_at(GAME_MODE_STATE), mask = (1u << k_time_field[2]) - 1;
    if (v < 0) v = 0;
    if ((uint32_t)v > mask) v = (int32_t)mask;
    if (st) ST32(st + k_time_field[0], (LD32(st + k_time_field[0]) & ~(mask << k_time_field[1])) | (uint32_t)v << k_time_field[1]);
}
#else
static int32_t time_left_raw(void) { return (int32_t)LD32(GAME_MODE_TIME_LEFT); }
static void set_time_left(int32_t v) { ST32(GAME_MODE_TIME_LEFT, (uint32_t)(v < 0 ? 0 : v)); }
#endif
static void end_race(void) { set_time_left(0); }

/* a mode with more time (k_time_scale): the clock at the start of the race, and every extension
 * the game adds at a checkpoint, times the factor */
static const double k_time_scale[MODE_COUNT] = { [MODE_GRAVITY] = 1.2, [MODE_BOMB] = 1.2 };
static int32_t g_time_last;
static void time_scale_tick(int racing_now, int race_started) {
    double k = k_time_scale[g_mode];
    if (k <= 1.0 || !racing_now) return;
    int32_t t = time_left_raw();
    if (race_started) t = (int32_t)(t * k);
    else if (t > g_time_last + 30) t += (int32_t)((t - g_time_last) * (k - 1.0));   /* an extension */
    else { g_time_last = t; return; }
    set_time_left(t);
    g_time_last = time_left_raw();
}

static double player_speed_kmh(void) {
    uint32_t p = ptr_at(GAME_MODE_PLAYER);
    return p ? fabs(LDF32(p + GAME_MODE_PLAYER_SPEED)) * 3.6 : 0;
}

static int lang(void) { return enh_lang(); }

/* the modes' log lines, only with RT_ENH_LOG (as the enhanced mode's) */
#define mlog(...) do { static int on_ = -1; if (on_ < 0) on_ = getenv("RT_ENH_LOG") != NULL; if (on_) rt_log(__VA_ARGS__); } while (0)

/* a small deterministic generator (the same sequence for the same seed) */
static uint32_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

/* ------------------------------------------------------------------ the race's cars */
#ifdef GAME_MODE_CARS
#define MAX_CARS 8
static uint32_t car_addr(int k) { return GAME_MODE_CARS + (uint32_t)k * GAME_MODE_CAR_STRIDE; }
static void car_pos(int k, double *p) {
    for (int i = 0; i < 3; i++) p[i] = LDF32(car_addr(k) + GAME_MODE_CAR_POS + 4 * i);
}
static int car_live(int k) {               /* an entry in use: a position that is not all zero */
    double p[3];
    car_pos(k, p);
    return p[0] != 0 || p[2] != 0;
}
/* Holding a CPU car where it is: car_hold_begin records it, car_hold puts it back every frame,
 * the frame before too, with its velocity zeroed. In GTI Club 2 the car array is the car (the
 * game moves it by its velocity). In Thrill Drive 2 a CPU car runs along its path, and the car
 * array only follows: cpu_path is {address of the first CPU car's path record, stride}, cpu_path_s
 * the offsets of its distance along the path (three copies), cpu_path_speed of its speed; the
 * distance is held and the speed zeroed. */
typedef struct { float pos[3], s[4]; } CarHold;
static CarHold g_car_hold[MAX_CARS];
#ifdef GAME_MODE_CPU_PATH
static const uint32_t k_cpu_path[2] = GAME_MODE_CPU_PATH, k_cpu_path_s[] = GAME_MODE_CPU_PATH_S,
                      k_cpu_path_speed[] = GAME_MODE_CPU_PATH_SPEED;
#define N_PATH_S (sizeof k_cpu_path_s / sizeof k_cpu_path_s[0])
#define N_PATH_SPEED (sizeof k_cpu_path_speed / sizeof k_cpu_path_speed[0])
static uint32_t cpu_path(int k) { return k_cpu_path[0] + (uint32_t)(k - 1) * k_cpu_path[1]; }
#endif

static void car_hold_begin(int k) {
    double p[3];
    car_pos(k, p);
    for (int i = 0; i < 3; i++) g_car_hold[k].pos[i] = (float)p[i];
#ifdef GAME_MODE_CPU_PATH
    for (size_t i = 0; i < N_PATH_S && i < 4; i++) g_car_hold[k].s[i] = (float)LDF32(cpu_path(k) + k_cpu_path_s[i]);
#endif
}

static void car_hold(int k) {
    uint32_t a = car_addr(k);
    for (int i = 0; i < 3; i++) {
        STF32(a + GAME_MODE_CAR_POS + 4 * i, g_car_hold[k].pos[i]);
        STF32(a + GAME_MODE_CAR_PREV + 4 * i, g_car_hold[k].pos[i]);
        STF32(a + GAME_MODE_CAR_VEL + 4 * i, 0);
    }
#ifdef GAME_MODE_CPU_PATH
    for (size_t i = 0; i < N_PATH_S && i < 4; i++) STF32(cpu_path(k) + k_cpu_path_s[i], g_car_hold[k].s[i]);
    for (size_t i = 0; i < N_PATH_SPEED; i++) STF32(cpu_path(k) + k_cpu_path_speed[i], 0);
#endif
}
static double car_dist(int a, int b) {
    double p[3], q[3];
    car_pos(a, p);
    car_pos(b, q);
    return sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
}
static const char *car_label(int k);          /* the label on its marker: TOP, 2ND, ..., 2P */
static int car_ahead_in_race(int k);          /* ahead of the player in the race (-1: not known) */
static int car_place(int k);                  /* its place on its marker (0 TOP), -1 not a CPU car's */
static void car_labels_reset(void);           /* a new game: no marker seen yet */

/* car b is ahead of car a (by a's direction of travel) */
static int car_ahead(int a, int b) {
    double p[3], q[3];
    car_pos(a, p);
    car_pos(b, q);
    double fx = p[0] - LDF32(car_addr(a) + GAME_MODE_CAR_PREV), fz = p[2] - LDF32(car_addr(a) + GAME_MODE_CAR_PREV + 8);
    return (q[0] - p[0]) * fx + (q[2] - p[2]) * fz >= 0;
}
#endif


/* ------------------------------------------------------------------ the game's explosion (GTI Club 2) */
/* A car's fire and smoke effect, the one the game shows when a car is hit hard. The routine
 * drawing a CPU car reads a flag word of its draw record and starts the effect (0x68114) when a
 * bit is set, stops it when cleared: the hook car_effect sits at that read (car_effect: the
 * record's register, the flag word's offset, the bit), and for the exploding CPU car (its record:
 * the car array entry's word car_record) sets the bit for FX_TIME. The player's car has no such
 * record: its effect slot (0 of the slots at *effect_slots, 0x28 bytes each) is started by
 * calling effect_start(1, 0, a record) from the hook, and its position written every frame
 * (x, height, z at +4) from the player's car (player_pos), its offset along the car (+0x10,
 * +0x18: k sin, k cos of the heading, k at effect_offset) from the player's heading, its drift
 * (+0x1c..+0x24, taken from that record's velocity) zeroed. */
#define FX_TIME 2.0
static int g_fx_car = -1, g_fx_started;
static double g_fx_t = -100;
static void explode(int k) { g_fx_car = k; g_fx_t = g_now; g_fx_started = 0; }

void modes_car_effect(PPCContext *c) {
#if defined(GAME_MODE_CAR_EFFECT) && defined(GAME_MODE_CARS)
    if (!g_mode || g_fx_car < 0 || g_now - g_fx_t > FX_TIME) return;
    static const uint32_t ce[3] = GAME_MODE_CAR_EFFECT, pp[3] = GAME_MODE_PLAYER_POS;
    uint32_t rec = c->r[ce[0]];
    if (!rec || rec >= RAM_SIZE) return;
    if (g_fx_car > 0) {
        if (rec == LD32(car_addr(g_fx_car) + GAME_MODE_CAR_RECORD)) ST32(rec + ce[1], LD32(rec + ce[1]) | ce[2]);
        return;
    }
    uint32_t slot = ptr_at(GAME_MODE_EFFECT_SLOTS), p = ptr_at(GAME_MODE_PLAYER);
    if (!slot || !p) return;
    if (!g_fx_started) {
        g_fx_started = 1;
        if (!LD8(slot)) {
            PPCContext saved = *c;
            c->r[3] = 1; c->r[4] = 0; c->r[5] = rec;
            rt_call(c, GAME_MODE_EFFECT_START);
            *c = saved;
        }
    }
    for (int i = 0; i < 3; i++) {
        STF32(slot + 4 + 4 * i, LDF32(p + pp[i]));
        STF32(slot + 0x1c + 4 * i, 0);       /* the drift it took from the record it started with */
    }
    /* its offset along the car (0x68cd0: k sin, k cos of the heading), from the player's heading */
    static const uint32_t eo[2] = GAME_MODE_EFFECT_OFFSET;
    uint32_t pool = ptr_at(eo[0]);
    if (pool) {
        double k = LDF32(pool + eo[1]), h = LDF32(p + GAME_MODE_PLAYER_HEADING);
        STF32(slot + 0x10, k * sin(h));
        STF32(slot + 0x18, k * cos(h));
    }
#else
    (void)c;
#endif
}

/* ------------------------------------------------------------------ GRAVITY (GTI Club 2) */
/* The profile's gravity constants, scaled by one factor (1 G = the game's own values): the
 * per-frame acceleration of an airborne car, and the g that the car's grip on the road is
 * divided by (low: it slides, barely accelerates and steers little; high: it sticks and steers
 * sharply). The grip follows the factor on a flatter curve, factor^GRAV_GRIP_EXP, so the
 * steering stays drivable (0.1 -> 0.45, 4 -> 1.62); the flight follows it in full.
 * Every 10 s of race clock a new factor, light (0.1 to 0.5) and heavy (2 to 4) in turn, and one
 * change in three starts with 5 s at "zero G" (then the new factor holds 10 s), where the flight
 * is GRAV_ZERO_FLIGHT, just below zero, and every car leaves the road as it starts, moving up at
 * GRAV_LAUNCH metres per game frame and then faster and faster: the player's car is put in the
 * air (its airborne flag set, no lift: it starts gently), and kept level (in the air the game
 * tips its nose down a little more every frame; the pitch fields are held at 0); a CPU car is
 * lifted GRAV_CPU_LIFT metres, which the game flies; the game puts
 * some back on the road (a slope under them), so a CPU car not in the air is lifted again every
 * GRAV_RELIFT seconds of the zero G, at most GRAV_RELIFTS times (a car standing still is always
 * put back, and would only hop). The player's car drifts up about 10 m in the 5 s.
 * The CPU cars fly by a gravity of their own (cpu_gravity, per game frame squared, subtracted
 * from their vertical velocity car_vy while in the air; their grip is not touched): at each game
 * frame (their position changed) a car whose vertical velocity fell by exactly that much since
 * the last one is in the air, and gets (1 - factor) of it back. The grip takes the factor from
 * 0.1 up: the game divides by it. */
#ifdef GAME_MODE_GRAVITY
#define GRAV_PERIOD 10.0
#define GRAV_ZERO_TIME 5.0
#define GRAV_GRIP_MIN 0.1
#define GRAV_GRIP_EXP 0.35
#define GRAV_ZERO_FLIGHT (-0.04)
#define GRAV_CPU_LIFT 1.0
#define GRAV_LAUNCH 0.01
#define GRAV_RELIFT 0.5
#define GRAV_RELIFTS 3
static const int k_grav_light[] = { 1, 2, 3, 5 }, k_grav_heavy[] = { 20, 25, 30, 40 };   /* tenths */
static const uint32_t k_grav[][2] = GAME_MODE_GRAVITY;     /* {address, the game's value} */
#define N_GRAV (sizeof k_grav / sizeof k_grav[0])
static uint32_t g_grav_written[N_GRAV];
static int g_grav_tenths = 10, g_grav_zero;
static double g_grav_next, g_grav_zero_end, g_grav_shown;

#if defined(GAME_MODE_CPU_GRAVITY) && defined(GAME_MODE_CARS)
static uint32_t g_cpu_ybits[MAX_CARS];
static float g_cpu_vy[MAX_CARS];
static int g_cpu_seen[MAX_CARS];
static double g_cpu_air_t[MAX_CARS];         /* when it was last seen in the air */
static int g_cpu_lifts[MAX_CARS];             /* lifts in this zero G */

static void grav_cpu(double factor) {
    for (int k = 1; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++) {
        uint32_t a = car_addr(k), yb = LD32(a + GAME_MODE_CAR_POS + 4);
        if (yb == g_cpu_ybits[k]) continue;                  /* no new game frame for it yet */
        g_cpu_ybits[k] = yb;
        float vy = (float)LDF32(a + GAME_MODE_CAR_VY);
        if (g_cpu_seen[k] && fabs(vy - g_cpu_vy[k] + GAME_MODE_CPU_GRAVITY) < 0.002) {
            g_cpu_air_t[k] = g_now;
            if (factor != 1.0) {
                vy += (float)((1.0 - factor) * GAME_MODE_CPU_GRAVITY);
                STF32(a + GAME_MODE_CAR_VY, vy);
            }
        }
        g_cpu_vy[k] = vy;
        g_cpu_seen[k] = 1;
    }
}
#else
static void grav_cpu(double factor) { (void)factor; }
#endif

/* zero G: every car off the road, moving upwards (player: at its start; CPU cars: every
 * GRAV_RELIFT s while one is not in the air) */
static void grav_launch(int start) {
#ifdef GAME_MODE_PLAYER_AIR
    uint32_t p = ptr_at(GAME_MODE_PLAYER);
    if (p) {                                 /* level, all along */
        static const uint32_t pitch[] = GAME_MODE_PLAYER_PITCH;
        for (size_t i = 0; i < sizeof pitch / sizeof pitch[0]; i++) STF32(p + pitch[i], 0);
    }
    if (p && start) {
        STF32(p + GAME_MODE_PLAYER_VY, GRAV_LAUNCH);
        STF32(p + GAME_MODE_PLAYER_VY0, GRAV_LAUNCH);
        ST8(p + GAME_MODE_PLAYER_AIR, 1);
        ST16(p + GAME_MODE_PLAYER_AIR_FRAMES, 1);
    }
#endif
#if defined(GAME_MODE_CPU_GRAVITY) && defined(GAME_MODE_CARS)
    static double last;
    if (!start && g_now - last < GRAV_RELIFT) return;
    last = g_now;
    for (int k = 1; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++) {
        if (start) g_cpu_lifts[k] = 0;
        if (!car_live(k) || (!start && g_now - g_cpu_air_t[k] < GRAV_RELIFT) || g_cpu_lifts[k] >= GRAV_RELIFTS) continue;
        g_cpu_lifts[k]++;
        uint32_t a = car_addr(k);
        STF32(a + GAME_MODE_CAR_POS + 4, LDF32(a + GAME_MODE_CAR_POS + 4) + GRAV_CPU_LIFT);
        STF32(a + GAME_MODE_CAR_PREV + 4, LDF32(a + GAME_MODE_CAR_PREV + 4) + GRAV_CPU_LIFT);
        STF32(a + GAME_MODE_CAR_VY, GRAV_LAUNCH);
        g_cpu_seen[k] = 0;                                  /* its velocity jumped: not a fall */
        g_cpu_air_t[k] = g_now;
    }
#endif
    if (start) mlog("modes: zero G, the cars lifted\n");
}

static void grav_write(double factor) {
    for (size_t i = 0; i < N_GRAV; i++) {
        union { float f; uint32_t u; } v = { .u = k_grav[i][1] }, w;
        uint32_t cur = LD32(k_grav[i][0]);
        w.f = (float)(v.f * (i == 0 ? factor : pow(factor < GRAV_GRIP_MIN ? GRAV_GRIP_MIN : factor, GRAV_GRIP_EXP)));
        /* only over the game's value or ours: never over another module loaded there */
        if (cur != k_grav[i][1] && cur != g_grav_written[i]) continue;
        ST32(k_grav[i][0], w.u);
        g_grav_written[i] = w.u;
    }
}

static void grav_restore(void) {
    for (size_t i = 0; i < N_GRAV; i++) {
        if (g_grav_written[i] && LD32(k_grav[i][0]) == g_grav_written[i]) ST32(k_grav[i][0], k_grav[i][1]);
        g_grav_written[i] = 0;
    }
}

static void grav_start(void) {
    g_grav_tenths = 10; g_grav_zero = 0; g_grav_next = 0; g_grav_shown = -10;
#if defined(GAME_MODE_CPU_GRAVITY) && defined(GAME_MODE_CARS)
    memset(g_cpu_seen, 0, sizeof g_cpu_seen);
#endif
}

static void grav_tick(Hud *h) {
    int l = lang();
    if (!g_racing) { grav_restore(); return; }
    if (g_go && !g_grav_next) g_grav_next = g_go_t + GRAV_PERIOD;
    if (g_grav_next && g_now >= g_grav_next) {
        g_grav_tenths = g_grav_tenths > 10 ? k_grav_light[rnd() % 4] : k_grav_heavy[rnd() % 4];
        g_grav_zero = rnd() % 3 == 0;
        static int force = -1;               /* RT_MODES_ZERO_G=1: every change starts at zero G, for tests */
        if (force < 0) force = getenv("RT_MODES_ZERO_G") != NULL;
        if (force) g_grav_zero = 1;
        if (g_grav_zero) grav_launch(1);
        g_grav_zero_end = g_now + GRAV_ZERO_TIME;
        g_grav_next = (g_grav_zero ? g_grav_zero_end : g_now) + GRAV_PERIOD;
        g_grav_shown = g_now;
    }
    if (g_grav_zero && g_now >= g_grav_zero_end) { g_grav_zero = 0; g_grav_shown = g_now; }
    double f = g_grav_zero ? GRAV_ZERO_FLIGHT : g_grav_tenths / 10.0;
    static double fixed = -2;                /* RT_MODES_GRAVITY=factor: a fixed one, for tests */
    if (fixed == -2) fixed = getenv("RT_MODES_GRAVITY") ? atof(getenv("RT_MODES_GRAVITY")) : -1;
    if (fixed >= 0) f = fixed;
    grav_write(f);
    grav_cpu(f);
    if (g_grav_zero) grav_launch(0);
    snprintf(h->line[0], sizeof h->line[0], "%s %d.%d G", l ? "GRAVITA" : "GRAVITY", g_grav_zero ? 0 : g_grav_tenths / 10, g_grav_zero ? 0 : g_grav_tenths % 10);
    h->line_rgb[0] = g_grav_zero ? 0xff60ff : g_grav_tenths > 10 ? 0xff9040 : g_grav_tenths < 10 ? 0x60d0ff : 0xffffff;
    if (g_grav_next) {
        snprintf(h->line[1], sizeof h->line[1], "%s %d", l ? "CAMBIA TRA" : "CHANGE IN", (int)ceil(g_grav_next - g_now));
        h->line_rgb[1] = 0xc0c0c0;
    }
    if (g_now - g_grav_shown < 1.5) {
        if (g_grav_zero) snprintf(h->big, sizeof h->big, "%s", l ? "GRAVITA ZERO!" : "ZERO G!");
        else snprintf(h->big, sizeof h->big, "%d.%d G", g_grav_tenths / 10, g_grav_tenths % 10);
        h->big_rgb = h->line_rgb[0];
    }
}
#else
static void grav_start(void) {}
static void grav_restore(void) {}
static void grav_tick(Hud *h) { (void)h; }
#endif

/* ------------------------------------------------------------------ SPEED */
/* As in the film: the bomb arms itself the first time the car reaches the limit, and from then
 * on SPEED_GRACE seconds below it set it off: the race ends there (the clock to zero). The limit
 * is in the unit of the game's speedometer (the TEST MODE option, speed_unit: its NVRAM byte and
 * bit, 1 = mph): 50 km/h or 30 mph. The fonts have no slash: KMH, MPH. */
#define SPEED_LIMIT_KMH 50
#define SPEED_LIMIT_MPH 30
#define SPEED_GRACE 2.0

static int speed_mph(void) {
#ifdef GAME_MODE_SPEED_UNIT
    static const uint32_t u[2] = GAME_MODE_SPEED_UNIT;
    return hw_nvram()[u[0]] >> u[1] & 1;
#else
    return 0;
#endif
}
static int speed_limit(void) { return speed_mph() ? SPEED_LIMIT_MPH : SPEED_LIMIT_KMH; }
static const char *speed_unit(void) { return speed_mph() ? "MPH" : "KMH"; }

static const char *speed_text(int line, int lang) {
    static char buf[2][64];
    if (line == 1) snprintf(buf[0], sizeof buf[0], lang ? "UNA BOMBA SI ARMA A %d %s" : "A BOMB ARMS ITSELF AT %d %s", speed_limit(), speed_unit());
    else snprintf(buf[1], sizeof buf[1], lang ? "SOTTO I %d %s PER 2 SECONDI ESPLODE" : "BELOW %d %s FOR 2 SECONDS IT EXPLODES", speed_limit(), speed_unit());
    return buf[line == 1 ? 0 : 1];
}
static int g_speed_armed, g_speed_boom;
static double g_speed_below, g_speed_boom_t;

static void speed_start(void) { g_speed_armed = 0; g_speed_boom = 0; g_speed_below = -1; }

static void speed_tick(Hud *h) {
    int l = lang();
    if (g_speed_boom) {                      /* keep the clock at zero until the race state ends */
        if (g_racing) end_race();
        if (g_now - g_speed_boom_t < 3.0) {
            snprintf(h->big, sizeof h->big, "BOOM!");
            h->big_rgb = 0xff4020;
            double k = 1.0 - (g_now - g_speed_boom_t) / 0.6;
            h->flash = k > 0 ? (int)(k * 230) : 0;
        }
        return;
    }
    if (!g_racing || !g_go) return;
    double v = player_speed_kmh() / (speed_mph() ? 1.609344 : 1.0);
    int limit = speed_limit();
    if (!g_speed_armed) {
        if (v >= limit) { g_speed_armed = 1; g_speed_below = -1; }
        snprintf(h->line[0], sizeof h->line[0], "%s %d %s", l ? "BOMBA: SI ARMA A" : "BOMB: ARMS AT", limit, speed_unit());
        h->line_rgb[0] = 0xffd800;
        return;
    }
    if (v >= limit) g_speed_below = -1;
    else if (g_speed_below < 0) g_speed_below = g_now;
    snprintf(h->line[0], sizeof h->line[0], "%s %d %s", l ? "ARMATA: SOPRA" : "ARMED: OVER", limit, speed_unit());
    h->line_rgb[0] = 0xff6040;
    if (g_speed_below >= 0) {
        double left = SPEED_GRACE - (g_now - g_speed_below);
        if (left <= 0) {
            g_speed_boom = 1;
            g_speed_boom_t = g_now;
            explode(0);
            end_race();
            mlog("modes: SPEED bomb exploded\n");
            return;
        }
        snprintf(h->big, sizeof h->big, "%s %.1f", l ? "ACCELERA!" : "SPEED UP!", left);
        h->big_rgb = ((int)(g_now * 6) & 1) ? 0xff2020 : 0xffffff;
    }
}

/* ------------------------------------------------------------------ PASS THE BOMB (GTI Club 2) */
/* The cars of the race are the profile's car array (`cars`: entry 0 the player, then the CPU
 * cars; position at car_pos, the one of the frame before at car_prev, velocity per frame at
 * car_vel, all x, height, z). 3 s after the start a random car gets the bomb, with a 25 s fuse.
 * A car touching the one with the bomb takes it: its centre within BOMB_TOUCH_LONG metres along
 * the bomb car's direction of travel and BOMB_TOUCH_SIDE across (two cars side by side in a lane
 * are further apart than that), at most every BOMB_COOLDOWN seconds, and never straight back to
 * the car it came from before BOMB_NO_RETURN seconds. When
 * the fuse runs out the bomb explodes: that car stands still for BOMB_STOP seconds (the player:
 * accelerator released and brake held; a CPU car: put back where it stopped, its velocity zeroed,
 * every frame), and after BOMB_REST seconds another car gets a new bomb. */
#if defined(GAME_MODE_CARS)
#define BOMB_FUSE 25.0
#define BOMB_TOUCH_LONG 4.0
#define BOMB_TOUCH_SIDE 1.9
#define BOMB_COOLDOWN 1.5
#define BOMB_NO_RETURN 5.0
#define BOMB_STOP 4.0
#define BOMB_REST 3.0
static int g_bomb_car, g_bomb_hit_car, g_bomb_from, g_bomb_last, g_bomb_booms[MAX_CARS];
static char g_bomb_to[8];                     /* the label of the car it was passed to, at that moment */
static double g_bomb_fuse_end, g_bomb_passed, g_bomb_next, g_bomb_boom_t;

/* car b touches car a: within the box around a, along a's direction of travel */
static int car_touch(int a, int b) {
    double p[3], q[3], f[2];
    car_pos(a, p);
    car_pos(b, q);
    f[0] = p[0] - LDF32(car_addr(a) + GAME_MODE_CAR_PREV);
    f[1] = p[2] - LDF32(car_addr(a) + GAME_MODE_CAR_PREV + 8);
    double len = sqrt(f[0] * f[0] + f[1] * f[1]), dx = q[0] - p[0], dz = q[2] - p[2];
    if (fabs(q[1] - p[1]) > 2.0) return 0;
    if (len < 1e-3) return dx * dx + dz * dz < BOMB_TOUCH_SIDE * BOMB_TOUCH_SIDE;   /* standing: a circle */
    double along = (dx * f[0] + dz * f[1]) / len, across = (dz * f[0] - dx * f[1]) / len;
    return fabs(along) < BOMB_TOUCH_LONG && fabs(across) < BOMB_TOUCH_SIDE;
}

static int bomb_holder(void) { return g_bomb_car; }

static void bomb_start(void) {
    g_bomb_car = -1; g_bomb_hit_car = -1; g_bomb_next = 0; g_bomb_boom_t = -100;
    memset(g_bomb_booms, 0, sizeof g_bomb_booms);
}

static void bomb_give(int except) {        /* a new bomb, to a random car in the race */
    int live[MAX_CARS], n = 0;
    for (int k = 0; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++)
        if (k != except && car_live(k)) live[n++] = k;
    if (!n) return;
    g_bomb_car = live[rnd() % n];
    g_bomb_from = -1;
    static double fuse = -1;                 /* RT_MODES_BOMB_FUSE=s: another fuse, for tests */
    if (fuse < 0) fuse = getenv("RT_MODES_BOMB_FUSE") ? atof(getenv("RT_MODES_BOMB_FUSE")) : BOMB_FUSE;
    g_bomb_fuse_end = g_now + fuse;
    g_bomb_passed = g_now;
    mlog("modes: bomb to car %d\n", g_bomb_car);
}

static void bomb_hold_tick(void) {         /* the exploded car stands still */
    int k = g_bomb_hit_car;
    if (k < 0) return;
    if (g_now - g_bomb_boom_t >= BOMB_STOP) { g_bomb_hit_car = -1; g_mode_hold = 0; return; }
    if (k == 0) { g_mode_hold = 1; return; }
    car_hold(k);
}

static void bomb_tick(Hud *h) {
    int l = lang();
    if (!g_racing) { g_mode_hold = 0; return; }
    bomb_hold_tick();
    if (!g_go) return;
    if (g_bomb_car < 0) {
        if (!g_bomb_next) g_bomb_next = g_go_t + 3.0;
        if (g_now >= g_bomb_next) bomb_give(g_bomb_boom_t > 0 ? g_bomb_last : -1);   /* not the car that just blew up */
    } else if (g_now >= g_bomb_fuse_end) {             /* BOOM */
        int k = g_bomb_car;
        if (k > 0) car_hold_begin(k);
        explode(k);
        g_bomb_hit_car = k;
        g_bomb_last = k;
        g_bomb_boom_t = g_now;
        if (k < MAX_CARS) g_bomb_booms[k]++;
        g_bomb_car = -1;
        g_bomb_next = g_now + BOMB_REST;
        mlog("modes: bomb exploded on car %d\n", k);
    } else if (g_now - g_bomb_passed >= BOMB_COOLDOWN) {  /* a touch passes it */
        int best = -1;
        double bd = 1e9;
        for (int k = 0; k < GAME_MODE_CAR_COUNT; k++) {
            if (k == g_bomb_car || k == g_bomb_hit_car || !car_live(k)) continue;
            if (k == g_bomb_from && g_now - g_bomb_passed < BOMB_NO_RETURN) continue;
            double d = car_dist(k, g_bomb_car);
            if (d < bd && car_touch(g_bomb_car, k)) { bd = d; best = k; }
        }
        if (best >= 0) {
            mlog("modes: bomb passed from car %d to car %d\n", g_bomb_car, best);
            g_bomb_from = g_bomb_car;
            g_bomb_car = best;
            snprintf(g_bomb_to, sizeof g_bomb_to, "%s", car_label(best) ? car_label(best) : "");
            g_bomb_passed = g_now;
        }
    }
    if (g_bomb_car < 0 && g_bomb_next > g_now && g_bomb_hit_car < 0 && g_now - g_bomb_boom_t > 1.5) {
        snprintf(h->line[0], sizeof h->line[0], "%s", l ? "NUOVA BOMBA IN ARRIVO" : "A NEW BOMB IS COMING");
        h->line_rgb[0] = 0xc0c0c0;
    }
    if (g_bomb_car >= 0) {
        int left = (int)ceil(g_bomb_fuse_end - g_now);
        if (g_bomb_car == 0) {
            snprintf(h->line[0], sizeof h->line[0], "%s %d", l ? "HAI LA BOMBA!" : "YOU HAVE THE BOMB!", left);
            h->line_rgb[0] = ((int)(g_now * (left <= 5 ? 6 : 2)) & 1) ? 0xff3020 : 0xffd800;
            snprintf(h->line[1], sizeof h->line[1], "%s", l ? "TOCCA UNA AUTO PER PASSARLA" : "TOUCH A CAR TO PASS IT");
            h->line_rgb[1] = 0xffffff;
        } else {
            /* who has it (the label on its marker), ahead or behind (by the player's direction
             * of travel), how far */
            const char *who = car_label(g_bomb_car);
            int ahead = car_ahead_in_race(g_bomb_car);
            if (ahead < 0) ahead = car_ahead(0, g_bomb_car);
            snprintf(h->line[0], sizeof h->line[0], "%s%s%s %s %d M  %d", l ? "BOMBA:" : "BOMB:", who ? " " : "", who ? who : "",
                     ahead ? (l ? "DAVANTI" : "AHEAD") : (l ? "DIETRO" : "BEHIND"), (int)car_dist(0, g_bomb_car), left);
            h->line_rgb[0] = 0xffa040;
        }
        if (g_now - g_bomb_passed < 1.2 && g_bomb_passed > g_go_t + 3.1) {
            const char *who = g_bomb_to[0] ? g_bomb_to : NULL;
            if (g_bomb_car == 0) snprintf(h->big, sizeof h->big, "%s", l ? "LA BOMBA E TUA!" : "YOU GOT THE BOMB!");
            else if (who) snprintf(h->big, sizeof h->big, "%s %s", l ? "BOMBA PASSATA A" : "BOMB PASSED TO", who);
            else snprintf(h->big, sizeof h->big, "%s", l ? "BOMBA PASSATA!" : "BOMB PASSED!");
            h->big_rgb = g_bomb_car == 0 ? 0xff4020 : 0x60ff60;
        }
    }
    if (g_now - g_bomb_boom_t < 2.0) {
        snprintf(h->big, sizeof h->big, "BOOM!");
        h->big_rgb = 0xff4020;
        if (g_bomb_hit_car == 0) {
            double k = 1.0 - (g_now - g_bomb_boom_t) / 0.6;
            h->flash = k > 0 ? (int)(k * 230) : 0;
        }
    }
    if (g_bomb_booms[0] && !h->line[1][0]) {
        snprintf(h->line[1], sizeof h->line[1], "%s %d", l ? "TUE ESPLOSIONI" : "YOUR EXPLOSIONS", g_bomb_booms[0]);
        h->line_rgb[1] = 0xc0c0c0;
    }
}
#else
static void bomb_start(void) {}
static void bomb_tick(Hud *h) { (void)h; }
#endif

/* ------------------------------------------------------------------ POLICE (Thrill Drive 2) */
/* The player is the police (in the police car), the CPU cars are on the run. Staying within
 * POLICE_DIST metres of one fills its arrest meter in POLICE_TIME seconds, all in one go: it
 * drops to zero as soon as the player is further away; a full meter arrests it: it stands where
 * it stopped for the rest of the race. When all of them are arrested, the race ends a few
 * seconds later. */
#ifdef GAME_MODE_CARS
#define POLICE_DIST 7.0                       /* between the centres: about 2.5 m between the bumpers */
#define POLICE_TIME 3.0
#define POLICE_HEAD_START 6.0                 /* the grid is close: no arrest before this */
static double g_pol_meter[MAX_CARS], g_pol_last, g_pol_done_t, g_pol_arrest_t;
static int g_pol_arrested[MAX_CARS], g_pol_count, g_pol_total, g_pol_police_held;

static void police_start(void) {
    memset(g_pol_meter, 0, sizeof g_pol_meter);
    memset(g_pol_arrested, 0, sizeof g_pol_arrested);
    g_pol_count = g_pol_total = g_pol_police_held = 0;
    g_pol_last = -1;
    g_pol_done_t = g_pol_arrest_t = -100;
}

static void police_tick(Hud *h) {
    int l = lang();
    if (!g_racing) return;
    for (int k = 1; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++)
        if (g_pol_arrested[k]) car_hold(k);
#ifdef GAME_MODE_POLICE_CAR_INDEX
    if (!g_pol_police_held) { car_hold_begin(GAME_MODE_POLICE_CAR_INDEX); g_pol_police_held = 1; }
    car_hold(GAME_MODE_POLICE_CAR_INDEX);    /* the course's own police car stays parked (and unseen) */
#endif
    if (!g_go) return;
    if (g_now - g_go_t < POLICE_HEAD_START) {
        snprintf(h->line[0], sizeof h->line[0], "%s %d", l ? "I FUGGITIVI SCAPPANO" : "THE SUSPECTS ESCAPE", (int)ceil(POLICE_HEAD_START - (g_now - g_go_t)));
        h->line_rgb[0] = 0xff6060;
        return;
    }
    double dt = g_pol_last < 0 ? 0 : g_now - g_pol_last;
    g_pol_last = g_now;
    int total = 0, near = -1;
    double nd = 1e9;
    for (int k = 1; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++) {
        if (!car_live(k)) continue;
        total++;
        if (g_pol_arrested[k]) continue;
        double d = car_dist(0, k);
        if (d < nd) { nd = d; near = k; }
        static double reach = -1;           /* RT_MODES_POLICE_DIST=m: another reach, for tests */
        if (reach < 0) reach = getenv("RT_MODES_POLICE_DIST") ? atof(getenv("RT_MODES_POLICE_DIST")) : POLICE_DIST;
        g_pol_meter[k] = d < reach ? g_pol_meter[k] + dt : 0;
        if (g_pol_meter[k] >= POLICE_TIME) {             /* arrested */
            car_hold_begin(k);
            g_pol_arrested[k] = 1;
            g_pol_count++;
            g_pol_arrest_t = g_now;
            mlog("modes: car %d arrested\n", k);
        }
    }
    g_pol_total = total;
    if (total && g_pol_count >= total && g_pol_done_t < 0) g_pol_done_t = g_now;
    if (g_pol_done_t > 0) {
        snprintf(h->big, sizeof h->big, "%s", l ? "TUTTI ARRESTATI!" : "ALL ARRESTED!");
        h->big_rgb = 0x60ff60;
        if (g_now - g_pol_done_t > 4.0) end_race();
    } else if (g_now - g_pol_arrest_t < 1.5) {
        snprintf(h->big, sizeof h->big, "%s", l ? "ARRESTATO!" : "ARRESTED!");
        h->big_rgb = 0x6080ff;
    } else if (near >= 0 && g_pol_meter[near] > 0) {
        snprintf(h->big, sizeof h->big, "%s %d%%", l ? "ARRESTO" : "ARREST", (int)(g_pol_meter[near] / POLICE_TIME * 100));
        h->big_rgb = ((int)(g_now * 6) & 1) ? 0xff3030 : 0x4060ff;
    }
    snprintf(h->line[0], sizeof h->line[0], "%s %d %s %d", l ? "ARRESTATI" : "ARRESTED", g_pol_count, l ? "SU" : "OF", total);
    h->line_rgb[0] = 0x80a0ff;
    if (near >= 0) {
        snprintf(h->line[1], sizeof h->line[1], "%s %s %d M", l ? "FUGGITIVO" : "SUSPECT",
                 car_ahead(0, near) ? (l ? "DAVANTI" : "AHEAD") : (l ? "DIETRO" : "BEHIND"), (int)nd);
        h->line_rgb[1] = 0xffffff;
    }
}
#else
static void police_start(void) {}
static void police_tick(Hud *h) { (void)h; }
#endif

/* ------------------------------------------------------------------ ELIMINATION (GTI Club 2) */
/* Every ELIM_INTERVAL seconds of race the last of the cars still in it is out: a CPU car stands
 * still for the rest of the race; the player stops, and the race ends. The places come from the
 * markers over the cars (the player's is the one no CPU car has; for a car whose marker is not
 * drawn now, far away, its last place); an elimination waits until every car's place has been
 * seen once.
 * The goal does not end the race while two cars are left: at every sector the game copies the
 * player's lap (its progress record's byte +1; progress: the word of the car array entry pointing
 * to it) next to the race's number of laps (race_laps: TOC slot of the race record, offset of
 * the laps; the lap is the byte after it) and finishes the race once the lap is past it. The laps
 * are set to RACE_LAPS_OPEN meanwhile, so the line is a checkpoint like the others. The last one
 * left wins: the player is moved past the line (elim_finish_player), the game finishes the race
 * at the next sector with the player first and goes on to its next course (NOW CHALLENGE ANOTHER COURSE),
 * where the elimination starts again with all the cars. The race's own laps are put back when
 * the race ends. */
#if defined(GAME_MODE_CARS) && defined(GAME_MODE_MARKER_POS)
#define ELIM_INTERVAL_S 45.0
static double elim_interval(void) {          /* RT_MODES_ELIM_INTERVAL=s: another one, for tests */
    static double v = -1;
    if (v < 0) v = getenv("RT_MODES_ELIM_INTERVAL") ? atof(getenv("RT_MODES_ELIM_INTERVAL")) : ELIM_INTERVAL_S;
    return v;
}
#define ELIM_INTERVAL elim_interval()
#define ELIM_END_DELAY 4.0
static int g_elim_out[MAX_CARS], g_elim_left, g_elim_last, g_elim_over;
static double g_elim_next, g_elim_t, g_elim_over_t;
static char g_elim_label[8];

static int g_elim_finished;
/* the time the game gives at the line (the goal, a checkpoint here) is cut to ELIM_LINE_TIME of
 * it: a rise of the clock within ELIM_LINE_WINDOW seconds of a change of the player's lap */
#define ELIM_LINE_TIME (1.0 / 3.0)
#define ELIM_LINE_WINDOW 2.0
static int32_t g_elim_clock, g_elim_rise;
static int g_elim_lap, g_elim_rise_cut;
static double g_elim_rise_t, g_elim_lap_t;
static void elim_start(void) {
    memset(g_elim_out, 0, sizeof g_elim_out);
    g_elim_finished = 0;
    g_elim_clock = -1; g_elim_lap = -1; g_elim_rise_t = g_elim_lap_t = -100; g_elim_rise_cut = 1;
    g_elim_left = GAME_MODE_CAR_COUNT;
    g_elim_next = 0; g_elim_t = -100; g_elim_last = -1; g_elim_over = 0;
}

/* the place of every car in the race (0 TOP), the last one seen; 0 if one was never seen */
static int elim_places(int *place) {
    int used = 0, n = 0;
    for (int k = 1; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++) {
        if (!car_live(k)) { place[k] = -1; continue; }
        n++;
        if (car_place(k) < 0) return 0;
        place[k] = car_place(k);
        used |= 1 << car_place(k);
    }
    int me = 0;
    while (me <= n && (used >> me & 1)) me++;
    place[0] = me;
    return 1;
}

#if defined(GAME_MODE_PROGRESS) && defined(GAME_MODE_RACE_LAPS)
#define RACE_LAPS_OPEN 99
static int g_laps_saved = -1;               /* the race's own laps, while changed */
static uint32_t race_laps_addr(void) {
    static const uint32_t rl[2] = GAME_MODE_RACE_LAPS;
    uint32_t r = ptr_at(rl[0]);
    return r ? r + rl[1] : 0;
}
static void race_laps_set(int n) {
    uint32_t a = race_laps_addr();
    if (!a) return;
    if (g_laps_saved < 0) g_laps_saved = LD8(a);
    ST8(a, (uint32_t)n);
}
static void race_laps_restore(void) {
    uint32_t a = race_laps_addr();
    if (a && g_laps_saved >= 0) ST8(a, (uint32_t)g_laps_saved);
    g_laps_saved = -1;
}
static void elim_hold_laps(void) { race_laps_set(RACE_LAPS_OPEN); }
/* the winner, once: the race's laps at the player's lap and the player one lap further (lap,
 * distance of the laps done and total distance), as at the line: at the next sector the game
 * finds the lap past the race's and finishes it (it only checks while its own copy of the lap is
 * not past the laps already) */
static void elim_finish_player(void) {
    static const uint32_t ll[2] = GAME_MODE_LAP_LENGTH;
    uint32_t p = LD32(car_addr(0) + GAME_MODE_PROGRESS), pool = ptr_at(ll[0]);
    if (g_elim_finished || !p || p >= RAM_SIZE || !pool) return;
    g_elim_finished = 1;
    double L = LDF32(pool + ll[1]);
    int lap = (int)LD8(p + 1);
    race_laps_set(lap);
    ST8(p + 1, (uint32_t)(lap + 1));
    STF32(p + 0xc, LDF32(p + 0xc) + L);
    STF32(p + 0x14, LDF32(p + 0x14) + L);
    mlog("modes: the winner's race ends at the next sector\n");
}
#else
static void elim_hold_laps(void) {}
static void elim_finish_player(void) {}
static void race_laps_restore(void) {}
#endif

static void elim_line_time(void) {
#ifdef GAME_MODE_PROGRESS
    uint32_t p = LD32(car_addr(0) + GAME_MODE_PROGRESS);
    int lap = p && p < RAM_SIZE ? (int)LD8(p + 1) : -1;
    if (lap >= 0 && g_elim_lap >= 0 && lap != g_elim_lap) g_elim_lap_t = g_now;
    g_elim_lap = lap;
#endif
    int32_t t = time_left_raw();
    if (g_elim_clock >= 0 && t > g_elim_clock + 30) {          /* the clock rose: an extension */
        g_elim_rise = t - g_elim_clock;
        g_elim_rise_t = g_now;
        g_elim_rise_cut = 0;
    }
    if (!g_elim_rise_cut && fabs(g_elim_rise_t - g_elim_lap_t) < ELIM_LINE_WINDOW) {
        t -= (int32_t)(g_elim_rise * (1.0 - ELIM_LINE_TIME));
        set_time_left(t);
        g_elim_rise_cut = 1;
        mlog("modes: the line's extension cut to %d\n", (int)(g_elim_rise * ELIM_LINE_TIME));
    }
    if (g_now - g_elim_rise_t > ELIM_LINE_WINDOW) g_elim_rise_cut = 1;
    g_elim_clock = time_left_raw();
}

static void elim_tick(Hud *h) {
    int l = lang();
    if (!g_racing) return;
    for (int k = 1; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++)
        if (g_elim_out[k]) car_hold(k);
    if (!g_go) return;
    if (g_elim_over) {
        if (g_elim_out[0]) {                 /* out: stopped, then the race ends */
            g_mode_hold = 1;
            if (g_now - g_elim_over_t > ELIM_END_DELAY) end_race();
        } else elim_finish_player();         /* the winner: the game finishes the race */
    } else {
        elim_hold_laps();
        elim_line_time();
        if (!g_elim_next) g_elim_next = g_go_t + ELIM_INTERVAL;
        int place[MAX_CARS];
        if (g_now >= g_elim_next && elim_places(place)) {
            int last = -1;
            static int spare = -1;           /* RT_MODES_ELIM_SPARE=1: never the player, for tests */
            if (spare < 0) spare = getenv("RT_MODES_ELIM_SPARE") != NULL;
            if (spare) place[0] = -1;
            for (int k = 0; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++)
                if (!g_elim_out[k] && place[k] >= 0 && (last < 0 || place[k] > place[last])) last = k;
            if (last >= 0) {
                snprintf(g_elim_label, sizeof g_elim_label, "%s", car_label(last) ? car_label(last) : "");
                g_elim_out[last] = 1;
                g_elim_last = last;
                g_elim_t = g_now;
                if (last > 0) car_hold_begin(last);
                mlog("modes: car %d eliminated\n", last);
                if (last == 0) { g_elim_over = 1; g_elim_over_t = g_now; }
            }
            g_elim_next += ELIM_INTERVAL;
        }
        int left = 0;
        for (int k = 0; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++)
            if (!g_elim_out[k] && (k == 0 || car_live(k))) left++;
        g_elim_left = left;
        if (left == 1 && !g_elim_out[0] && !g_elim_over) { g_elim_over = 1; g_elim_over_t = g_now; }
    }
    if (g_elim_over && g_elim_out[0]) {
        snprintf(h->big, sizeof h->big, "%s", l ? "SEI ELIMINATO!" : "YOU ARE OUT!");
        h->big_rgb = 0xff4020;
        return;
    }
    if (g_elim_over) {
        snprintf(h->big, sizeof h->big, "%s", l ? "HAI VINTO!" : "YOU WIN!");
        h->big_rgb = 0x60ff60;
        return;
    }
    int next = (int)ceil(g_elim_next - g_now);
    snprintf(h->line[0], sizeof h->line[0], "%s %d", l ? "ELIMINAZIONE TRA" : "ELIMINATION IN", next < 0 ? 0 : next);
    h->line_rgb[0] = next <= 3 ? (((int)(g_now * 6) & 1) ? 0xff3020 : 0xffffff) : 0xffd800;
    snprintf(h->line[1], sizeof h->line[1], "%s %d", l ? "IN GARA" : "STILL IN", g_elim_left);
    h->line_rgb[1] = 0xc0c0c0;
    if (g_now - g_elim_t < 2.0 && g_elim_last > 0) {
        if (g_elim_label[0]) snprintf(h->big, sizeof h->big, "%s %s", l ? "FUORI:" : "OUT:", g_elim_label);
        else snprintf(h->big, sizeof h->big, "%s", l ? "UNA AUTO E FUORI!" : "A CAR IS OUT!");
        h->big_rgb = 0xff9040;
    }
}
#else
static void elim_start(void) {}
static void elim_tick(Hud *h) { (void)h; }
#endif

/* ------------------------------------------------------------------ the frame */
static void mode_end(void) {
    if (g_mode) mlog("modes: %s over\n", k_mode_key[g_mode]);
    grav_restore();
    race_laps_restore();
    g_mode = 0;
    g_racing = g_go = 0;
    g_mode_hold = g_mode_steer_hold = 0;
}

void modes_tick(int attract) {
    if (g_mode_steer_hold && --g_car_select_seen <= 0) g_mode_steer_hold = 0;   /* the screen is over */
    if (!g_mode && !g_selected) return;
    g_now = (double)rt_now() / CPU_HZ;
    if (attract) {                           /* no game: the chosen mode waits for its START */
        if (g_mode) mode_end();
        Hud *h = hud_begin(); hud_end(); (void)h;
        return;
    }
    if (!g_mode) {                           /* a game started with a mode chosen */
        g_mode = g_selected;
        g_selected = 0;
        g_rng = (uint32_t)rt_now() | 1;
        grav_start();
        speed_start();
        bomb_start();
        police_start();
        elim_start();
        car_labels_reset();
        mlog("modes: %s\n", k_mode_key[g_mode]);
    }
    int racing = race_now();
    if (racing && !g_racing) {                 /* a race starts (also the next course of a game) */
        g_race_t0 = g_now;
        elim_start();
        car_labels_reset();
        g_mode_hold = 0;
    }
    time_scale_tick(racing, racing && !g_racing);
    if (racing && !g_racing) { g_go = 0; g_time_at_race = time_left_raw(); }
    if (racing && !g_go && time_left_raw() != g_time_at_race) { g_go = 1; g_go_t = g_now; }
    if (!racing && g_racing) race_laps_restore();
    g_racing = racing;
    Hud *h = hud_begin();
    switch (g_mode) {
    case MODE_GRAVITY: grav_tick(h); break;
    case MODE_SPEED: speed_tick(h); break;
    case MODE_BOMB: bomb_tick(h); break;
    case MODE_POLICE: police_tick(h); break;
    case MODE_ELIMINATION: elim_tick(h); break;
    default: break;
    }
    if (!g_racing) h->line[0][0] = h->line[1][0] = 0;    /* only the explosion outlives the race */
    hud_end();
}

void modes_draw(uint32_t *fb, int w, int h) {
    const Hud *d = &g_hud[g_hud_cur];
    if (!d->mode) return;
    if (d->flash) enh_hud_dim(fb, w, h, 0, 0, w, h, d->flash, 0xffffff);
    int y = GAME_MODE_HUD_Y;
    for (int i = 0; i < 2; i++)
        if (d->line[i][0]) {
            enh_hud_text(fb, w, h, ENH_FONT_SMALL, -1, y, d->line[i], d->line_rgb[i]);
            y += enh_hud_font_height(ENH_FONT_SMALL) + 2;
        }
    if (d->big[0]) enh_hud_text(fb, w, h, ENH_FONT_LARGE, -1, GAME_MODE_HUD_BIG_Y, d->big, d->big_rgb);
}

int modes_active(void) { return g_mode; }


/* POLICE (Thrill Drive 2): the police car the game puts on the course does not show up, the
 * player is the police. The hook car_draw sits where the routine drawing one car has the
 * record's "not drawn" byte in a register (hide_police: the record's register, offset of its
 * marker byte, the police car's high nibble there, the register to set to 1); the car itself
 * stays parked where it waits (police_tick holds it), so it does not chase unseen. */
void modes_car_draw(PPCContext *c) {
#ifdef GAME_MODE_HIDE_POLICE
    static const uint32_t hp[4] = GAME_MODE_HIDE_POLICE;
    uint32_t rec = c->r[hp[0]];
    if (g_mode == MODE_POLICE && rec && rec < RAM_SIZE && (LD8(rec + hp[1]) >> 4) == hp[2]) c->r[hp[3]] = 1;
#else
    (void)c;
#endif
}

/* SELECT A CAR (Thrill Drive 2): a mode can come with its own car, the SPEED mode the bus, the
 * POLICE mode the police car. The hook car_select runs at every frame of the screen and at its
 * confirmation; car_select is {register holding the screen's struct, offset of the car index,
 * offset of the choice's countdown, offset of the screen's state, the choosing state, the
 * countdown left}: the mode's car on the carousel, the countdown cut to that while choosing (time
 * to pick the transmission), and the steering held at the centre (hw.c, g_mode_steer_hold) so the
 * carousel does not turn. */
volatile int g_mode_steer_hold;

static int mode_car(int mode) {            /* the car index a mode drives, -1 any */
    switch (mode) {
#ifdef GAME_MODE_BUS
    case MODE_SPEED: return GAME_MODE_BUS;
#endif
#ifdef GAME_MODE_POLICE_CAR
    case MODE_POLICE: return GAME_MODE_POLICE_CAR;
#endif
    default: return -1;
    }
}

void modes_car_select(PPCContext *c) {
#ifdef GAME_MODE_CAR_SELECT
    static const uint32_t sc[6] = GAME_MODE_CAR_SELECT;
    uint32_t s = c->r[sc[0]];
    int car = mode_car(g_mode);
    if (car < 0 || !s || s >= RAM_SIZE) return;
    ST32(s + sc[1], (uint32_t)car);
    if (LD32(s + sc[3]) == sc[4] && (int32_t)LD32(s + sc[2]) > (int32_t)sc[5]) ST32(s + sc[2], sc[5]);
    g_car_select_seen = 4;                                   /* frames the steering stays held */
    g_mode_steer_hold = 1;
#else
    (void)c;
#endif
}

/* The markers over the cars (GTI Club 2, hook "car_marker" at the entry of the routine that
 * draws one, 0x65bbc): r3 is the marker's byte, bits 3-5 the player (0-3: 1P-4P, 4: a CPU car)
 * and bits 0-2 the car's place (0 TOP, 1 2nd, ...), shown on it; f1 its distance from the camera
 * (the routine draws nothing out of its range); the caller's r18 the car's draw record, x at
 * +0x64 and z at +0x68, which tell which car of the car array it is. Each marker drawn records
 * the car's label (for the texts); the one over the car with the bomb blinks (out of range half
 * of the time). */
#if defined(GAME_MODE_CARS) && defined(GAME_MODE_MARKER_POS)
static char g_car_label[MAX_CARS][8];
static int g_car_place[MAX_CARS];             /* 0 TOP, 1 2nd, ...; -1 a human player's marker */
static double g_car_label_t[MAX_CARS];
static int bomb_holder(void);

void modes_car_marker(PPCContext *c) {
    static int dbg = -1;                     /* RT_MARKER_LOG=1: every marker drawn, for finding things */
    if (dbg < 0) dbg = getenv("RT_MARKER_LOG") != NULL;
    if (dbg) {
        static const uint32_t at_[2] = GAME_MODE_MARKER_POS;
        rt_log("marker %02x r%u=%08x x=%.1f z=%.1f\n", c->r[3] & 0xff, at_[0], c->r[at_[0]], LDF32(c->r[at_[0]] + at_[1]), LDF32(c->r[at_[0]] + at_[1] + 4));
    }
    if (!g_mode || !g_racing) return;
    static const uint32_t at[2] = GAME_MODE_MARKER_POS;   /* register, offset of x */
    double x = LDF32(c->r[at[0]] + at[1]), z = LDF32(c->r[at[0]] + at[1] + 4);
    int best = -1;
    double bd = 9.0;                                         /* within 3 m */
    for (int k = 0; k < GAME_MODE_CAR_COUNT && k < MAX_CARS; k++) {
        double p[3];
        car_pos(k, p);
        double d = (p[0] - x) * (p[0] - x) + (p[2] - z) * (p[2] - z);
        if (d < bd) { bd = d; best = k; }
    }
    if (best < 0) return;
    uint32_t b = c->r[3] & 0xff, who = (b >> 3) & 7, place = b & 7;
    g_car_place[best] = who < 4 ? -1 : (int)place;
    if (who < 4) snprintf(g_car_label[best], sizeof g_car_label[best], "%uP", who + 1);
    else if (place == 0) snprintf(g_car_label[best], sizeof g_car_label[best], "TOP");
    else snprintf(g_car_label[best], sizeof g_car_label[best], "%u%s", place + 1, place == 1 ? "ND" : place == 2 ? "RD" : "TH");
    g_car_label_t[best] = g_now;
    if (g_mode == MODE_BOMB && best == bomb_holder() && fmod(g_now, 0.4) >= 0.2) c->f[1] = 1e9;   /* blink */
}

static int car_place(int k) { return k >= 0 && k < MAX_CARS && g_car_label[k][0] ? g_car_place[k] : -1; }
static void car_labels_reset(void) { memset(g_car_label, 0, sizeof g_car_label); }

/* the label of car k, if its marker was drawn lately */
#define LABEL_FRESH 3.0
static const char *car_label(int k) {
    return k >= 0 && k < MAX_CARS && g_car_label[k][0] && g_now - g_car_label_t[k] < LABEL_FRESH ? g_car_label[k] : NULL;
}

/* car k is ahead of the player in the race: by the places on the markers (the player's is the
 * one no other car has), when all are known; -1 if not */
static int car_ahead_in_race(int k) {
    int used = 0, n = 0;
    for (int j = 1; j < GAME_MODE_CAR_COUNT && j < MAX_CARS; j++) {
        if (!car_live(j)) continue;
        n++;
        if (!car_label(j) || g_car_place[j] < 0) return -1;
        used |= 1 << g_car_place[j];
    }
    int me = 0;
    while (me <= n && (used >> me & 1)) me++;
    return g_car_place[k] < me;
}
#else
void modes_car_marker(PPCContext *c) { (void)c; }
static const char *car_label(int k) { (void)k; return NULL; }
static int car_ahead_in_race(int k) { (void)k; return -1; }
static int car_place(int k) { (void)k; return -1; }
static void car_labels_reset(void) {}
#endif

#else  /* no EXTRA modes for this version */
int modes_list(const int **ids) { static const int none[1]; *ids = none; return 0; }
const char *modes_text(int mode, int line, int lang) { (void)mode; (void)line; (void)lang; return ""; }
void modes_select(int mode) { (void)mode; }
void modes_tick(int attract) { (void)attract; }
void modes_draw(uint32_t *fb, int w, int h) { (void)fb; (void)w; (void)h; }
int modes_active(void) { return 0; }
void modes_car_marker(PPCContext *c) { (void)c; }
void modes_car_select(PPCContext *c) { (void)c; }
void modes_car_draw(PPCContext *c) { (void)c; }
void modes_car_effect(PPCContext *c) { (void)c; }
volatile int g_mode_hold, g_mode_steer_hold;
#endif
