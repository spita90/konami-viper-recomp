/*
 * SDL2 frontend: window, real-time pacing, audio, keyboard/gamepad input.
 *
 * Threads: the host main thread runs SDL (events, presentation); the guest runs on
 * runtime fibers.  Shared state is small: input bytes/words (single writer), the video
 * frame (mutex inside the voodoo bridge) and a lock-free audio ring buffer.
 *
 * Controls (driving-game inputs as in MAME's viper.cpp "thrild2" / "gticlub2"):
 *   Left/Right or A/D  steering   Up or W  gas   Down or S  brake   Space  handbrake (GTI Club 2)
 *   E  shift up   Q  shift down   5  coin   1  start   F2  test   9  service
 *   (enhanced mode: test, service and coin are not passed to the game: TEST MODE cannot be
 *   opened, and the game is on free play. In the rankings' name entry the keyboard types the
 *   letters, Backspace deletes, Enter ends, Left/Right and the D-pad step through the letters;
 *   in the course select (and GTI Club 2's transmission select) Left/Right, A/D, the D-pad
 *   and the stick step through the choices, and the wheel stays on the chosen one)
 *   Gamepad: left stick = steering, R2/L2 = gas/brake, R1/L1 = shift up/down,
 *            X = handbrake, Start = start, Back = coin
 *   F11 fullscreen, Esc quit
 */
#include "runtime.h"
#include "game_config.h"
#include "controller_math.h"
#include <SDL.h>
#include "window_state.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t g_in[8];
extern int16_t g_analog[4];
#define ANALOG_RANGE 200     /* keep clear of ADC saturation; matches tools/calibrate.sh */

/* ------------------------------------------------------------------ audio ring (SPSC) */
#define AUDIO_RING (1 << 15)              /* stereo frames */
static int16_t g_ring[AUDIO_RING][2];
static atomic_uint g_ring_w, g_ring_r;
static SDL_AudioDeviceID g_audio;
int g_audio_gain = 16;                    /* samples peak ~1.5% FS; the cabinet has a power amp */

static int16_t sat16(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static int g_frontend_active;

void audio_frontend_push(const uint8_t *blk) {
    if (!g_audio || enh_turbo()) return;       /* muted while the enhanced mode fast-forwards */
    unsigned w = atomic_load_explicit(&g_ring_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&g_ring_r, memory_order_acquire);
    for (int i = 0; i < 256; i++) {
        if (w - r >= AUDIO_RING - 1) break;           /* full: drop */
        const uint8_t *p = blk + 8 * i;
        int32_t l = (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
        int32_t rr = (int32_t)(((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7]);
        g_ring[w % AUDIO_RING][0] = sat16(((int64_t)l * g_audio_gain) >> 16);
        g_ring[w % AUDIO_RING][1] = sat16(((int64_t)rr * g_audio_gain) >> 16);
        w++;
    }
    atomic_store_explicit(&g_ring_w, w, memory_order_release);
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    unsigned r = atomic_load_explicit(&g_ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&g_ring_w, memory_order_acquire);
    static int16_t last[2];
    if (enh_paused()) { memset(stream, 0, (size_t)len); return; }
    for (int i = 0; i < frames; i++) {
        if (r != w) {
            last[0] = g_ring[r % AUDIO_RING][0];
            last[1] = g_ring[r % AUDIO_RING][1];
            r++;
        }
        out[2 * i] = last[0];
        out[2 * i + 1] = last[1];
    }
    /* keep latency bounded: if far behind, skip ahead */
    if (w - r > 44100 / 5) r = w - 44100 / 20;
    atomic_store_explicit(&g_ring_r, r, memory_order_release);
}

/* ------------------------------------------------------------------ pacing (guest thread) */
static uint64_t g_pace_t0;          /* host ticks at virtual time 0 */
static double g_pace_freq;

void rt_pace_vblank(void) {
    if (!g_frontend_active) return;
    double virt = (double)rt_now() / CPU_HZ;
    if (enh_turbo()) {                  /* enhanced mode boot/apply: as fast as possible */
        g_pace_t0 = SDL_GetPerformanceCounter() - (uint64_t)(virt * g_pace_freq);
        return;
    }
    if (enh_paused()) {                 /* enhanced mode pause: the game waits here */
        while (enh_paused() && g_frontend_active) SDL_Delay(5);
        g_pace_t0 = SDL_GetPerformanceCounter() - (uint64_t)(virt * g_pace_freq);
        return;
    }
    for (;;) {
        double real = (double)(SDL_GetPerformanceCounter() - g_pace_t0) / g_pace_freq;
        double ahead = virt - real;
        if (ahead <= 0.0005) {
            if (ahead < -0.25) g_pace_t0 = SDL_GetPerformanceCounter() - (uint64_t)(virt * g_pace_freq); /* resync after stalls */
            return;
        }
        SDL_Delay(ahead > 0.002 ? (Uint32)((ahead - 0.001) * 1000) : 0);
    }
}

/* ------------------------------------------------------------------ input */
/* each digital control may be held by several keys and buttons: one bit per source, so
 * releasing one of them does not release the others */
enum { SRC_KEY = 1, SRC_KEY2 = 2, SRC_PAD = 4 };
typedef struct {
    int steer_left, steer_right, gas, brake, handbrake;
    double steer;                   /* -1..1 (keyboard, ramped) */
    int shift_up, shift_down, coin, start, test, service;
} Controls;
static Controls ctl;

static void hold(int *f, int src, int down) { *f = down ? *f | src : *f & ~src; }
static SDL_GameController *g_pad;
static SDL_JoystickID g_pad_id = -1;
static int g_input_focus = 1;
static int g_menu_repeat_button = -1;
static Uint32 g_menu_repeat_at;
static int g_controller_log;
static Uint32 g_controller_log_tick;
static double g_stick_deadzone = 0.10, g_stick_curve = 3.0, g_trigger_deadzone = 0.03;
int frontend_stick_response(void) { return (int)lround(g_stick_curve) - 1; }
void frontend_set_stick_response(int response) {
    g_stick_curve = response >= 0 && response <= 2 ? response + 1.0 : 3.0;
}


static double controller_option(const char *name, double fallback, double lo, double hi) {
    const char *value = getenv(name);
    if (!value) return fallback;
    char *end;
    double result = strtod(value, &end);
    if (end == value || *end || !isfinite(result) || result < lo || result > hi) {
        rt_log("%s: expected a number between %g and %g; using %g\n", name, lo, hi, fallback);
        return fallback;
    }
    return result;
}

static int pad_matches(SDL_JoystickID id) { return g_pad && id == g_pad_id; }

static void close_pad(void) {
    g_menu_repeat_button = -1;
    if (g_pad) SDL_GameControllerClose(g_pad);
    g_pad = NULL;
    g_pad_id = -1;
    /* Releasing a controller must not release a held keyboard key. */
    ctl.gas &= ~SRC_PAD; ctl.brake &= ~SRC_PAD; ctl.handbrake &= ~SRC_PAD;
    ctl.shift_up &= ~SRC_PAD; ctl.shift_down &= ~SRC_PAD;
    ctl.coin &= ~SRC_PAD; ctl.start &= ~SRC_PAD;
}

static void open_pad(void) {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        g_pad = SDL_GameControllerOpen(i);
        if (!g_pad) continue;
        g_pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad));
        rt_log("gamepad: %s\n", SDL_GameControllerName(g_pad));
        break;
    }
}


static void apply_inputs(double dt) {
    /* Menu confirmation can consume a pedal button's down event. These controls
     * represent held state, so reconcile them with the device every frame. */
    int pad_active = g_pad && g_input_focus;
    hold(&ctl.gas, SRC_PAD, pad_active && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A));
    hold(&ctl.brake, SRC_PAD, pad_active && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B));
    hold(&ctl.handbrake, SRC_PAD, pad_active && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_X));

    /* keyboard steering: ramp towards target */
    int wsel = enh_wheel_select_active();
    double target = wsel ? enh_wheel_select_pos() : !!ctl.steer_right - !!ctl.steer_left;
    double speed = 4.0 * SDL_min(dt, 0.05);
    if (ctl.steer < target) ctl.steer = SDL_min(target, ctl.steer + speed);
    else if (ctl.steer > target) ctl.steer = SDL_max(target, ctl.steer - speed);
    double steer = ctl.steer;
    /* Take the strongest pedal source: a slightly pressed trigger must not reduce a
     * fully held key/button. Right-stick Y supplies proportional pedals on Switch Pro. */
    double gas = ctl.gas ? 1.0 : 0.0, brake = ctl.brake ? 1.0 : 0.0;
    if (g_pad && g_input_focus) {
        double stick = controller_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX),
                                       g_stick_deadzone, g_stick_curve);
        if (stick != 0 && !wsel) steer = stick;
        double pedals = controller_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY),
                                        g_stick_deadzone, 1.0);
        gas = fmax(gas, fmax(-pedals, controller_trigger(
            SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT), g_trigger_deadzone)));
        brake = fmax(brake, fmax(pedals, controller_trigger(
            SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT), g_trigger_deadzone)));
    }
    /* signed positions for the differential ADC: steering -200..+200, pedals released=-200 */
    if (enh_name_entry_active()) steer = 0;   /* the letters come from the keyboard */
    g_analog[0] = (int16_t)lround(steer * ANALOG_RANGE);
    if (g_controller_log && g_pad && (Uint32)(SDL_GetTicks() - g_controller_log_tick) >= 100) {
        g_controller_log_tick = SDL_GetTicks();
        rt_log("controller: left_x=%d steering_adc=%d gas=%.3f brake=%.3f\n",
               SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX), g_analog[0], gas, brake);
    }
    g_analog[1] = (int16_t)(-ANALOG_RANGE + gas * 2 * ANALOG_RANGE);
    g_analog[2] = (int16_t)(-ANALOG_RANGE + brake * 2 * ANALOG_RANGE);
    if (GAME_HAS_HANDBRAKE) g_analog[3] = (int16_t)(ctl.handbrake ? ANALOG_RANGE : -ANALOG_RANGE);
    uint8_t in3 = 0xff, in4 = 0xff;
    if (ctl.service && !g_enhanced) in3 &= ~0x01;
    if (ctl.test && !g_enhanced) in3 &= ~0x02;
    if (ctl.coin && !g_enhanced) in3 &= ~0x04;
    if (ctl.start || enh_start_held()) in3 &= ~0x10;
    if (ctl.shift_down) in3 &= ~0x40;
    if (ctl.shift_up) in4 &= ~0x01;
    if (enh_inputs_owned()) return;     /* the enhanced layer is driving TEST MODE */
    if (enh_menu_active()) {            /* the menu owns the controls: the attract gets nothing */
        in3 = enh_start_held() ? 0xef : 0xff;
        in4 = 0xff;
        g_analog[1] = g_analog[2] = (int16_t)-ANALOG_RANGE;
    }
    g_in[3] = in3;
    g_in[4] = in4;
}

/* enhanced mode: keys and buttons that drive the attract menu while it is on screen */
static int menu_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: case SDLK_w: return ENH_UP;
    case SDLK_DOWN: case SDLK_s: return ENH_DOWN;
    case SDLK_LEFT: case SDLK_a: return ENH_LEFT;
    case SDLK_RIGHT: case SDLK_d: return ENH_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_1: return ENH_OK;
    case SDLK_BACKSPACE: return ENH_BACK;
    default: return -1;
    }
}

static int menu_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return ENH_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ENH_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ENH_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ENH_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: return ENH_OK;
    case SDL_CONTROLLER_BUTTON_B: return ENH_BACK;
    default: return -1;
    }
}

/* Repeat only value adjustments; confirmation must never repeat into another page. */
static void menu_pad_press(int button, Uint32 now) {
    int action = menu_button(button);
    g_menu_repeat_button = (action == ENH_LEFT || action == ENH_RIGHT) ? button : -1;
    g_menu_repeat_at = now + 400;
    enh_menu_action(action);
}

static void menu_pad_repeat(Uint32 now) {
    if (!g_input_focus || !(enh_menu_active() || enh_paused())) g_menu_repeat_button = -1;
    if (g_menu_repeat_button >= 0 && (Sint32)(now - g_menu_repeat_at) >= 0) {
        enh_menu_action(menu_button(g_menu_repeat_button));
        g_menu_repeat_at = now + 80;
    }
}

/* enhanced mode, name entry: 1 if the key is the name entry's (the characters themselves come
 * as SDL_TEXTINPUT, which follows the keyboard layout) */
static int name_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_BACKSPACE: enh_name_type('\b'); return 1;
    case SDLK_RETURN: case SDLK_KP_ENTER: enh_name_type('\r'); return 1;
    case SDLK_LEFT: enh_name_step(-1); return 1;
    case SDLK_RIGHT: enh_name_step(1); return 1;
    default: return (k >= SDLK_SPACE && k <= SDLK_z) || (k >= SDLK_KP_1 && k <= SDLK_KP_PERIOD);
    }
}

static void key(SDL_Keycode k, int down) {
    switch (k) {
    case SDLK_LEFT: hold(&ctl.steer_left, SRC_KEY, down); break;
    case SDLK_RIGHT: hold(&ctl.steer_right, SRC_KEY, down); break;
    case SDLK_UP: hold(&ctl.gas, SRC_KEY, down); break;
    case SDLK_DOWN: hold(&ctl.brake, SRC_KEY, down); break;
    case SDLK_a: hold(&ctl.steer_left, SRC_KEY2, down); break;
    case SDLK_d: hold(&ctl.steer_right, SRC_KEY2, down); break;
    case SDLK_w: hold(&ctl.gas, SRC_KEY2, down); break;
    case SDLK_s: hold(&ctl.brake, SRC_KEY2, down); break;
    case SDLK_SPACE: hold(&ctl.handbrake, SRC_KEY, down); break;
    case SDLK_e: hold(&ctl.shift_up, SRC_KEY, down); break;
    case SDLK_q: hold(&ctl.shift_down, SRC_KEY, down); break;
    case SDLK_5: hold(&ctl.coin, SRC_KEY, down); break;
    case SDLK_1: hold(&ctl.start, SRC_KEY, down); break;
    case SDLK_F2: ctl.test = down; break;
    case SDLK_9: ctl.service = down; break;
    default: break;
    }
}

static void pad_button(int b, int down) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: hold(&ctl.shift_up, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: hold(&ctl.shift_down, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_START: hold(&ctl.start, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_BACK: hold(&ctl.coin, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_A: hold(&ctl.gas, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_B: hold(&ctl.brake, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_X: hold(&ctl.handbrake, SRC_PAD, down); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ main loop */
void nvram_save(void);

/* the normal window only: fullscreen, maximized and minimized bounds are not kept */
static void save_window(SDL_Window *win) {
    if (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) return;
    int r[4];
    SDL_GetWindowPosition(win, &r[0], &r[1]);
    SDL_GetWindowSize(win, &r[2], &r[3]);
    enh_set_window(r);
}

int frontend_run(int scale, int scale_explicit) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        rt_log("SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    /* the window of the last run (port settings), fitted to the displays connected now; an
     * explicit --scale keeps only its position (not after an enhanced-mode restart, which runs
     * again with the same arguments and continues the session) */
    SDL_Rect window_rect = { SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 512 * scale, 384 * scale };
    int saved[4];
    if (enh_want_window(saved)) {
        window_rect.x = saved[0];
        window_rect.y = saved[1];
        if (!scale_explicit || getenv("RT_RESTARTED")) { window_rect.w = saved[2]; window_rect.h = saved[3]; }
        int count = SDL_GetNumVideoDisplays(), usable = 0;
        SDL_Rect *displays = count > 0 ? calloc((size_t)count, sizeof *displays) : NULL;
        if (displays) {
            for (int i = 0; i < count; i++)
                if (SDL_GetDisplayUsableBounds(i, &displays[usable]) == 0 || SDL_GetDisplayBounds(i, &displays[usable]) == 0)
                    usable++;
            window_state_fit(&window_rect, displays, usable);
            free(displays);
        }
    }
    SDL_Window *win = SDL_CreateWindow(g_enhanced ? GAME_TITLE " - enhanced" : GAME_TITLE,
        window_rect.x, window_rect.y, window_rect.w, window_rect.h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { rt_log("SDL_CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return -1; }
    SDL_SetWindowMinimumSize(win, 320, 240);
    int window_dirty = 0;               /* moved or resized: saved 0.5 s after the last change */
    Uint32 window_changed = 0;
    if (getenv("RT_RESTARTED")) {       /* enhanced mode, after a restart: macOS does not reactivate */
        unsetenv("RT_RESTARTED");       /* the re-executed program, so take the focus back */
        SDL_SetHint("SDL_FORCE_RAISEWINDOW", "1");
        SDL_RaiseWindow(win);
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, 512, 384);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 512, 384);
    int tw = 512, th = 384;
    int window_filter_applied = SDL_ScaleModeLinear;   /* the textures are made linear (the hint) */

    SDL_AudioSpec want = {0}, have;
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else rt_log("audio: %s\n", SDL_GetError());

    g_stick_deadzone = controller_option("RT_STICK_DEADZONE", 0.10, 0.0, 0.5);
    g_stick_curve = controller_option("RT_STICK_CURVE", g_stick_curve, 1.0, 3.0);
    g_trigger_deadzone = controller_option("RT_TRIGGER_DEADZONE", 0.03, 0.0, 0.5);
    open_pad();

    g_pace_freq = (double)SDL_GetPerformanceFrequency();
    g_pace_t0 = SDL_GetPerformanceCounter() - (uint64_t)((double)rt_now() / CPU_HZ * g_pace_freq);
    g_frontend_active = 1;

    static uint32_t frame[2048 * 2048], raw[2048 * 2048];
    uint64_t last_frame = 0, last_tick = SDL_GetPerformanceCounter(), last_save = last_tick;
    int running = 1, fs_applied = 0, restart = 0, text_on = 1;
    while (running) {
        /* text input only for the name entry: SDL starts it with the video, and while it is on
         * macOS opens its accent picker on a held letter key (W, A, S, D while driving) */
        if (enh_name_entry_active() != text_on) {
            text_on = enh_name_entry_active();
            if (text_on) SDL_StartTextInput(); else SDL_StopTextInput();
        }
        if (enh_want_fullscreen() != fs_applied) {      /* enhanced mode: DISPLAY option */
            fs_applied = enh_want_fullscreen();
            SDL_SetWindowFullscreen(win, fs_applied ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    g_input_focus = 0;
                    g_menu_repeat_button = -1;
                    memset(&ctl, 0, sizeof ctl);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_input_focus = 1;
                if (ev.window.event == SDL_WINDOWEVENT_MOVED || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    window_dirty = 1;
                    window_changed = SDL_GetTicks();
                }
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_F9) {
                    if (!ev.key.repeat) {
                        g_controller_log = !g_controller_log;
                        rt_log("controller input logging: %s\n", g_controller_log ? "on" : "off");
                    }
                    break;
                }
                if ((enh_menu_active() || enh_paused()) && menu_key(ev.key.keysym.sym) >= 0) {
                    int action = menu_key(ev.key.keysym.sym);
                    g_menu_repeat_button = -1;
                    if (!ev.key.repeat || action == ENH_LEFT || action == ENH_RIGHT) enh_menu_action(action);
                } else if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (!ev.key.repeat && !enh_escape()) running = 0;   /* enhanced: pause / back */
                }
                else if (ev.key.keysym.sym == SDLK_F11) {
                    Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    enh_set_fullscreen(!fs);    /* enhanced mode: remembered in the settings */
                    fs_applied = !fs;
                } else if (!(enh_name_entry_active() && !enh_paused() && name_key(ev.key.keysym.sym))) {
                    if (!ev.key.repeat && !enh_paused() && enh_wheel_select_active()) {
                        SDL_Keycode k = ev.key.keysym.sym;
                        if (k == SDLK_LEFT || k == SDLK_a) enh_wheel_select_step(-1);
                        else if (k == SDLK_RIGHT || k == SDLK_d) enh_wheel_select_step(1);
                    }
                    key(ev.key.keysym.sym, 1);
                }
                break;
            case SDL_KEYUP: key(ev.key.keysym.sym, 0); break;
            case SDL_TEXTINPUT:
                if (enh_name_entry_active() && !enh_paused())
                    for (const char *p = ev.text.text; *p; p++) enh_name_type((unsigned char)*p);
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (pad_matches(ev.caxis.which) && g_input_focus && ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    static int zone;
                    int z=ev.caxis.value < -16000 ? -1 : ev.caxis.value > 16000 ? 1 : 0;
                    if (z && z != zone && !enh_paused()) enh_wheel_select_step(z);
                    zone=z;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: open_pad(); break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (pad_matches(ev.cdevice.which)) { close_pad(); open_pad(); }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (pad_matches(ev.cbutton.which)) g_menu_repeat_button = -1;
                if (!pad_matches(ev.cbutton.which) || !g_input_focus) break;
                if ((enh_menu_active() || enh_paused()) && menu_button(ev.cbutton.button) >= 0) menu_pad_press(ev.cbutton.button, SDL_GetTicks());
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) { if (!enh_escape()) running=0; }
                else if (enh_name_entry_active() && !enh_paused() && (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                                                                       ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
                    enh_name_step(ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);
                else if (enh_wheel_select_active() && !enh_paused() && (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                                                                          ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
                    enh_wheel_select_step(ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);
                else pad_button(ev.cbutton.button, 1);
                break;
            case SDL_CONTROLLERBUTTONUP:
                if (pad_matches(ev.cbutton.which)) {
                    if (g_menu_repeat_button == ev.cbutton.button) g_menu_repeat_button = -1;
                    pad_button(ev.cbutton.button, 0);
                }
                break;
            default: break;
            }
        }
        menu_pad_repeat(SDL_GetTicks());
        uint64_t now = SDL_GetPerformanceCounter();
        apply_inputs((double)(now - last_tick) / g_pace_freq);
        last_tick = now;
        if ((double)(now - last_save) / g_pace_freq > 60.0) { nvram_save(); last_save = now; }
        if (enh_quit_requested()) running = 0;
        if (enh_restart_requested()) { running = 0; restart = 1; }

        int w, h;
        uint64_t cnt = voodoo_get_frame(NULL, 0, &w, &h);
        int fresh = cnt != last_frame && w > 0 && h > 0;
        if (fresh) {
            last_frame = cnt;
            voodoo_get_frame(raw, 2048 * 2048, &w, &h);
            if (w != tw || h != th) {
                /* a new aspect ratio (enhanced mode, widescreen): the window keeps its height,
                 * unless it already has that ratio (a window restored from the settings) */
                if ((long)w * th != (long)h * tw && !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
                    int ww, wh;
                    SDL_GetWindowSize(win, &ww, &wh);
                    if ((long)w * wh != (long)h * ww) SDL_SetWindowSize(win, (int)((long)wh * w / h), wh);
                }
                SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                window_filter_applied = SDL_ScaleModeLinear;
                SDL_RenderSetLogicalSize(ren, w, h);
                tw = w; th = h;
            }
        }
        /* the overlay is redrawn over the last game frame every loop, so the enhanced menus
         * respond while the game is paused */
        if (fresh || (g_enhanced && last_frame)) {
            memcpy(frame, raw, (size_t)tw * th * 4);
            enh_draw_overlay(frame, tw, th);
            SDL_UpdateTexture(tex, NULL, frame, tw * 4);
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        int window_filter = enh_texture_filter() ? SDL_ScaleModeNearest : SDL_ScaleModeLinear;
        if (window_filter != window_filter_applied) {
            SDL_SetTextureScaleMode(tex, (SDL_ScaleMode)window_filter);
            window_filter_applied = window_filter;
        }
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);       /* vsync paces this loop */
        if (window_dirty && (Uint32)(SDL_GetTicks() - window_changed) >= 500) {
            save_window(win);
            window_dirty = 0;
        }
    }
    if (window_dirty) save_window(win);
    nvram_save();
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    close_pad();
    SDL_Quit();
    return restart ? 2 : 0;
}
