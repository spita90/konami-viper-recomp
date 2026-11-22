/*
 * Frames in between (enhanced mode, GPU renderer): one more picture per game frame, halfway
 * between the last two, drawn by running the game's own gl command lists again with the objects'
 * matrices interpolated. The game itself, its timing and its state do not change.
 *
 * The game builds its 3D scene as a command list in RAM (docs/ARCHITECTURE.md, section 5b,
 * "frames in between"): each object is a 05 command (a 3x4 modelview matrix) followed by a 38
 * (call display list N). At every flush (hook interp_flush) the list and the gl library's state
 * are kept; at the buffer swap (hook interp_swap) the objects of this frame are paired with the
 * previous frame's (same display list, nearest position) and every flush is run again, outside
 * the emulated time, once for each display time of the stretch between the two frames, with the
 * matrices that far between: the Voodoo draws them into shadow render targets, and the frontend
 * shows each at its time (vsyncs measured by the frontend, or fewer: FRAME RATE). RAM, CPU and
 * device state are put back afterwards.
 *
 * DISPLAY > FRAME RATE (frame_rate in the settings file). GTI Club 2 (the gl module of JAB and
 * EAA) and Thrill Drive 2 (a later gl version: the same state addresses, other hook addresses;
 * the same module in EBB, AAA and JAA). RT_INTERP=0/1/fps overrides the option (1: the screen's refresh), RT_INTERP_LOG=1 logs
 * the cost, RT_INTERP_CHECK=1 compares the RAM and the VRAM before and after (slow).
 */
#include "runtime.h"
#include "game_config.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

int voodoo_gpu_active(void);
void voodoo_sandbox(int on);
void voodoo_keep_state(int slot);
void voodoo_load_state(int state, int slot, uint32_t fifo);
void voodoo_show_between(uint64_t when);
void voodoo_handover(void);

#if defined(GAME_ENH_HOOK_INTERP_FLUSH) && defined(GAME_ENH_HOOK_INTERP_SWAP)

/* the gl library (GTI Club 2): list start and write pointer, its state */
#define GL_LIST_START 0x2320u
#define GL_LIST_PTR 0x2328u
#define GL_STATE_LO 0x2000u
#define GL_STATE_HI 0x3500u
#define GL_STATE_SIZE (GL_STATE_HI - GL_STATE_LO)
#define GL_FIFO_BASE 0x2350u                /* the command FIFO in VRAM (BAR1 addresses): base, end */
#define GL_FIFO_END 0x2354u
#define GL_FIFO_PTR 0x2358u                 /* where the library writes next */
#define GL_FIFO_WORDS_SIZE 20               /* base, end, next, limit, jump word */
#define BAR1 0x84000000u

#define OP_MATRIX 0x05u                     /* 12 floats: translation, then the 3x3 rotation */
#define MATRIX_WORDS 13
#define MAX_FLUSHES 16
#define PAIR_DIST 12.0f                     /* farther than this between frames: not the same object */
#define MAX_BETWEEN 16                      /* frames in between of one game frame, at most */
#define MAX_SLOTS 32                        /* shadow targets: drawn, waiting for their time */
#define MAX_RATE 1000                       /* RT_INTERP=1: the screen's refresh */

typedef struct { uint32_t s0, n, off; } Flush;          /* list address, words, offset in words[] */
typedef struct { uint32_t key, pos; float m[12]; int used, pair; } Obj;   /* pos: index in words[] */
typedef struct {
    Flush fl[MAX_FLUSHES];
    int nfl;
    uint32_t *words;                        /* the lists, one after the other */
    size_t nwords, capw;
    uint8_t *states;                        /* the gl state at each flush */
    Obj *obj;
    size_t nobj, capo;
    uint64_t swap;                          /* emulated time of its buffer swap */
} Frame;

static Frame g_fr[2];
static int g_cur, g_force = -2, g_check, g_in_sandbox, g_log, g_was_on;
static int g_complete;                      /* the frame being kept started with its first flush */
static uint32_t *g_out;                     /* the lists with matrices halfway */
static size_t g_cap_out;
static uint8_t *g_full;                     /* RT_INTERP_CHECK: the whole RAM before */
static unsigned g_frames, g_runs, g_skipped, g_swaps_run;
static double g_us;
static _Atomic unsigned long long g_us_total;     /* RT_FPS_STATS: the same, read by the frontend */
double interp_ms_take(void) {
    static unsigned long long last;
    unsigned long long now = atomic_load(&g_us_total), d = now - last;
    last = now;
    return (double)d / 1000.0;
}
static int g_slot, g_slots = 4;             /* the next shadow target slot, and how many */
static uint64_t g_len;                      /* the game's frame interval, smoothed (cycles) */
static uint64_t g_last_when;                /* the display time of the last frame in between */
static unsigned g_stopped;                  /* game frames whose frames in between were stopped */

static float as_float(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static uint32_t as_word(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

static void *grow(void *p, size_t *cap, size_t need, size_t size) {
    if (need <= *cap) return p;
    size_t c = *cap ? *cap : 1024;
    while (c < need) c *= 2;
    p = realloc(p, c * size);
    if (!p) rt_fatal("interp: out of memory");
    *cap = c;
    return p;
}

int interp_supported(void) { return 1; }
unsigned long long interp_frames(void) { return g_runs; }

static int enabled(void) {
    if (g_force == -2) {
        const char *e = getenv("RT_INTERP");
        g_force = e ? atoi(e) : -1;         /* 0 off, 1 the screen's refresh, else at most that */
        g_check = getenv("RT_INTERP_CHECK") != NULL;
        g_log = getenv("RT_INTERP_LOG") ? atoi(getenv("RT_INTERP_LOG")) : 0;
    }
    /* not while booting or applying settings unpaced: nobody sees those frames */
    int on = (g_force >= 0 ? g_force : enh_frame_rate()) != 0 && g_enhanced && voodoo_gpu_active() && !enh_turbo();
    if (on != g_was_on) {
        g_was_on = on;
        g_complete = 0;                     /* switched on in the middle of a frame: not that one */
        if (g_log) rt_log("interp: frames in between %s\n", on ? "on" : "off");
    }
    return on;
}

/* at a flush: keep the list and the gl state it starts from */
static void on_flush(void) {
    Frame *f = &g_fr[g_cur];
    if (f->nfl == MAX_FLUSHES) return;
    uint32_t s0 = LD32(GL_LIST_START), e0 = LD32(GL_LIST_PTR);
    if (e0 <= s0 || e0 - s0 > 0x100000u) return;
    uint32_t n = (e0 - s0) / 4;
    f->words = grow(f->words, &f->capw, f->nwords + n, 4);
    for (uint32_t k = 0; k < n; k++) f->words[f->nwords + k] = LD32(s0 + 4 * k);
    f->states = realloc(f->states, (size_t)(f->nfl + 1) * GL_STATE_SIZE);
    memcpy(f->states + (size_t)f->nfl * GL_STATE_SIZE, g_ram + GL_STATE_LO, GL_STATE_SIZE);
    if (f->nfl == 0) voodoo_keep_state(g_cur);         /* the device as the frame's lists find it */
    f->fl[f->nfl++] = (Flush){ s0, n, (uint32_t)f->nwords };
    f->nwords += n;
}

/* 12 words that can be a matrix: floats that are 0 or of a sane size. A 05 byte in other data
 * (Thrill Drive 2's 2D pictures: bytes such as 0x05050005, 0x1c040202) gives tiny ones */
static int matrix_like(const uint32_t *w) {
    for (int j = 0; j < 12; j++) {
        uint32_t e = (w[j] >> 23) & 0xff;            /* the exponent: 0 only for 0 (and denormals) */
        if ((w[j] & 0x7fffffffu) && (e < 127 - 64 || e > 127 + 40)) return 0;   /* below 1e-19, above 1e12 */
    }
    return 1;
}

/* the objects of a frame: each 05 matrix, named by the command after it (38: the display list
 * it calls; 18: geometry in the list, its header), or in Thrill Drive 2 by the model it calls */
static void find_objects(Frame *f) {
    f->nobj = 0;
    for (int i = 0; i < f->nfl; i++) {
        const uint32_t *w = f->words + f->fl[i].off;
        uint32_t n = f->fl[i].n;
        for (uint32_t k = 0; k + MATRIX_WORDS < n; k++) {
            if (w[k] != OP_MATRIX << 24 || w[k + MATRIX_WORDS] >> 24 >= 0x50 || !matrix_like(w + k + 1)) continue;
            f->obj = grow(f->obj, &f->capo, f->nobj + 1, sizeof(Obj));
            Obj *o = &f->obj[f->nobj++];
            o->key = w[k + MATRIX_WORDS];
#if GAME_ENH_INTERP_CALL_OP
            /* Thrill Drive 2 sets the material (1d, 1e, 32, 30, 47, 2e, 46) after the matrix, then
             * calls the model: its name is that call, the first before the next matrix (a small
             * list number, not a float that starts with the same byte) */
            if (o->key >> 24 != GAME_ENH_INTERP_CALL_OP)
                for (uint32_t j = k + MATRIX_WORDS + 1; j < n && j <= k + MATRIX_WORDS + 64; j++) {
                    if (w[j] == OP_MATRIX << 24) break;
                    if (w[j] >> 24 == GAME_ENH_INTERP_CALL_OP && (w[j] & 0xffffff) < 0x10000) { o->key = w[j]; break; }
                }
#endif
            o->pos = f->fl[i].off + k + 1;
            o->used = 0;
            for (int j = 0; j < 12; j++) o->m[j] = as_float(w[k + 1 + j]);
            k += MATRIX_WORDS;
        }
    }
}

/* a rotation (3x3, rows) as a quaternion, and back */
static void to_quat(const double *r, double *q) {
    double tr = r[0] + r[4] + r[8];
    if (tr > 0) {
        double s = sqrt(tr + 1.0) * 2;
        q[0] = 0.25 * s; q[1] = (r[7] - r[5]) / s; q[2] = (r[2] - r[6]) / s; q[3] = (r[3] - r[1]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        double s = sqrt(1.0 + r[0] - r[4] - r[8]) * 2;
        q[0] = (r[7] - r[5]) / s; q[1] = 0.25 * s; q[2] = (r[1] + r[3]) / s; q[3] = (r[2] + r[6]) / s;
    } else if (r[4] > r[8]) {
        double s = sqrt(1.0 + r[4] - r[0] - r[8]) * 2;
        q[0] = (r[2] - r[6]) / s; q[1] = (r[1] + r[3]) / s; q[2] = 0.25 * s; q[3] = (r[5] + r[7]) / s;
    } else {
        double s = sqrt(1.0 + r[8] - r[0] - r[4]) * 2;
        q[0] = (r[3] - r[1]) / s; q[1] = (r[2] + r[6]) / s; q[2] = (r[5] + r[7]) / s; q[3] = 0.25 * s;
    }
}

static void from_quat(const double *q, double *r) {
    double w = q[0], x = q[1], y = q[2], z = q[3];
    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - z * w);     r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w);     r[7] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}

static double det3(const double *r) {
    return r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
}

/* polar decomposition m = r p (r a rotation, p symmetric) of a matrix with a positive determinant:
 * Higham's iteration r <- (r + r^-T) / 2, a few steps from a matrix this close to a rotation */
static int polar(const double *m, double *r, double *p) {
    memcpy(r, m, 9 * sizeof *r);
    for (int it = 0; it < 12; it++) {
        double d = det3(r), n[9], e = 0;
        if (fabs(d) < 1e-12) return 0;
        /* the inverse transposed: the cofactors over the determinant */
        n[0] = (r[4] * r[8] - r[5] * r[7]) / d; n[1] = (r[5] * r[6] - r[3] * r[8]) / d; n[2] = (r[3] * r[7] - r[4] * r[6]) / d;
        n[3] = (r[2] * r[7] - r[1] * r[8]) / d; n[4] = (r[0] * r[8] - r[2] * r[6]) / d; n[5] = (r[1] * r[6] - r[0] * r[7]) / d;
        n[6] = (r[1] * r[5] - r[2] * r[4]) / d; n[7] = (r[2] * r[3] - r[0] * r[5]) / d; n[8] = (r[0] * r[4] - r[1] * r[3]) / d;
        for (int i = 0; i < 9; i++) { double v = 0.5 * (r[i] + n[i]); e += fabs(v - r[i]); r[i] = v; }
        if (e < 1e-9) break;
    }
    for (int i = 0; i < 3; i++)             /* p = r^T m */
        for (int j = 0; j < 3; j++)
            p[3 * i + j] = r[i] * m[j] + r[3 + i] * m[3 + j] + r[6 + i] * m[6 + j];
    return 1;
}

/* a 3x4 matrix at t between a and b: the translation linear; the 3x3, as the linear map it is (the
 * matrix stores it by columns), split into a rotation, along the shorter arc, and a stretch after
 * it (the model's own scales and squashes), linear: each axis of the model then keeps a length
 * between its two (split on the other side, a model squashed along one axis, as Thrill Drive 2's
 * crashed cars, swelled and sheared halfway). A mirrored one (the wheels of one side) has its
 * last column negated first. */
static void blend(const float *a, const float *b, float t, float *out) {
    for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * t;
    double ma[9], mb[9], ra[9], rb[9], pa[9], pb[9], qa[4], qb[4], q[4], r[9], l = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) { ma[3 * i + j] = a[3 + 3 * j + i]; mb[3 * i + j] = b[3 + 3 * j + i]; }
    double da = det3(ma), db = det3(mb);
    int mirror = da < 0;
    if ((da < 0) != (db < 0) || da == 0 || db == 0) goto linear;
    if (mirror)
        for (int i = 2; i < 9; i += 3) { ma[i] = -ma[i]; mb[i] = -mb[i]; }
    if (!polar(ma, ra, pa) || !polar(mb, rb, pb)) goto linear;
    to_quat(ra, qa);
    to_quat(rb, qb);
    double d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3], sb = d < 0 ? -1.0 : 1.0;
    for (int i = 0; i < 4; i++) { q[i] = qa[i] + (sb * qb[i] - qa[i]) * t; l += q[i] * q[i]; }
    l = 1.0 / sqrt(l);
    for (int i = 0; i < 4; i++) q[i] *= l;
    from_quat(q, r);
    for (int i = 0; i < 3; i++)             /* the rotation times the stretch halfway, by columns */
        for (int j = 0; j < 3; j++) {
            double v = 0;
            for (int k = 0; k < 3; k++) v += r[3 * i + k] * (pa[3 * k + j] + (pb[3 * k + j] - pa[3 * k + j]) * t);
            out[3 + 3 * j + i] = (float)(mirror && j == 2 ? -v : v);
        }
    return;
linear:
    for (int i = 3; i < 12; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}

static float dist2(const float *a, const float *b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

/* Pairing. The positions are in view space, so a turning camera moves everything: far objects by
 * tens of units a frame (the car select's carousel turns 7.5 degrees a frame), and objects of the
 * same name (four wheels, a row of posts) a unit or two apart swap places with the nearest one. The
 * list's order does not help either: the game sorts them. So each object's previous position is
 * carried forward by the motion of a rigid neighbour, an anchor: an object whose name occurs once
 * in each frame, so its pair is certain, and whose own matrices give that motion (the camera's,
 * for the scenery; the camera's and the car's, for a car's wheels). An object is then paired with
 * the one of its name whose position, carried by one of its two nearest anchors (or as it is),
 * comes closest, the closest pairs of each name first. */
#define ANCHORS_NEAR 2
typedef struct { uint32_t key; int idx; } KeyIdx;
typedef struct { float q[3], p[3], r[9]; } Anchor;   /* positions in f and in p, the motion p -> f */
typedef struct { float d; int i, j; } Cand;

static KeyIdx *g_kp, *g_kf;
static Anchor *g_anc;
static Cand *g_cand;
static size_t g_cap_kp, g_cap_kf, g_cap_anc, g_cap_cand;

static int cmp_key(const void *a, const void *b) {
    const KeyIdx *x = a, *y = b;
    return x->key != y->key ? (x->key < y->key ? -1 : 1) : x->idx - y->idx;
}

static int cmp_cand(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    return x->d < y->d ? -1 : x->d > y->d;
}

static KeyIdx *by_key(Frame *fr, KeyIdx *k, size_t *cap) {
    k = grow(k, cap, fr->nobj + 1, sizeof *k);
    for (size_t i = 0; i < fr->nobj; i++) k[i] = (KeyIdx){ fr->obj[i].key, (int)i };
    qsort(k, fr->nobj, sizeof *k, cmp_key);
    return k;
}

/* the 3x3 as the linear map it is (stored by columns) */
static void linear(const float *m, float *l) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) l[3 * r + c] = m[3 + 3 * c + r];
}

static void add_anchor(const Obj *op, const Obj *of, size_t *n) {
    float q2 = of->m[0] * of->m[0] + of->m[1] * of->m[1] + of->m[2] * of->m[2];
    float lim = PAIR_DIST + 0.3f * sqrtf(q2);      /* a far one moves farther when the camera turns */
    if (dist2(op->m, of->m) > lim * lim) return;
    float a[9], b[9], v[9];
    linear(op->m, a);
    linear(of->m, b);
    float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (fabsf(det) < 1e-12f) return;
    v[0] = (a[4] * a[8] - a[5] * a[7]) / det; v[1] = (a[2] * a[7] - a[1] * a[8]) / det; v[2] = (a[1] * a[5] - a[2] * a[4]) / det;
    v[3] = (a[5] * a[6] - a[3] * a[8]) / det; v[4] = (a[0] * a[8] - a[2] * a[6]) / det; v[5] = (a[2] * a[3] - a[0] * a[5]) / det;
    v[6] = (a[3] * a[7] - a[4] * a[6]) / det; v[7] = (a[1] * a[6] - a[0] * a[7]) / det; v[8] = (a[0] * a[4] - a[1] * a[3]) / det;
    g_anc = grow(g_anc, &g_cap_anc, *n + 1, sizeof *g_anc);
    Anchor *x = &g_anc[(*n)++];
    for (int k = 0; k < 3; k++) { x->q[k] = of->m[k]; x->p[k] = op->m[k]; }
    for (int r = 0; r < 3; r++)                     /* b a^-1: the anchor's motion */
        for (int c = 0; c < 3; c++)
            x->r[3 * r + c] = b[3 * r] * v[c] + b[3 * r + 1] * v[3 + c] + b[3 * r + 2] * v[6 + c];
}

/* how much c's 3x3, turned by rot (a linear map, or none), differs from o's, over o's scale:
 * 0 for the same orientation, up to 8 for a half turn */
#define TURN_WEIGHT 1.0f                    /* units^2 per unit of that: breaks ties of position */
static float turn2(const Obj *o, const Obj *c, const float *rot) {
    float lo[9], lc[9], e = 0, s = 0;
    linear(o->m, lo);
    linear(c->m, lc);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            float v = rot ? rot[3 * r] * lc[k] + rot[3 * r + 1] * lc[3 + k] + rot[3 * r + 2] * lc[6 + k] : lc[3 * r + k];
            e += (lo[3 * r + k] - v) * (lo[3 * r + k] - v);
            s += lo[3 * r + k] * lo[3 * r + k];
        }
    return s > 0 ? 3 * e / s : 0;
}

/* how far apart o (in f) and c (in p) are, c carried by the anchors near[] or as it is: the
 * distance squared (*pos) and that plus how much it turned (the cost). Two of a name at the same
 * place (both faces of a sign, two shadows) differ only by how they turn */
static float carried(const Obj *o, const Obj *c, const int *near, int nn, float *pos) {
    float d = dist2(o->m, c->m), best = d + TURN_WEIGHT * turn2(o, c, NULL);
    *pos = d;
    for (int k = 0; k < nn; k++) {
        const Anchor *a = &g_anc[near[k]];
        float x = c->m[0] - a->p[0], y = c->m[1] - a->p[1], z = c->m[2] - a->p[2], e = 0;
        for (int r = 0; r < 3; r++) {
            float w = a->q[r] + a->r[3 * r] * x + a->r[3 * r + 1] * y + a->r[3 * r + 2] * z - o->m[r];
            e += w * w;
        }
        if (e >= best) continue;
        float cost = e + TURN_WEIGHT * turn2(o, c, a->r);
        if (cost < best) { best = cost; *pos = e; }
    }
    return best;
}

/* each object of f paired with one of the same name in p (o->pair, -1 if none); 0 when most of
 * the scene has no pair (a cut, a new screen) */
static int pair_objects(Frame *p, Frame *f) {
    g_kp = by_key(p, g_kp, &g_cap_kp);
    g_kf = by_key(f, g_kf, &g_cap_kf);
    for (size_t i = 0; i < f->nobj; i++) f->obj[i].pair = -1;
    for (size_t i = 0; i < p->nobj; i++) p->obj[i].used = 0;
    size_t na = 0, paired = 0;
    for (int pass = 0; pass < 2; pass++)            /* the anchors, then the pairs */
        for (size_t a = 0, b = 0; a < p->nobj && b < f->nobj;) {
            uint32_t ka = g_kp[a].key, kb = g_kf[b].key;
            if (ka != kb) { if (ka < kb) a++; else b++; continue; }
            size_t ea = a, eb = b;
            while (ea < p->nobj && g_kp[ea].key == ka) ea++;
            while (eb < f->nobj && g_kf[eb].key == kb) eb++;
            if (pass == 0) {
                if (ea - a == 1 && eb - b == 1) add_anchor(&p->obj[g_kp[a].idx], &f->obj[g_kf[b].idx], &na);
                a = ea; b = eb;
                continue;
            }
            size_t nc = 0;
            g_cand = grow(g_cand, &g_cap_cand, (ea - a) * (eb - b), sizeof *g_cand);
            for (size_t i = b; i < eb; i++) {
                const Obj *o = &f->obj[g_kf[i].idx];
                int near[ANCHORS_NEAR], nn = 0;
                float nd[ANCHORS_NEAR];
                for (size_t k = 0; k < na; k++) {   /* its nearest anchors */
                    float x = g_anc[k].q[0] - o->m[0], y = g_anc[k].q[1] - o->m[1], z = g_anc[k].q[2] - o->m[2];
                    float d = x * x + y * y + z * z;
                    int at = nn < ANCHORS_NEAR ? nn++ : ANCHORS_NEAR;
                    while (at > 0 && nd[at - 1] > d) { if (at < ANCHORS_NEAR) { nd[at] = nd[at - 1]; near[at] = near[at - 1]; } at--; }
                    if (at < ANCHORS_NEAR) { nd[at] = d; near[at] = (int)k; }
                }
                for (size_t j = a; j < ea; j++) {
                    float pos, d = carried(o, &p->obj[g_kp[j].idx], near, nn, &pos);
                    if (pos < PAIR_DIST * PAIR_DIST) g_cand[nc++] = (Cand){ d, g_kf[i].idx, g_kp[j].idx };
                }
            }
            if (nc > 1) qsort(g_cand, nc, sizeof *g_cand, cmp_cand);
            for (size_t k = 0; k < nc; k++) {
                Obj *o = &f->obj[g_cand[k].i], *c = &p->obj[g_cand[k].j];
                if (o->pair >= 0 || c->used) continue;
                o->pair = g_cand[k].j;
                c->used = 1;
                paired++;
            }
            a = ea; b = eb;
        }
    if (g_log > 2) rt_log("interp: frame %u: %zu objects, %zu paired, %zu anchors\n", g_frames, f->nobj, paired, na);
    return f->nobj > 0 && paired * 2 >= f->nobj;
}

/* g_out: frame f's lists, each paired object's matrix at t between its pair in p and itself */
static void fill(Frame *p, Frame *f, float t) {
    g_out = grow(g_out, &g_cap_out, f->nwords, 4);
    memcpy(g_out, f->words, f->nwords * 4);
    if (t >= 1.0f) return;
    for (size_t i = 0; i < f->nobj; i++) {
        Obj *o = &f->obj[i];
        if (o->pair < 0) continue;
        float m[12];
        blend(p->obj[o->pair].m, o->m, t, m);
        for (int k = 0; k < 12; k++) g_out[o->pos + k] = as_word(m[k]);
    }
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* RT_INTERP_CHECK: the RAM must be what it was */
static void check_ram(void) {
    uint32_t bad = 0, first = 0, last = 0;
    for (uint32_t a = 0; a < RAM_SIZE; a++)
        if (g_ram[a] != g_full[a]) {
            if (!bad++) first = a;
            last = a;
            if (bad <= 12) rt_log("interp:   %06x: %02x, was %02x\n", a, g_ram[a], g_full[a]);
        }
    if (bad) rt_log("interp: CHECK frame %u: %u bytes differ, %06x..%06x\n", g_frames, bad, first, last);
}

/* the display, from the frontend: a vsync and the period between two, emulated cycles */
static _Atomic uint64_t g_vsync, g_period;
void interp_set_display(uint64_t vsync, uint64_t period) { g_vsync = vsync; g_period = period; }

/* Draw the frames in between of frame f: for each n, f's lists with the matrices at t[n] (1: as
 * they are), shown from emulated time when[n]. Outside the emulated time, on a copy of the RAM:
 * whatever the gl library writes (its state, the lists, the display lists it patches, its stack)
 * stays in the copy; the accessors read g_ram at every access, so pointing it at the copy costs
 * nothing elsewhere. The copy serves all of them: the library runs them as it runs frames. */
static void run_frames(PPCContext *c, Frame *p, Frame *f, const float *t, const uint64_t *when, int n) {
    double t0 = now_us();
    static uint8_t *copy, *vram0, *fifo;
    static size_t cap_fifo;
    if (!copy && !(copy = malloc(RAM_SIZE))) rt_fatal("interp: out of memory");
    if (g_check) {
        if (!g_full) g_full = malloc(RAM_SIZE);
        memcpy(g_full, g_ram, RAM_SIZE);
        uint32_t vs = 0;
        const uint8_t *v = voodoo_vram(&vs);
        if (!vram0) vram0 = malloc(vs);
        memcpy(vram0, v, vs);
    }
    memcpy(copy, g_ram, RAM_SIZE);
    /* the command FIFO in VRAM: the lists run again from where the library writes next, over
     * packets of this frame the Voodoo may not have run yet; put back afterwards */
    uint32_t vsize = 0, flo = LD32(GL_FIFO_BASE) - BAR1, fhi = LD32(GL_FIFO_END) + 8 - BAR1;
    uint8_t *vram = (uint8_t *)voodoo_vram(&vsize);
    int keep_fifo = vram && flo < fhi && fhi <= vsize;
    if (keep_fifo) {
        fifo = grow(fifo, &cap_fifo, fhi - flo, 1);
        memcpy(fifo, vram + flo, fhi - flo);
    }
    uint8_t *ram = g_ram;
    g_ram = copy;

    RtSandbox sv;
    rt_sandbox_enter(c, &sv);
    g_in_sandbox = 1;
    voodoo_sandbox(1);
    uint8_t fifo_words[GL_FIFO_WORDS_SIZE];
    for (int k = 0; k < n; k++) {
        fill(p, f, t[k]);
        /* the device as the first list found it, its FIFO empty where the library writes next:
         * the lists then go through it one after the other, as they did; the library gets its
         * state at each flush, except where it writes in the FIFO */
        if (g_log > 1) rt_log("interp: run %d/%d of frame %u, t %.3f, fifo at %x\n", k + 1, n, g_frames, t[k], LD32(GL_FIFO_PTR) - BAR1);
        voodoo_load_state((int)(f - g_fr), g_slot, LD32(GL_FIFO_PTR) - BAR1);
        g_slot = (g_slot + 1) % g_slots;
        for (int i = 0; i < f->nfl; i++) {
            memcpy(fifo_words, g_ram + GL_FIFO_BASE, sizeof fifo_words);
            memcpy(g_ram + GL_STATE_LO, f->states + (size_t)i * GL_STATE_SIZE, GL_STATE_SIZE);
            memcpy(g_ram + GL_FIFO_BASE, fifo_words, sizeof fifo_words);
            for (uint32_t w = 0; w < f->fl[i].n; w++) ST32(f->fl[i].s0 + 4 * w, g_out[f->fl[i].off + w]);
            rt_call(c, GAME_ENH_HOOK_INTERP_FLUSH);
            if (rt_sandbox_stopped()) break;
        }
        if (rt_sandbox_stopped()) {         /* the lists met data no longer theirs: none shown */
            g_stopped++;
            break;
        }
        voodoo_show_between(when[k]);
        voodoo_handover();                  /* to the frontend now, ahead of its time */
    }
    voodoo_sandbox(0);
    g_in_sandbox = 0;
    rt_sandbox_leave(c, &sv);
    g_ram = ram;
    if (keep_fifo) memcpy(vram + flo, fifo, fhi - flo);

    if (g_check) {
        check_ram();
        uint32_t vs = 0, bad = 0, first = 0, last = 0;
        const uint8_t *v = voodoo_vram(&vs);
        for (uint32_t a = 0; a < vs; a++)
            if (v[a] != vram0[a] && (a < flo || a >= fhi)) { if (!bad++) first = a; last = a; }
        if (bad) rt_log("interp: CHECK frame %u: %u VRAM bytes differ, %06x..%06x\n", g_frames, bad, first, last);
    }
    double const spent = now_us() - t0;
    g_us += spent;
    atomic_fetch_add(&g_us_total, (unsigned long long)spent);
    g_runs += (unsigned)n;
    g_swaps_run++;
    if (g_log && g_swaps_run % 60 == 0)
        rt_log("interp: %u frames, %u in between, %u without, %u stopped; %d this time; %.0f us a frame, %.0f a game frame\n",
               g_frames, g_runs, g_skipped, g_stopped, n, g_us / g_runs, g_us / g_swaps_run);
}

/* the frames in between of frame f (swapped at now, emulated cycles): one for each display
 * time (a vsync, or at most `fps` a second) of the stretch between frames p and f, shown one
 * frame interval later (D): the moment shown runs behind the game by D, evenly */
static void schedule(PPCContext *c, Frame *p, Frame *f, uint64_t now) {
    float t[MAX_BETWEEN];
    uint64_t when[MAX_BETWEEN];
    int n = 0;
    int fps = g_force == 1 ? MAX_RATE : g_force > 1 ? g_force : enh_frame_rate();
    uint64_t vs = g_vsync, per = g_period;
    if (!per) {                             /* no frontend (headless): a screen at that rate, or 60 */
        vs = 0;
        per = (uint64_t)(CPU_HZ / (fps > 0 && fps < MAX_RATE ? fps : 60));
    }
    /* at most fps a second, on every k-th vsync with k whole: a rate that does not divide the
     * screen's (120 on 144 Hz) puts the frames on uneven vsyncs, in a pattern that drifts with the
     * game's frame against the screen (smooth for a moment, uneven the next) */
    uint64_t step = per;
    if (fps > 0 && fps < MAX_RATE) {
        double k = ceil((double)CPU_HZ / fps / (double)per - 0.05);
        if (k > 1) step = per * (uint64_t)k;
    }
    /* slots: twice the frames of a game frame (a few of the last stretch wait for their time
     * while the next is drawn); each is a render target as large as the picture */
    int slots = 2 * (int)((uint64_t)(CPU_HZ / 25) / step + 2);
    g_slots = slots > MAX_SLOTS ? MAX_SLOTS : slots;
    if (g_slot >= g_slots) g_slot = 0;
    int paired = p->nfl && p->swap && pair_objects(p, f);
    uint64_t len = paired ? f->swap - p->swap : 0;
    if (paired && len > 0 && len < (uint64_t)(CPU_HZ / 10)) {
        g_len = g_len ? (g_len * 7 + len) / 8 : len;           /* the game's frame interval, smoothed */
        uint64_t d = g_len + (uint64_t)(CPU_HZ * 0.004);      /* and a margin to draw them */
        uint64_t a = p->swap + d, b = f->swap + d;
        if (a < now) a = now;
        /* every k-th vsync from the last frame in between (its vsync, on the vsyncs measured), so
         * the cadence holds from one game frame to the next; from the latest vsync after a gap */
        uint64_t base = vs;
        if (step > per && g_last_when && g_last_when + 2 * step > a && g_last_when < a + step) {
            int64_t m = llround((double)(int64_t)(g_last_when - vs) / (double)per);
            base = (uint64_t)((int64_t)vs + m * (int64_t)per);
        }
        uint64_t h = base + (a > base ? (a - base + step - 1) / step * step : 0);
        if (step > per && h <= g_last_when) h += step;
        for (; h < b && n < MAX_BETWEEN; h += step) {
            t[n] = (float)((double)(h - d - p->swap) / (double)len);
            when[n++] = h;
        }
    }
    if (!n) {                               /* a cut, a new screen, nothing paired: f as it is */
        t[0] = 1.0f;
        when[0] = vs + (now > vs ? (now - vs + step - 1) / step * step : 0);
        n = 1;
        g_skipped++;
    }
    g_last_when = when[n - 1];
    run_frames(c, p, f, t, when, n);
}

/* hooks; 1 if pc was one of them */
int interp_hook(PPCContext *c, uint32_t pc) {
    if (pc != GAME_ENH_HOOK_INTERP_FLUSH && pc != GAME_ENH_HOOK_INTERP_SWAP) return 0;
    if (g_in_sandbox || !enabled()) return 1;
    if (pc == GAME_ENH_HOOK_INTERP_FLUSH) { on_flush(); return 1; }
    /* the buffer swap: frame f is complete */
    Frame *f = &g_fr[g_cur], *p = &g_fr[g_cur ^ 1];
    g_frames++;
    if (!g_complete) f->nfl = 0;            /* kept from the middle: not usable */
    f->swap = rt_now();
    find_objects(f);
    if (f->nfl) schedule(c, p, f, f->swap);
    g_cur ^= 1;
    g_complete = 1;
    Frame *n = &g_fr[g_cur];
    n->nfl = 0;
    n->nwords = 0;
    n->nobj = 0;
    n->swap = 0;
    return 1;
}

#else
int interp_supported(void) { return 0; }
unsigned long long interp_frames(void) { return 0; }
void interp_set_display(uint64_t vsync, uint64_t period) { (void)vsync; (void)period; }
int interp_hook(PPCContext *c, uint32_t pc) { (void)c; (void)pc; return 0; }
double interp_ms_take(void) { return 0; }
#endif
