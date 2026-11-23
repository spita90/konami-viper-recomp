/*
 * On-screen touch controls of the iOS app: the cabinet's controls on a phone or tablet (VIPER_IOS
 * only; the desktop builds do not have this file).
 *
 * Three layouts follow what is on screen (enhanced mode):
 *   - driving: a steering stick on the left half (it appears where the thumb lands and follows
 *     it), BRAKE and GAS at the bottom right (a thumb slides from one to the other), shift down
 *     and up above them, VIEW (the START button, which changes the view in a race) and pause at
 *     the top; classic mode: COIN instead of pause;
 *   - menus (the attract menu, the pause, the options): a tap on an item selects it (enh_menu_tap:
 *     the overlay notes where it drew them), and BACK at the top left (not on the main menu);
 *   - the rankings' name entry: left/right through the letters, OK (the pedal, which takes the
 *     letter shown), DEL, END and KEYS (the on-screen keyboard, to type the name).
 * Nothing is shown behind the LOADING screen. A gamepad in use hides them until the next touch.
 *
 * The layout is in points (the window's units, SDL's touch coordinates) from the short side of
 * the screen and the safe area; the sprites are drawn once into an atlas at the screen's pixel
 * size (shapes here, the labels in the game font from enhanced.c), and the frontend draws them as
 * quads over everything, with the GPU renderer or the SDL one. Host main thread only.
 *
 * Tests: RT_TOUCH_SCRIPT="seconds:d|m|u:finger:x:y,..." puts fingers down, moves and lifts them at
 * those times after the start (real time), at x, y in 0..1 of the window, as a screen would.
 */
#include "runtime.h"
#include "touch.h"
#include "platform_ios.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { L_NONE, L_DRIVE, L_MENU, L_NAME };
enum { E_STICK, E_GAS, E_BRAKE, E_SHIFT_UP, E_SHIFT_DOWN, E_VIEW, E_PAUSE, E_COIN,
       E_UP, E_DOWN, E_LEFT, E_RIGHT, E_OK, E_BACK, E_DEL, E_END, E_KEYS, E_COUNT };
enum { SP_BTN, SP_BTN_ON, SP_PEDAL, SP_PEDAL_ON, SP_RING, SP_KNOB,
       SP_ARROW_U, SP_ARROW_D, SP_ARROW_L, SP_ARROW_R, SP_PAUSE, SP_EXIT, SP_KEYS,
       SP_TXT_GAS, SP_TXT_BRAKE, SP_TXT_VIEW, SP_TXT_COIN, SP_TXT_OK, SP_TXT_BACK, SP_TXT_DEL, SP_TXT_END,
       SP_COUNT };
enum { K_HOLD, K_TAP, K_REPEAT };

static const int k_icon[E_COUNT] = {
    [E_STICK] = -1, [E_GAS] = SP_TXT_GAS, [E_BRAKE] = SP_TXT_BRAKE, [E_SHIFT_UP] = SP_ARROW_U,
    [E_SHIFT_DOWN] = SP_ARROW_D, [E_VIEW] = SP_TXT_VIEW, [E_PAUSE] = SP_PAUSE, [E_COIN] = SP_TXT_COIN,
    [E_UP] = SP_ARROW_U, [E_DOWN] = SP_ARROW_D, [E_LEFT] = SP_ARROW_L, [E_RIGHT] = SP_ARROW_R,
    [E_OK] = SP_TXT_OK, [E_BACK] = SP_TXT_BACK, [E_DEL] = SP_TXT_DEL, [E_END] = SP_TXT_END, [E_KEYS] = SP_KEYS,
};

typedef struct { int on, rect; float x, y, w, h; } Elem;   /* centre and size, points */
static Elem g_e[E_COUNT];

#define MAX_FINGERS 10
enum { F_FREE = -1, F_DEAD = -2 };          /* no element / its layout went away: inert until lifted */
typedef struct { int used, elem; SDL_FingerID id; float x, y, ox, oy; Uint32 repeat_at; } Finger;
static Finger g_f[MAX_FINGERS];

static int g_on, g_visible = 1, g_layout = L_NONE, g_keyboard;
static int g_pic_w = 512, g_pic_h = 384;    /* the picture, letterboxed on the screen */
static float g_unit, g_stick_r, g_stick_top, g_stick_right;      /* points */
static float g_idle_x, g_idle_y;                                  /* the stick at rest */
static float g_ww = 1, g_wh = 1;                                  /* the window */

/* RT_TOUCH_SCRIPT (tests) */
typedef struct { double t; char act; int id; float x, y; } ScriptEv;
static ScriptEv g_script[64];
static int g_nscript, g_script_pos;
static Uint32 g_script_t0;

static void script_init(void) {
    const char *s = getenv("RT_TOUCH_SCRIPT");
    while (s && *s && g_nscript < 64) {
        ScriptEv e;
        int used = 0;
        if (sscanf(s, "%lf:%c:%d:%f:%f%n", &e.t, &e.act, &e.id, &e.x, &e.y, &used) != 5) break;
        g_script[g_nscript++] = e;
        s += used;
        while (*s == ',') s++;
    }
    g_script_t0 = SDL_GetTicks();
    if (g_nscript) g_on = 1;
}

static void finger_down(SDL_FingerID id, float x, float y);
static void finger_move(SDL_FingerID id, float x, float y);
static void finger_up(SDL_FingerID id);

static void script_tick(float ww, float wh) {
    double t = (SDL_GetTicks() - g_script_t0) / 1000.0;
    for (; g_script_pos < g_nscript && g_script[g_script_pos].t <= t; g_script_pos++) {
        const ScriptEv *e = &g_script[g_script_pos];
        SDL_FingerID id = (SDL_FingerID)(1000 + e->id);
        rt_log("touch (script): %c finger %d at %.2f, %.2f\n", e->act, e->id, e->x, e->y);
        if (e->act == 'd') finger_down(id, e->x * ww, e->y * wh);
        else if (e->act == 'm') finger_move(id, e->x * ww, e->y * wh);
        else finger_up(id);
    }
}

int touch_init(void) {
    g_on = 1;
    script_init();
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    return g_on;
}

void touch_hide(void) { g_visible = 0; }
void touch_set_picture(int w, int h) { if (w > 0 && h > 0) { g_pic_w = w; g_pic_h = h; } }

/* ------------------------------------------------------------------ layout */
static int current_layout(void) {
    if (g_enhanced) {
        if (enh_turbo()) return L_NONE;
        if (enh_paused() || enh_menu_active()) return L_MENU;   /* (the linked race's question: driving goes on) */
        if (enh_name_entry_active()) return L_NAME;
    }
    return L_DRIVE;
}

static int kind_of(int e) {
    switch (e) {
    case E_PAUSE: case E_BACK: case E_DEL: case E_END: case E_KEYS: return K_TAP;
    case E_OK: return g_layout == L_NAME ? K_HOLD : K_TAP;     /* name entry: the pedal takes the letter */
    case E_UP: case E_DOWN: case E_LEFT: case E_RIGHT: return K_REPEAT;
    default: return K_HOLD;
    }
}

static void circle(int e, float x, float y, float r) { g_e[e] = (Elem){ 1, 0, x, y, 2 * r, 2 * r }; }
static void rect(int e, float x0, float y0, float x1, float y1) {
    g_e[e] = (Elem){ 1, 1, (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0 };
}

static void layout(int lay, float ww, float wh) {
    float sl = 0, st = 0, sr = 0, sb = 0;
    ios_safe_area(&sl, &st, &sr, &sb);
    memset(g_e, 0, sizeof g_e);
    g_ww = ww;
    g_wh = wh;
    float u = (ww < wh ? ww : wh) / 100.0f, m = 3 * u;
    g_unit = u;
    float left = sl + m, top = st + m, right = ww - sr - m, bottom = wh - sb - m;
    if (lay == L_DRIVE) {
        float pw = 19 * u, ph = 32 * u;
        rect(E_GAS, right - pw, bottom - ph, right, bottom);
        rect(E_BRAKE, right - 2 * pw - 4 * u, bottom - ph, right - pw - 4 * u, bottom);
        float sy = bottom - ph - 3 * u - 6.5f * u;
        circle(E_SHIFT_UP, g_e[E_GAS].x, sy, 6.5f * u);
        circle(E_SHIFT_DOWN, g_e[E_BRAKE].x, sy, 6.5f * u);
        circle(E_VIEW, right - 6.5f * u, top + 6.5f * u, 6.5f * u);
        if (g_enhanced) circle(E_PAUSE, left + 6 * u, top + 6 * u, 6 * u);   /* linked: leave the session? */
        else circle(E_COIN, right - 22 * u, top + 6.5f * u, 6.5f * u);
        g_stick_r = 15 * u;
        g_stick_top = top + 14 * u;
        g_stick_right = ww * 0.5f;
        g_idle_x = left + 22 * u;
        g_idle_y = bottom - 22 * u;
        g_e[E_STICK].on = 1;
    } else if (lay == L_MENU) {
        if (!enh_menu_at_top()) circle(E_BACK, left + 7 * u, top + 7 * u, 7 * u);
    } else if (lay == L_NAME) {
        float cx = left + 24 * u, cy = bottom - 24 * u, d = 15 * u, r = 8 * u;
        circle(E_LEFT, cx - d, cy, r);
        circle(E_RIGHT, cx + d, cy, r);
        float ox = right - 11 * u, oy = bottom - 20 * u;
        circle(E_OK, ox, oy, 11 * u);
        circle(E_DEL, ox - 26 * u, oy + 8 * u, 8.5f * u);
        circle(E_END, ox - 4 * u, oy - 26 * u, 8.5f * u);
        circle(E_KEYS, right - 6.5f * u, top + 6.5f * u, 6.5f * u);
    }
}

/* the element under (x, y): the nearest one whose area, a little enlarged, holds it */
static int hit(float x, float y, int pedals_only) {
    int best = -1;
    float bestd = 1.35f;
    for (int e = 0; e < E_COUNT; e++) {
        const Elem *p = &g_e[e];
        if (!p->on || e == E_STICK || (pedals_only && e != E_GAS && e != E_BRAKE)) continue;
        float d;
        if (p->rect) d = fmaxf(fabsf(x - p->x) / (p->w / 2), fabsf(y - p->y) / (p->h / 2));
        else d = hypotf(x - p->x, y - p->y) / (p->w / 2);
        if (p->rect && d > 1) d = 1 + (d - 1) * 0.5f;        /* the pedals: a wider margin */
        if (d < bestd) { bestd = d; best = e; }
    }
    return best;
}

/* ------------------------------------------------------------------ actions */
static void fire(int e) {
    switch (e) {
    case E_PAUSE: enh_escape(); break;
    case E_UP: enh_menu_action(ENH_UP); break;
    case E_DOWN: enh_menu_action(ENH_DOWN); break;
    case E_LEFT: if (g_layout == L_NAME) enh_name_step(-1); else enh_menu_action(ENH_LEFT); break;
    case E_RIGHT: if (g_layout == L_NAME) enh_name_step(1); else enh_menu_action(ENH_RIGHT); break;
    case E_OK: enh_menu_action(ENH_OK); break;
    case E_BACK: enh_menu_action(ENH_BACK); break;
    case E_DEL: enh_name_type('\b'); break;
    case E_END: enh_name_type('\r'); break;
    case E_KEYS: g_keyboard = 1; break;
    default: break;
    }
}

static int stick_finger(void) {
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].elem == E_STICK) return i;
    return -1;
}

static void press(Finger *f, int e) {
    f->elem = e;
    ios_haptic_tap();
    int k = kind_of(e);
    if (k == K_TAP || k == K_REPEAT) fire(e);
    f->repeat_at = SDL_GetTicks() + 400;
}

/* a tap on the menu's items: (x, y) in the picture (letterboxed as the frontend draws it) */
static int menu_tap(float x, float y) {
    float pa = (float)g_pic_w / g_pic_h, pw = g_ww, ph = g_wh;
    if (pw / ph > pa) pw = ph * pa; else ph = pw / pa;
    float nx = (x - (g_ww - pw) / 2) / pw, ny = (y - (g_wh - ph) / 2) / ph;
    if (nx < 0 || ny < 0 || nx > 1 || ny > 1 || !enh_menu_tap(nx, ny)) return 0;
    ios_haptic_tap();
    return 1;
}

static void finger_down(SDL_FingerID id, float x, float y) {
    g_visible = 1;
    Finger *f = NULL;
    for (int i = 0; i < MAX_FINGERS && !f; i++)
        if (!g_f[i].used) f = &g_f[i];
    if (!f) return;
    *f = (Finger){ 1, F_FREE, id, x, y, x, y, 0 };
    int e = hit(x, y, 0);
    if (e >= 0) press(f, e);
    else if (g_layout == L_DRIVE && enh_link_menu_active() && menu_tap(x, y)) {}   /* LEAVE / STAY */
    else if (g_layout == L_DRIVE && x < g_stick_right && y > g_stick_top && stick_finger() < 0) f->elem = E_STICK;
    else if (g_layout == L_MENU) menu_tap(x, y);
    if (f->elem == F_FREE) f->used = 0;
}

static Finger *finger(SDL_FingerID id) {
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].id == id) return &g_f[i];
    return NULL;
}

static void finger_move(SDL_FingerID id, float x, float y) {
    Finger *f = finger(id);
    if (!f) return;
    f->x = x;
    f->y = y;
    if (f->elem == E_STICK) {               /* the centre follows a thumb beyond the stick's reach */
        if (x - f->ox > g_stick_r) f->ox = x - g_stick_r;
        if (x - f->ox < -g_stick_r) f->ox = x + g_stick_r;
    } else if (f->elem == E_GAS || f->elem == E_BRAKE) {
        int e = hit(x, y, 1);
        if (e >= 0 && e != f->elem) press(f, e);
    }
}

static void finger_up(SDL_FingerID id) {
    Finger *f = finger(id);
    if (f) f->used = 0;
}

int touch_event(SDL_Window *win, const SDL_Event *ev) {
    if (!g_on) return 0;
    int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    switch (ev->type) {
    case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: {
        /* screens only: a trackpad's fingers are not on the picture */
        if (SDL_GetTouchDeviceType(ev->tfinger.touchId) != SDL_TOUCH_DEVICE_DIRECT) return 0;
        float x = ev->tfinger.x * ww, y = ev->tfinger.y * wh;
        static int first = 1;
        if (first && ev->type == SDL_FINGERDOWN) { first = 0; rt_log("touch: first finger at %.2f, %.2f\n", ev->tfinger.x, ev->tfinger.y); }
        if (ev->type == SDL_FINGERDOWN) finger_down(ev->tfinger.fingerId, x, y);
        else if (ev->type == SDL_FINGERMOTION) finger_move(ev->tfinger.fingerId, x, y);
        else finger_up(ev->tfinger.fingerId);
        return 1;
    }
    default:
        return 0;
    }
}

void touch_frame(SDL_Window *win) {
    if (!g_on) return;
    int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    int lay = current_layout();
    if (lay != g_layout)                   /* a new screen: what the fingers held is gone */
        for (int i = 0; i < MAX_FINGERS; i++)
            if (g_f[i].used) g_f[i].elem = F_DEAD;
    g_layout = lay;
    layout(lay, (float)ww, (float)wh);
    if (g_script_pos < g_nscript) script_tick((float)ww, (float)wh);
    Uint32 now = SDL_GetTicks();
    for (int i = 0; i < MAX_FINGERS; i++) {
        Finger *f = &g_f[i];
        if (!f->used || f->elem < 0 || f->elem == E_STICK || !g_e[f->elem].on) continue;
        if (kind_of(f->elem) == K_REPEAT && SDL_TICKS_PASSED(now, f->repeat_at)) {
            fire(f->elem);
            f->repeat_at = now + 110;
        }
    }
}

void touch_output(TouchOut *out) {
    memset(out, 0, sizeof *out);
    if (!g_on) return;
    int held[E_COUNT] = { 0 };
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].elem >= 0 && g_e[g_f[i].elem].on) held[g_f[i].elem] = 1;
    int s = stick_finger();
    if (s >= 0 && g_e[E_STICK].on) {
        double v = (g_f[s].x - g_f[s].ox) / g_stick_r;
        out->steer_on = 1;
        out->steer = v < -1 ? -1 : v > 1 ? 1 : v;
    }
    out->gas = held[E_GAS] || (g_layout == L_NAME && held[E_OK]);
    out->brake = held[E_BRAKE];
    out->shift_up = held[E_SHIFT_UP];
    out->shift_down = held[E_SHIFT_DOWN];
    out->start = held[E_VIEW];
    out->coin = held[E_COIN];
    out->keyboard = g_keyboard;
    g_keyboard = 0;
}

/* ------------------------------------------------------------------ atlas */
typedef struct { int x, y, w, h; } Sprite;
static Sprite g_sp[SP_COUNT];
static uint32_t *g_atlas;
static int g_aw, g_ah;
static unsigned g_atlas_version;
static float g_atlas_px;                    /* the unit in pixels the atlas was drawn for */

/* "over" onto a premultiplied pixel: colour rgb (0xRRGGBB) at opacity a (0..1) */
static void over(uint32_t *d, uint32_t rgb, float a) {
    if (a <= 0.002f) return;
    if (a > 1) a = 1;
    uint32_t s = *d;
    float k = 1 - a;
    int A = (int)(a * 255 + (s >> 24) * k + 0.5f);
    int R = (int)(((rgb >> 16) & 255) * a + ((s >> 16) & 255) * k + 0.5f);
    int G = (int)(((rgb >> 8) & 255) * a + ((s >> 8) & 255) * k + 0.5f);
    int B = (int)((rgb & 255) * a + (s & 255) * k + 0.5f);
    *d = (uint32_t)(A > 255 ? 255 : A) << 24 | (uint32_t)(R > 255 ? 255 : R) << 16 | (uint32_t)(G > 255 ? 255 : G) << 8 |
         (uint32_t)(B > 255 ? 255 : B);
}

static float cover(float d) { return d < -0.5f ? 1 : d > 0.5f ? 0 : 0.5f - d; }   /* signed distance -> coverage */
static uint32_t *spx(int sp, int x, int y) { return &g_atlas[(g_sp[sp].y + y) * g_aw + g_sp[sp].x + x]; }

/* a disc (fill) with a ring (its outline), or a rounded rectangle (radius rr) */
static void draw_shape(int sp, int round_rect, float rr, uint32_t fill, float fa, uint32_t line, float la, float t) {
    int w = g_sp[sp].w, h = g_sp[sp].h;
    float hw = w / 2.0f - 1, hh = h / 2.0f - 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float px = x + 0.5f - w / 2.0f, py = y + 0.5f - h / 2.0f, d;
            if (round_rect) {
                float qx = fabsf(px) - (hw - rr), qy = fabsf(py) - (hh - rr);
                d = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - rr;
            } else d = hypotf(px, py) - hw;
            uint32_t *p = spx(sp, x, y);
            over(p, fill, fa * cover(d));
            if (la > 0) over(p, line, la * cover(fabsf(d + t / 2) - t / 2));
        }
}

/* a convex polygon (n points, in 0..1 of the sprite) in white */
static void draw_poly(int sp, const float *pt, int n, float alpha) {
    int w = g_sp[sp].w, h = g_sp[sp].h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float px = x + 0.5f, py = y + 0.5f, d = -1e9f;
            for (int i = 0; i < n; i++) {
                float ax = pt[2 * i] * w, ay = pt[2 * i + 1] * h;
                float bx = pt[2 * ((i + 1) % n)] * w, by = pt[2 * ((i + 1) % n) + 1] * h;
                float ex = bx - ax, ey = by - ay, len = hypotf(ex, ey);
                float side = (ex * (py - ay) - ey * (px - ax)) / len;     /* > 0 inside (clockwise on screen) */
                d = fmaxf(d, -side);
            }
            over(spx(sp, x, y), 0xffffff, alpha * cover(d));
        }
}

static void draw_box(int sp, float x0, float y0, float x1, float y1, float alpha) {
    float pt[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    draw_poly(sp, pt, 4, alpha);
}

/* a bar from (x0, y0) to (x1, y1), half as thick as t (in 0..1 of the sprite) */
static void draw_bar(int sp, float x0, float y0, float x1, float y1, float t) {
    float dx = x1 - x0, dy = y1 - y0, len = hypotf(dx, dy), nx = -dy / len * t, ny = dx / len * t;
    float pt[8] = { x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, x0 + nx, y0 + ny };
    draw_poly(sp, pt, 4, 0.95f);
}

static void draw_arrow(int sp, int dir) {           /* 0 up, 1 down, 2 left, 3 right */
    static const float up[6] = { 0.5f, 0.2f, 0.84f, 0.72f, 0.16f, 0.72f };
    float pt[6];
    for (int i = 0; i < 3; i++) {
        float x = up[2 * i], y = up[2 * i + 1];
        if (dir == 1) { y = 1 - y; x = 1 - x; }     /* turned by 180 degrees: still clockwise */
        else if (dir == 2) { float t = x; x = y; y = 1 - t; }
        else if (dir == 3) { float t = x; x = 1 - y; y = t; }
        pt[2 * i] = x;
        pt[2 * i + 1] = y;
    }
    draw_poly(sp, pt, 3, 0.95f);
}

static const char *const k_label[SP_COUNT] = {
    [SP_TXT_GAS] = "GAS", [SP_TXT_BRAKE] = "BRAKE", [SP_TXT_VIEW] = "VIEW", [SP_TXT_COIN] = "COIN",
    [SP_TXT_OK] = "OK", [SP_TXT_BACK] = "BACK", [SP_TXT_DEL] = "DEL", [SP_TXT_END] = "END",
};
static const uint32_t k_label_rgb[SP_COUNT] = {
    [SP_TXT_GAS] = 0x70ff70, [SP_TXT_BRAKE] = 0xff7060, [SP_TXT_VIEW] = 0xffffff, [SP_TXT_COIN] = 0xffffff,
    [SP_TXT_OK] = 0xffd800, [SP_TXT_BACK] = 0xffffff, [SP_TXT_DEL] = 0xffffff, [SP_TXT_END] = 0xffffff,
};

static void build_atlas(float px) {
    int btn = (int)(22 * px + 0.5f), icon = btn / 2, label_h = (int)(btn * 0.3f + 0.5f);
    int size[SP_COUNT][2] = {
        [SP_BTN] = { btn, btn }, [SP_BTN_ON] = { btn, btn },
        [SP_PEDAL] = { (int)(19 * px + 0.5f), (int)(32 * px + 0.5f) }, [SP_PEDAL_ON] = { (int)(19 * px + 0.5f), (int)(32 * px + 0.5f) },
        [SP_RING] = { (int)(36 * px + 0.5f), (int)(36 * px + 0.5f) }, [SP_KNOB] = { (int)(16 * px + 0.5f), (int)(16 * px + 0.5f) },
    };
    for (int s = SP_ARROW_U; s <= SP_KEYS; s++) size[s][0] = size[s][1] = icon;   /* (SP_EXIT among them) */
    for (int s = SP_TXT_GAS; s < SP_COUNT; s++) {
        int w = 0;
        size[s][1] = label_h;
        size[s][0] = enh_label_width(k_label[s], label_h, &w) ? w : 0;
    }
    /* shelves, 2 pixels apart (the sprites are drawn scaled, filtered) */
    int aw = 2048, x = 2, y = 2, shelf = 0;
    for (int s = 0; s < SP_COUNT; s++) {
        int w = size[s][0], h = size[s][1];
        if (w <= 0 || h <= 0) { g_sp[s] = (Sprite){ 0, 0, 0, 0 }; continue; }
        if (x + w + 2 > aw) { x = 2; y += shelf + 4; shelf = 0; }
        g_sp[s] = (Sprite){ x, y, w, h };
        x += w + 4;
        if (h > shelf) shelf = h;
    }
    int ah = y + shelf + 2;
    free(g_atlas);
    g_atlas = calloc((size_t)aw * ah, 4);
    g_aw = aw;
    g_ah = ah;
    float t = fmaxf(2.0f, btn * 0.045f);
    draw_shape(SP_BTN, 0, 0, 0x101010, 0.40f, 0xffffff, 0.80f, t);
    draw_shape(SP_BTN_ON, 0, 0, 0xffc000, 0.55f, 0xffffff, 0.95f, t);
    float rr = g_sp[SP_PEDAL].w * 0.2f;
    draw_shape(SP_PEDAL, 1, rr, 0x101010, 0.40f, 0xffffff, 0.80f, t);
    draw_shape(SP_PEDAL_ON, 1, rr, 0xffc000, 0.55f, 0xffffff, 0.95f, t);
    draw_shape(SP_RING, 0, 0, 0x000000, 0.18f, 0xffffff, 0.55f, t * 1.4f);
    draw_shape(SP_KNOB, 0, 0, 0xffffff, 0.55f, 0xffffff, 0.95f, t);
    draw_arrow(SP_ARROW_U, 0);
    draw_arrow(SP_ARROW_D, 1);
    draw_arrow(SP_ARROW_L, 2);
    draw_arrow(SP_ARROW_R, 3);
    draw_box(SP_PAUSE, 0.26f, 0.2f, 0.42f, 0.8f, 0.95f);
    draw_box(SP_PAUSE, 0.58f, 0.2f, 0.74f, 0.8f, 0.95f);
    draw_bar(SP_EXIT, 0.27f, 0.27f, 0.73f, 0.73f, 0.07f);      /* an X: leave a linked race */
    draw_bar(SP_EXIT, 0.73f, 0.27f, 0.27f, 0.73f, 0.07f);
    {   /* a keyboard: its outline, two rows of keys and a space bar */
        draw_box(SP_KEYS, 0.08f, 0.24f, 0.92f, 0.30f, 0.95f);
        draw_box(SP_KEYS, 0.08f, 0.70f, 0.92f, 0.76f, 0.95f);
        draw_box(SP_KEYS, 0.08f, 0.24f, 0.14f, 0.76f, 0.95f);
        draw_box(SP_KEYS, 0.86f, 0.24f, 0.92f, 0.76f, 0.95f);
        for (int r = 0; r < 2; r++)
            for (int k = 0; k < 4; k++) {
                float kx = 0.22f + k * 0.15f, ky = 0.37f + r * 0.13f;
                draw_box(SP_KEYS, kx, ky, kx + 0.09f, ky + 0.08f, 0.95f);
            }
        draw_box(SP_KEYS, 0.32f, 0.61f, 0.68f, 0.66f, 0.95f);
    }
    for (int s = SP_TXT_GAS; s < SP_COUNT; s++) {
        Sprite *p = &g_sp[s];
        if (!p->w) continue;
        uint32_t *tmp = calloc((size_t)p->w * p->h, 4);
        enh_label_draw(tmp, p->w, p->h, k_label[s], k_label_rgb[s]);
        for (int r = 0; r < p->h; r++) memcpy(spx(s, 0, r), tmp + (size_t)r * p->w, (size_t)p->w * 4);
        free(tmp);
    }
    g_atlas_px = px;
    g_atlas_version++;
}

const uint32_t *touch_atlas(int *w, int *h, unsigned *version) {
    *w = g_aw;
    *h = g_ah;
    *version = g_atlas_version;
    return g_atlas;
}

/* ------------------------------------------------------------------ drawing */
static int quad(UiQuad *q, int n, int max, int sp, float cx, float cy, float w, float h, float k, float a) {
    const Sprite *p = &g_sp[sp];
    if (n >= max || !p->w) return n;
    q[n] = (UiQuad){ (cx - w / 2) * k, (cy - h / 2) * k, w * k, h * k, (float)p->x / g_aw, (float)p->y / g_ah,
                     (float)(p->x + p->w) / g_aw, (float)(p->y + p->h) / g_ah, a };
    return n + 1;
}

int touch_quads(SDL_Window *win, UiQuad *q, int max) {
    if (!g_on || !g_visible || g_layout == L_NONE) return 0;
    int ww, wh, dw, dh;
    SDL_GetWindowSize(win, &ww, &wh);
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_OPENGL) SDL_GL_GetDrawableSize(win, &dw, &dh);
    else if (SDL_GetRenderer(win)) SDL_GetRendererOutputSize(SDL_GetRenderer(win), &dw, &dh);
    else { dw = ww; dh = wh; }
    if (ww <= 0 || wh <= 0) return 0;
    float k = (float)dw / ww, px = g_unit * k;
    if (!g_atlas || fabsf(px - g_atlas_px) > 0.25f) build_atlas(px);
    int held[E_COUNT] = { 0 };
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_f[i].used && g_f[i].elem >= 0) held[g_f[i].elem] = 1;
    int n = 0;
    for (int e = 0; e < E_COUNT; e++) {
        const Elem *p = &g_e[e];
        if (!p->on || e == E_STICK) continue;
        int on = held[e];
        float a = on ? 1.0f : 0.75f;
        n = quad(q, n, max, p->rect ? (on ? SP_PEDAL_ON : SP_PEDAL) : (on ? SP_BTN_ON : SP_BTN), p->x, p->y, p->w, p->h, k, a);
        int ic = k_icon[e];
        if (e == E_PAUSE && net_active() && !net_session_over()) ic = SP_EXIT;   /* linked: no pause, leave */
        if (ic < 0 || !g_sp[ic].w) continue;
        float iw, ih;
        if (ic >= SP_TXT_GAS) {            /* a label: its height from the element, narrower than it */
            ih = (p->rect ? p->w : p->h) * 0.3f;
            iw = ih * g_sp[ic].w / g_sp[ic].h;
            float maxw = p->w * 0.8f;
            if (iw > maxw) { ih *= maxw / iw; iw = maxw; }
        } else iw = ih = fminf(p->w, p->h) * 0.5f;
        n = quad(q, n, max, ic, p->x, p->y, iw, ih, k, a);
    }
    if (g_e[E_STICK].on) {
        float rr = 18 * g_unit, kr = 8 * g_unit;
        int s = stick_finger();
        if (s >= 0) {
            const Finger *f = &g_f[s];
            float dx = f->x - f->ox;
            dx = dx < -g_stick_r ? -g_stick_r : dx > g_stick_r ? g_stick_r : dx;
            n = quad(q, n, max, SP_RING, f->ox, f->oy, 2 * rr, 2 * rr, k, 1.0f);
            n = quad(q, n, max, SP_KNOB, f->ox + dx, f->oy, 2 * kr, 2 * kr, k, 1.0f);
        } else if (frontend_steering_mode() == 0) {     /* tilt steering: no stick at rest */
            n = quad(q, n, max, SP_RING, g_idle_x, g_idle_y, 2 * rr, 2 * rr, k, 0.6f);
            n = quad(q, n, max, SP_ARROW_L, g_idle_x - rr * 0.62f, g_idle_y, rr * 0.45f, rr * 0.45f, k, 0.5f);
            n = quad(q, n, max, SP_ARROW_R, g_idle_x + rr * 0.62f, g_idle_y, rr * 0.45f, rr * 0.45f, k, 0.5f);
            n = quad(q, n, max, SP_KNOB, g_idle_x, g_idle_y, 2 * kr, 2 * kr, k, 0.6f);
        }
    }
    return n;
}
