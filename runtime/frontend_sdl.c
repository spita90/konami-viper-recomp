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
 *   Gamepad: left stick or D-pad = steering, R2/L2 = gas/brake, R1/L1 = shift up/down,
 *            X = brake, B = handbrake, Start = start, Back = coin (enhanced: pause, as Guide, which
 *            Steam and the consoles often keep for themselves)
 *   F11 fullscreen, Esc quit
 */
#include "runtime.h"
#include "game_config.h"
#include "controller_math.h"
#include <SDL.h>
#ifdef VIPER_NATIVE_HAPTICS
#include "controller_haptics_mac.h"
static int send_controller_rumble(SDL_GameController *pad, Uint16 low, Uint16 high, Uint32 duration);
#define CONTROLLER_RUMBLE_SEND send_controller_rumble
#endif
#include "controller_rumble.h"
#include "window_state.h"
#ifdef VIPER_IOS
#include "touch.h"
#include "platform_ios.h"
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *g_rumble_backend = "SDL";
#ifdef VIPER_NATIVE_HAPTICS
static int send_controller_rumble(SDL_GameController *pad, Uint16 low, Uint16 high, Uint32 duration) {
    const char *backend = getenv("RT_RUMBLE_BACKEND");
    if (SDL_NumJoysticks() == 1 && SDL_GameControllerGetType(pad) == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO &&
        (backend && !strcmp(backend, "apple"))) {
        int result = controller_haptics_rumble((float)SDL_max(low, high) / 65535.0f, duration / 1000.0);
        if (result > 0) { g_rumble_backend = "Apple"; return 0; }
        if (result < 0) { g_rumble_backend = "Apple retry"; return SDL_SetError("Apple controller haptics temporarily unavailable"); }
        /* Native discovery may be delayed or the device may not expose haptics. */
    }
    controller_haptics_stop();
    g_rumble_backend = "SDL";
    return SDL_GameControllerRumble(pad, low, high, duration);
}
#endif

extern uint8_t g_in[8];
extern int16_t g_analog[4];
#define ANALOG_RANGE 200     /* keep clear of ADC saturation; matches tools/calibrate.sh */

/* ------------------------------------------------------------------ audio ring (SPSC) */
#define AUDIO_RING (1 << 15)              /* stereo frames */
static int16_t g_ring[AUDIO_RING][2];
static atomic_uint g_ring_w, g_ring_r;
static SDL_AudioDeviceID g_audio;
int g_audio_gain = 1;                     /* the game's samples as they are: volume 18 peaks at 57-81% (hw.c) */

/* GPU renderer (gpu_gl.cpp) */
#ifdef VIPER_IOS
void gpu_gl_ui(const uint32_t *atlas, int aw, int ah, unsigned version, const UiQuad *q, int n);
#endif
int gpu_gl_init(SDL_Window *win, uint32_t vram_size);
void gpu_gl_frame(SDL_Window *win, const uint32_t *overlay, int ow, int oh, uint64_t show_at);
void gpu_gl_shutdown(void);
const char *gpu_gl_name(void);
int gpu_gl_max_texture(void);

static int16_t sat16(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static int g_frontend_active;
#ifdef VIPER_IOS
/* the app in the background: no drawing, the game waits in its pacing, the sound is paused */
static atomic_int g_suspended, g_resumed;
#define app_suspended() atomic_load(&g_suspended)
static char g_gpu_guard[1024];          /* the GPU renderer is starting (see frontend_run) */
static int g_gpu_guard_frames = -1;     /* frames shown since, -1 when not guarding */
static void gpu_guard_off(void) { if (g_gpu_guard_frames >= 0) { remove(g_gpu_guard); g_gpu_guard_frames = -1; } }
#else
#define app_suspended() 0
#endif

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

/* RT_FPS_STATS: audio frames the game had not made in time (the sound breaks up), and how far the
 * game fell behind real time (ms) and how often it was resynchronized */
static _Atomic unsigned g_audio_short, g_behind_ms, g_resyncs;
void frontend_diag_take(unsigned *audio_short, unsigned *behind_ms, unsigned *resyncs) {
    *audio_short = atomic_exchange(&g_audio_short, 0);
    *behind_ms = atomic_exchange(&g_behind_ms, 0);
    *resyncs = atomic_exchange(&g_resyncs, 0);
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    unsigned r = atomic_load_explicit(&g_ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&g_ring_w, memory_order_acquire);
    static int16_t last[2];
    if (enh_paused()) { memset(stream, 0, (size_t)len); return; }
    int short_frames = 0;
    for (int i = 0; i < frames; i++) {
        if (r != w) {
            last[0] = g_ring[r % AUDIO_RING][0];
            last[1] = g_ring[r % AUDIO_RING][1];
            r++;
        } else short_frames++;
        out[2 * i] = last[0];
        out[2 * i + 1] = last[1];
    }
    /* keep latency bounded: if far behind, skip ahead */
    if (w - r > 44100 / 5) r = w - 44100 / 20;
    atomic_store_explicit(&g_ring_r, r, memory_order_release);
    if (short_frames) atomic_fetch_add_explicit(&g_audio_short, (unsigned)short_frames, memory_order_relaxed);
}

/* ------------------------------------------------------------------ pacing (guest thread) */
static uint64_t g_pace_t0;          /* host ticks at virtual time 0 */
static double g_pace_freq;

/* headless --realtime (a linked node with no window, net.c): the emulated time follows the clock */
int g_realtime;
static void headless_pace(double virt) {
    static double t0 = -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double real = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (t0 < 0 || enh_turbo() || real - t0 - virt > 0.25) t0 = real - virt;   /* start, fast-forward, stalls */
    double ahead = virt - (real - t0);
    if (ahead > 0.0005) {
        struct timespec d = { 0, (long)(ahead * 1e9) };
        nanosleep(&d, NULL);
    }
}

void rt_pace_vblank(void) {
    if (!g_frontend_active) {
        if (g_realtime) headless_pace((double)rt_now() / CPU_HZ);
        return;
    }
    double virt = (double)rt_now() / CPU_HZ;
    if (enh_turbo()) {                  /* enhanced mode boot/apply: as fast as possible */
        g_pace_t0 = SDL_GetPerformanceCounter() - (uint64_t)(virt * g_pace_freq);
        return;
    }
    if (enh_paused() || app_suspended()) {   /* enhanced mode pause, the iOS app in the background: the game waits here */
        while ((enh_paused() || app_suspended()) && g_frontend_active) SDL_Delay(5);
        g_pace_t0 = SDL_GetPerformanceCounter() - (uint64_t)(virt * g_pace_freq);
        return;
    }
    for (;;) {
        double real = (double)(SDL_GetPerformanceCounter() - g_pace_t0) / g_pace_freq;
        double ahead = virt - real;
        if (ahead <= 0.0005) {
            unsigned behind = ahead < 0 ? (unsigned)(-ahead * 1000) : 0;
            if (behind > atomic_load_explicit(&g_behind_ms, memory_order_relaxed)) atomic_store(&g_behind_ms, behind);
            if (ahead < -0.25) {            /* resync after stalls */
                g_pace_t0 = SDL_GetPerformanceCounter() - (uint64_t)(virt * g_pace_freq);
                atomic_fetch_add(&g_resyncs, 1);
            }
            return;
        }
        SDL_Delay(ahead > 0.002 ? (Uint32)((ahead - 0.001) * 1000) : 0);
    }
}

/* ------------------------------------------------------------------ input */
/* each digital control may be held by several keys and buttons: one bit per source, so
 * releasing one of them does not release the others */
enum { SRC_KEY = 1, SRC_KEY2 = 2, SRC_PAD = 4, SRC_TOUCH = 8 };
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
static double g_steer_speed = 4.0;      /* steering travel per second, centre to full lock */
int frontend_stick_response(void) { return (int)lround(g_stick_curve) - 1; }
void frontend_set_stick_response(int response) {
    g_stick_curve = response >= 0 && response <= 2 ? response + 1.0 : 3.0;
}

/* ------------------------------------------------------------------ touch controls (touch.c) */
/* STEERING: the touch stick or tilting the device (the iOS app; elsewhere there is no tilt, and
 * the option is not offered) */
static int g_steer_mode;
int frontend_tilt_available(void) {
#ifdef VIPER_IOS
    return ios_tilt_available();
#else
    return 0;
#endif
}
int frontend_steering_mode(void) { return g_steer_mode; }
void frontend_set_steering_mode(int mode) { g_steer_mode = mode == 1 && frontend_tilt_available(); }

#ifdef VIPER_IOS
static int g_touch;                     /* on */
static TouchOut g_touch_out;            /* this loop's */
#define MAX_UI_QUADS 64
static UiQuad g_ui_quads[MAX_UI_QUADS];
/* the response is gentler than a gamepad stick's for the same STICK RESPONSE: the stick is short
 * under a thumb */
static double touch_curve(void) { return 1.0 + 0.5 * frontend_stick_response(); }

#define TILT_DEADZONE 0.05      /* radians: about 3 degrees */
#define TILT_LOCK 0.50          /* full lock at about 29 degrees */
static int tilt_steer(double *v) {
    double a;
    if (g_steer_mode != 1 || !ios_tilt(&a)) return 0;
    double m = (fabs(a) - TILT_DEADZONE) / (TILT_LOCK - TILT_DEADZONE);
    m = m < 0 ? 0 : m > 1 ? 1 : m;
    *v = copysign(pow(m, touch_curve()), a);
    return 1;
}
#endif


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

static int g_rumble_testing;
static atomic_uchar g_motor_output;
static atomic_uint g_motor_writes, g_motor_energized_writes;
static Uint32 g_rumble_trace_started, g_rumble_trace_tick;
static int g_rumble_trace;
static ControllerRumble g_rumble = { .id = -1 };
static double g_rumble_gain = 0.5;

int frontend_rumble_multiplier(void) { return (int)lround(g_rumble_gain * 200); }
void frontend_set_rumble_multiplier(int percent) {
    g_rumble_gain = SDL_clamp(percent, 0, 400) / 200.0;
}
static Uint32 g_rumble_test_started;

static void test_rumble(void) {
    if (!g_pad) { rt_log("rumble test: no controller connected\n"); return; }
    if (g_rumble_testing) return; /* let the current pulse finish */
    g_rumble_test_started = SDL_GetTicks();
    if (controller_rumble_pulse(&g_rumble, g_pad, g_rumble_gain, g_rumble_test_started) < 0) {
        rt_log("rumble test failed: %s\n", SDL_GetError());
        return;
    }
    g_rumble_testing = 1;
    g_rumble_trace = 1;
    g_rumble_trace_started = g_rumble_test_started;
    g_rumble_trace_tick = g_rumble_test_started - 1000;
    rt_log("rumble test: single 1000 ms pulse at %.1fx strength (output=%u)\n",
           frontend_rumble_multiplier() / 100.0, g_rumble.strength);
}

/* Guest publishes motor commands; SDL calls stay on the host main thread. */
void frontend_set_motor(uint8_t command) {
    atomic_store_explicit(&g_motor_output, command, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_motor_writes, 1, memory_order_relaxed);
    if ((command & 0x80) && (command & 15)) atomic_fetch_add_explicit(&g_motor_energized_writes, 1, memory_order_relaxed);
}

static void update_rumble(int active) {
    Uint32 now = SDL_GetTicks();
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (g_rumble_testing && (!window || (Uint32)(now - g_rumble_test_started) >= 1000))
        g_rumble_testing = 0;
    uint8_t motor = atomic_load_explicit(&g_motor_output, memory_order_relaxed);
    if (!g_rumble_testing)
        controller_rumble_update(&g_rumble, g_pad, motor, g_rumble_gain, active, now);
    if (g_rumble_trace && (Uint32)(now - g_rumble_trace_started) >= 30000) g_rumble_trace = 0;
    if (g_rumble_trace && (Uint32)(now - g_rumble_trace_tick) >= 1000) {
        g_rumble_trace_tick = now;
        rt_log("rumble: motor=%02x writes=%u energized=%u active=%d test=%d gain=%.1fx output=%u retry=%d backend=%s paused=%d menu=%d turbo=%d owned=%d focus=%d\n",
            motor, atomic_load_explicit(&g_motor_writes, memory_order_relaxed),
            atomic_load_explicit(&g_motor_energized_writes, memory_order_relaxed),
            active, g_rumble_testing, frontend_rumble_multiplier() / 100.0,
            g_rumble.strength, g_rumble.failed, g_rumble_backend,
            enh_paused(), enh_menu_active(), enh_turbo(), enh_inputs_owned(), window != NULL);
    }
}

static void apply_inputs(double dt) {
    /* Menu confirmation can consume a pedal button's down event. These controls
     * represent held state, so reconcile them with the device every frame. */
    int pad_active = g_pad && g_input_focus;
    hold(&ctl.gas, SRC_PAD, pad_active && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A));
    hold(&ctl.brake, SRC_PAD, pad_active && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_X));
    hold(&ctl.handbrake, SRC_PAD, pad_active && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B));
    hold(&ctl.steer_left, SRC_PAD, pad_active && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    hold(&ctl.steer_right, SRC_PAD, pad_active && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
#ifdef VIPER_IOS
    hold(&ctl.gas, SRC_TOUCH, g_touch_out.gas);
    hold(&ctl.brake, SRC_TOUCH, g_touch_out.brake);
    hold(&ctl.shift_up, SRC_TOUCH, g_touch_out.shift_up);
    hold(&ctl.shift_down, SRC_TOUCH, g_touch_out.shift_down);
    hold(&ctl.start, SRC_TOUCH, g_touch_out.start);
    hold(&ctl.coin, SRC_TOUCH, g_touch_out.coin);
#endif

    /* every steering source ramps towards its target, as a wheel turns: a stick flicked to full
     * lock in one frame flips GTI Club 2's car over */
    int wsel = enh_wheel_select_active();
    double target = wsel ? enh_wheel_select_pos() : !!ctl.steer_right - !!ctl.steer_left;
    if (g_pad && g_input_focus && !wsel) {
        double stick = controller_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX),
                                       g_stick_deadzone, g_stick_curve);
        if (stick != 0) target = stick;
    }
#ifdef VIPER_IOS
    if (!wsel) {                        /* the touch stick, else tilt when nothing else steers */
        double v;
        if (g_touch_out.steer_on) target = controller_axis((int)lround(g_touch_out.steer * 32767), 0.04, touch_curve());
        else if (target == 0 && tilt_steer(&v)) target = v;
    }
#endif
    double speed = g_steer_speed * SDL_min(dt, 0.05);
    if (ctl.steer < target) ctl.steer = SDL_min(target, ctl.steer + speed);
    else if (ctl.steer > target) ctl.steer = SDL_max(target, ctl.steer - speed);
    double steer = ctl.steer;
    /* Take the strongest pedal source: a slightly pressed trigger must not reduce a
     * fully held key/button. Right-stick Y supplies proportional pedals on Switch Pro. */
    double gas = ctl.gas ? 1.0 : 0.0, brake = ctl.brake ? 1.0 : 0.0;
    if (g_pad && g_input_focus) {
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
    /* the enhanced mode declares a cabinet with a handbrake in every version (hw.c) */
    if (GAME_HAS_HANDBRAKE || (g_enhanced && GAME_ENH_CAB_HANDBRAKE))
        g_analog[3] = (int16_t)(ctl.handbrake ? ANALOG_RANGE : -ANALOG_RANGE);
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
    case SDL_CONTROLLER_BUTTON_X: hold(&ctl.brake, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_B: hold(&ctl.handbrake, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: hold(&ctl.steer_left, SRC_PAD, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: hold(&ctl.steer_right, SRC_PAD, down); break;
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

/* ------------------------------------------------------------------ vsync (frames in between) */
/* The display's refresh as the GPU path meets it: the time each buffer swap returns (a vsync) and
 * the median of the last intervals, in emulated time (the pacing's clock), for interp.c to make
 * the frames in between for the vsyncs to come. Measured, not taken from the display mode: it
 * follows the screen the system syncs the window to (the one holding most of it), VRR and caps. */
#define VSYNC_N 31
static double g_vsync_iv[VSYNC_N];
static int g_vsync_n, g_vsync_pos;
static uint64_t g_vsync_last;
static double g_vsync_period;              /* seconds, 0 = not known yet */
static uint64_t g_show_at;                  /* the frames in between due at the next vsync: up to this */

/* the highest refresh of the screens connected, for the FRAME RATE values offered */
static void report_refresh(void) {
    int best = 0, n = SDL_GetNumVideoDisplays();
    for (int i = 0; i < n; i++) {
        SDL_DisplayMode m;
        if (SDL_GetCurrentDisplayMode(i, &m) == 0 && m.refresh_rate > best) best = m.refresh_rate;
    }
    enh_set_refresh(best);
}

static void vsync_reset(void) { g_vsync_n = g_vsync_pos = 0; g_vsync_last = 0; }

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* after a swap returned: returns the emulated time up to which a frame in between is shown at
 * the next vsync, halfway to the one after (0: not known). The frames' times fall on the vsyncs
 * measured, so the line between two is the middle: a swap returning a little late or early
 * does not move a frame to the next vsync (one shown twice, the next one skipped) */
static uint64_t vsync_tick(void) {
    uint64_t now = SDL_GetPerformanceCounter();
    if (g_vsync_last) {
        double iv = (double)(now - g_vsync_last) / g_pace_freq;
        if (iv > 1.0 / 500 && iv < 1.0 / 20) {
            g_vsync_iv[g_vsync_pos] = iv;
            g_vsync_pos = (g_vsync_pos + 1) % VSYNC_N;
            if (g_vsync_n < VSYNC_N) g_vsync_n++;
        }
    }
    g_vsync_last = now;
    if (g_vsync_n < 8) return 0;
    double s[VSYNC_N];
    memcpy(s, g_vsync_iv, sizeof s);
    qsort(s, (size_t)g_vsync_n, sizeof *s, cmp_double);
    g_vsync_period = s[g_vsync_n / 2];
    double e = (double)(int64_t)(now - g_pace_t0) / g_pace_freq;       /* emulated seconds */
    if (e < 0) return 0;
    interp_set_display((uint64_t)(e * CPU_HZ), (uint64_t)(g_vsync_period * CPU_HZ));
    return (uint64_t)((e + 1.5 * g_vsync_period) * CPU_HZ);
}

/* the picture's height on screen, in pixels (letterboxed in the window), for the AUTO resolution;
 * the largest scale: software 2X, GPU 6X if its textures take the widest target (21:9) */
static void report_display(SDL_Window *win, int gpu, int max_tex, int tw, int th) {
    int dw = 0, dh = 0;
    if (gpu) SDL_GL_GetDrawableSize(win, &dw, &dh);   /* software: AUTO stays 1X (2X doubles the CPU time) */
    long ph = (long)dw * th / tw;
    if (ph > dh) ph = dh;
    int max_scale = 2;
    if (gpu) for (max_scale = 6; max_scale > 2 && (512 + 2 * 199) * max_scale > max_tex; max_scale--) {}
    enh_set_display((int)ph, max_scale);
}

#ifdef VIPER_IOS
/* the app's life, told synchronously on the main thread: before the background it must stop
 * drawing (OpenGL there ends the app), and the NVRAM is saved as the system may end it there */
static int SDLCALL app_events(void *ud, SDL_Event *ev) {
    (void)ud;
    switch (ev->type) {
    case SDL_APP_WILLENTERBACKGROUND:
        gpu_guard_off();                /* leaving it is not a failure to start */
        if (!atomic_exchange(&g_suspended, 1)) {
            if (g_audio) SDL_PauseAudioDevice(g_audio, 1);
            nvram_save();
        }
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        if (atomic_exchange(&g_suspended, 0)) {
            if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
            atomic_store(&g_resumed, 1);
        }
        break;
    case SDL_APP_TERMINATING:
        gpu_guard_off();
        nvram_save();
        break;
    default:
        break;
    }
    return 1;
}

/* the touch controls with the SDL renderer: the atlas as a texture, premultiplied */
static void draw_touch_sdl(SDL_Renderer *ren, SDL_Texture **tex, unsigned *version, int n) {
    int aw, ah;
    unsigned v;
    const uint32_t *atlas = touch_atlas(&aw, &ah, &v);
    if (!atlas || n <= 0) return;
    if (!*tex || v != *version) {
        if (*tex) SDL_DestroyTexture(*tex);
        *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, aw, ah);
        if (!*tex) return;
        SDL_UpdateTexture(*tex, NULL, atlas, aw * 4);
        SDL_SetTextureBlendMode(*tex, SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD));
        SDL_SetTextureScaleMode(*tex, SDL_ScaleModeLinear);
        *version = v;
    }
    SDL_RenderSetLogicalSize(ren, 0, 0);       /* the quads are in output pixels */
    for (int i = 0; i < n; i++) {
        const UiQuad *q = &g_ui_quads[i];
        Uint8 a = (Uint8)(q->a * 255 + 0.5f);
        SDL_SetTextureColorMod(*tex, a, a, a);
        SDL_SetTextureAlphaMod(*tex, a);
        SDL_Rect src = { (int)(q->u0 * aw + 0.5f), (int)(q->v0 * ah + 0.5f), (int)((q->u1 - q->u0) * aw + 0.5f), (int)((q->v1 - q->v0) * ah + 0.5f) };
        SDL_FRect dst = { q->x, q->y, q->w, q->h };
        SDL_RenderCopyF(ren, *tex, &src, &dst);
    }
}
#endif

int frontend_run(int scale, int scale_explicit) {
#ifdef VIPER_IOS
    /* landscape only; the home indicator and the edges' gestures need a second swipe */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    g_touch = touch_init();
#endif
#ifdef VIPER_NATIVE_HAPTICS
    const char *rumble_backend = getenv("RT_RUMBLE_BACKEND");
    if (rumble_backend && !strcmp(rumble_backend, "apple")) controller_haptics_init();
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        rt_log("SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
#ifdef VIPER_IOS
    SDL_AddEventWatch(app_events, NULL);
#endif
    const char *rumble = getenv("RT_RUMBLE");
    if (rumble) {
        char *end;
        double gain = strtod(rumble, &end);
        if (end != rumble && !*end && isfinite(gain) && gain >= 0 && gain <= 2) g_rumble_gain = gain;
        else rt_log("RT_RUMBLE: expected 0..2; keeping current setting\n");
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
    /* GPU renderer (enhanced mode): an OpenGL 3.3 core window; the device already records for it */
    int gpu = voodoo_gpu_active();
    if (gpu) {
#ifdef VIPER_GLES                       /* OpenGL ES 3.0 (iOS) */
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_RETAINED_BACKING, 0);
#else
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    }
#ifdef VIPER_IOS
    Uint32 window_flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    SDL_Window *win = SDL_CreateWindow(g_enhanced ? GAME_TITLE " - enhanced" : GAME_TITLE,
        window_rect.x, window_rect.y, window_rect.w, window_rect.h,
        window_flags | (gpu ? SDL_WINDOW_OPENGL : 0));
    if (!win) { rt_log("SDL_CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return -1; }
    SDL_SetWindowMinimumSize(win, 320, 240);
    int window_dirty = 0;               /* moved or resized: saved 0.5 s after the last change */
    Uint32 window_changed = 0;
    if (getenv("RT_RESTARTED")) {       /* enhanced mode, after a restart: macOS does not reactivate */
        unsetenv("RT_RESTARTED");       /* the re-executed program, so take the focus back */
        SDL_SetHint("SDL_FORCE_RAISEWINDOW", "1");
        SDL_RaiseWindow(win);
    }
    int gpu_failed = 0;
#ifdef VIPER_IOS
    /* a guard around the GPU renderer's start: a file says it is starting, removed once it has
     * shown a few seconds of the game. Found at the next launch, the app died in between (a driver
     * that does not take the pixel pipeline): the software renderer from then on (DISPLAY,
     * RENDERER brings it back) */
    snprintf(g_gpu_guard, sizeof g_gpu_guard, "%s/gpu_starting", ios_documents_dir());
    if (gpu) {
        FILE *g = fopen(g_gpu_guard, "rb");
        if (g) {
            fclose(g);
            remove(g_gpu_guard);
            rt_log("renderer: the hardware renderer did not start last time, software from now on\n");
            enh_renderer_software();
            voodoo_set_gpu(0);
            gpu = 0;
            gpu_failed = 1;
        } else if ((g = fopen(g_gpu_guard, "wb"))) { fclose(g); g_gpu_guard_frames = 0; }
    }
#endif
    if (gpu) {
        uint32_t vram_size = 0;
        if (!voodoo_vram(&vram_size) || !gpu_gl_init(win, vram_size)) {
            voodoo_set_gpu(0);
            gpu = 0;
            gpu_failed = 1;
        }
    }
    /* which chip draws the 3D graphics (also after a RENDERER change, which restarts the game) */
#ifdef VIPER_IOS
    if (!gpu) gpu_guard_off();          /* no GPU renderer to guard (it fell back) */
#endif
    if (gpu) rt_log("renderer: hardware, %s\n", gpu_gl_name());
    else rt_log("renderer: software (CPU)%s\n", gpu_failed ? ", the GPU is not available" : "");
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    if (!gpu) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(ren, 512, 384);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 512, 384);
    }
    int tw = 512, th = 384, overlay_drawn = 0;
    int max_tex = gpu ? gpu_gl_max_texture() : 0;
#ifdef VIPER_IOS
    SDL_Texture *ui_tex = NULL;         /* the touch controls with the SDL renderer */
    unsigned ui_version = 0;
    /* the frame loop draws once per refresh, waiting for it in UIKit's run loop (platform_ios.m) */
    ios_vsync_start(enh_frame_rate() > 60 ? 120 : 60);
    int tilt_on = -1;
#endif
    report_refresh();
    report_display(win, gpu, max_tex, tw, th);

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
    g_steer_speed = controller_option("RT_STEER_SPEED", 4.0, 1.0, 100.0);
    open_pad();

    g_pace_freq = (double)SDL_GetPerformanceFrequency();
    g_pace_t0 = SDL_GetPerformanceCounter() - (uint64_t)((double)rt_now() / CPU_HZ * g_pace_freq);
    g_frontend_active = 1;

    static uint32_t frame[2048 * 2048], raw[2048 * 2048];
    uint64_t last_frame = 0, last_tick = SDL_GetPerformanceCounter(), last_save = last_tick;
    int running = 1, fs_applied = 0, restart = 0, text_on = 1;
    while (running) {
#ifdef VIPER_IOS
        if (!atomic_load(&g_suspended)) {
            ios_vsync_set_rate(enh_frame_rate() > 60 ? 120 : 60);
            ios_vsync_wait();
        }
        if (tilt_on != (g_steer_mode == 1)) { tilt_on = g_steer_mode == 1; ios_tilt_enable(tilt_on); }
#endif
        /* text input only for the name entry: SDL starts it with the video, and while it is on
         * macOS opens its accent picker on a held letter key (W, A, S, D while driving). In the iOS
         * app the on-screen keyboard comes up only from the touch controls' KEYS. */
        if (enh_text_input_active() != text_on) {
            text_on = enh_text_input_active();
#ifdef VIPER_IOS
            if (!text_on) SDL_StopTextInput();
#else
            if (text_on) SDL_StartTextInput(); else SDL_StopTextInput();
#endif
        }
        if (g_enhanced && enh_want_fullscreen() != fs_applied) {      /* enhanced mode: DISPLAY option */
            fs_applied = enh_want_fullscreen();
            SDL_SetWindowFullscreen(win, fs_applied ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
        SDL_Event ev;
#ifdef VIPER_IOS
        static unsigned diag_frames, diag_touches;
        static Uint32 diag_at;
        diag_frames++;
        if (!diag_at) diag_at = SDL_GetTicks();
        if (SDL_GetTicks() - diag_at >= 5000 && diag_at < 40000) {     /* the first half minute */
            rt_log("frontend: %u loops/s, UIKit state %d, %u touches\n", diag_frames / 5, ios_app_state(), diag_touches);
            diag_frames = 0;
            diag_at = SDL_GetTicks();
        }
#endif
        while (SDL_PollEvent(&ev)) {
#ifdef VIPER_IOS
            if (ev.type == SDL_FINGERDOWN) diag_touches++;
#endif
#ifdef VIPER_IOS
            if (g_touch && touch_event(win, &ev)) continue;
#endif
            switch (ev.type) {
            case SDL_QUIT: running = 0; rt_log("frontend: the window was closed\n"); break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    g_input_focus = 0;
                    g_menu_repeat_button = -1;
                    memset(&ctl, 0, sizeof ctl);
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_input_focus = 1;
#if SDL_VERSION_ATLEAST(2, 0, 18)
                if (ev.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) { vsync_reset(); report_refresh(); }
#endif
#ifndef VIPER_IOS                      /* the window is the screen */
                if (ev.window.event == SDL_WINDOWEVENT_MOVED || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    window_dirty = 1;
                    window_changed = SDL_GetTicks();
                }
#endif
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_F8) { if (!ev.key.repeat) test_rumble(); break; }
                if (ev.key.keysym.sym == SDLK_F9) {
                    if (!ev.key.repeat) {
                        g_controller_log = !g_controller_log;
                        rt_log("controller input logging: %s\n", g_controller_log ? "on" : "off");
                    }
                    break;
                }
                if (enh_code_entry_active()) {           /* MULTIPLAYER code: the letters type */
                    SDL_Keycode k = ev.key.keysym.sym;
                    if (k == SDLK_ESCAPE) enh_menu_action(ENH_BACK);
                    else if (k == SDLK_BACKSPACE) enh_code_key('\b');
                    else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && !ev.key.repeat) enh_code_key('\r');
                    else if (k == SDLK_v && (ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI))) {
                        char *clip = SDL_GetClipboardText();
                        enh_code_paste(clip);
                        SDL_free(clip);
                    }
                } else if (enh_link_menu_active() && (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER)) {
                    if (!ev.key.repeat) enh_link_answer(1);    /* leave the session (Esc: stay) */
                } else if ((enh_menu_active() || enh_paused()) && menu_key(ev.key.keysym.sym) >= 0) {
                    int action = menu_key(ev.key.keysym.sym);
                    g_menu_repeat_button = -1;
                    if (!ev.key.repeat || action == ENH_LEFT || action == ENH_RIGHT) enh_menu_action(action);
                } else if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (!ev.key.repeat && !enh_escape()) { running = 0; rt_log("frontend: Esc, quit\n"); }   /* enhanced: pause / back */
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
                if (enh_code_entry_active() && !(SDL_GetModState() & (KMOD_CTRL | KMOD_GUI)))
                    for (const char *p = ev.text.text; *p; p++) enh_code_key((unsigned char)*p);
                else if (enh_name_entry_active() && !enh_paused())
                    for (const char *p = ev.text.text; *p; p++) enh_name_type((unsigned char)*p);
                break;
            case SDL_CONTROLLERAXISMOTION:
#ifdef VIPER_IOS
                if (pad_matches(ev.caxis.which) && (ev.caxis.value < -16000 || ev.caxis.value > 16000)) touch_hide();
#endif
                if (pad_matches(ev.caxis.which) && g_input_focus && ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    static int zone;
                    int z=ev.caxis.value < -16000 ? -1 : ev.caxis.value > 16000 ? 1 : 0;
                    if (z && z != zone && !enh_paused()) enh_wheel_select_step(z);
                    zone=z;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: open_pad(); break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (pad_matches(ev.cdevice.which)) { g_rumble_testing = 0; update_rumble(0); close_pad(); open_pad(); }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (pad_matches(ev.cbutton.which)) g_menu_repeat_button = -1;
#ifdef VIPER_IOS
                if (pad_matches(ev.cbutton.which)) touch_hide();
#endif
                if (!pad_matches(ev.cbutton.which) || !g_input_focus) break;
#if SDL_VERSION_ATLEAST(2, 0, 14)
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_MISC1 && SDL_GetKeyboardFocus()) {
                    test_rumble(); break;  /* Switch Capture: test the transport independently of game FFB. */
                }
#endif
                if (enh_link_menu_active() && ev.cbutton.button == SDL_CONTROLLER_BUTTON_Y)
                    enh_link_answer(1);         /* leave the session; Back again: stay (the others drive) */
                else if ((enh_menu_active() || enh_paused()) && menu_button(ev.cbutton.button) >= 0)
                    menu_pad_press(ev.cbutton.button, SDL_GetTicks());
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                    if (!enh_escape()) { running = 0; rt_log("frontend: Guide button, quit\n"); }  /* Home: pause/back in play, quit from main menu. */
                }
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && g_enhanced)
                    enh_escape();       /* pause / back, as Guide, without quitting: no coins in the enhanced mode */
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
#ifdef VIPER_IOS
        if (atomic_load(&g_suspended)) { SDL_Delay(16); continue; }     /* in the background: no drawing */
        /* SDL's launch screen must not stay over the game, taking its touches: SDL fades it out
         * at once; still up half a second later, it is hidden here */
        static int launch_checks;
        if (launch_checks < 300 && ++launch_checks >= 30 && ios_hide_launch_screen())
            rt_log("frontend: SDL's launch screen was still up after %d frames, hidden\n", launch_checks);
        if (atomic_exchange(&g_resumed, 0)) {
            vsync_reset();
            /* back in the foreground in a race: paused, as the player left it */
            if (g_enhanced && !enh_paused() && !enh_menu_active() && !enh_turbo() && !enh_name_entry_active() &&
                !(net_active() && !net_session_over())) enh_escape();       /* (a linked race goes on) */
        }
        if (g_touch) {
            touch_set_picture(tw, th);
            touch_frame(win);
            touch_output(&g_touch_out);
            static int zone;             /* a wheel select (course): the stick steps through it */
            int z = !g_touch_out.steer_on ? 0 : g_touch_out.steer < -0.5 ? -1 : g_touch_out.steer > 0.5 ? 1 : 0;
            if (z && z != zone && !enh_paused()) enh_wheel_select_step(z);
            zone = z;
            if (g_touch_out.keyboard) {
                if (SDL_IsTextInputActive()) SDL_StopTextInput();
                else SDL_StartTextInput();
            }
        }
#endif
        menu_pad_repeat(SDL_GetTicks());
        update_rumble((SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) &&
                      !enh_paused() && !enh_menu_active() && !enh_turbo() && !enh_inputs_owned());
        uint64_t now = SDL_GetPerformanceCounter();
        apply_inputs((double)(now - last_tick) / g_pace_freq);
        last_tick = now;
        if ((double)(now - last_save) / g_pace_freq > 60.0) { nvram_save(); last_save = now; }
        if (enh_quit_requested()) { running = 0; rt_log("frontend: QUIT from the menu\n"); }
        if (g_enhanced) {
            const char *url = enh_take_url();
#if SDL_VERSION_ATLEAST(2, 0, 14)
            if (url && SDL_OpenURL(url) != 0) rt_log("frontend: cannot open %s: %s\n", url, SDL_GetError());
#else
            if (url) rt_log("frontend: cannot open %s (SDL older than 2.0.14)\n", url);
#endif
        }
        if (enh_restart_requested()) {
#ifdef VIPER_IOS                        /* an app cannot start itself again: the player does */
            nvram_save();
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, GAME_TITLE,
                "The new settings are saved. The game closes now: open it again to play with them.", win);
#endif
            running = 0; restart = 1; rt_log("frontend: restart\n");
        }

        int w, h;
        uint64_t cnt = voodoo_get_frame(NULL, 0, &w, &h);
        int fresh = cnt != last_frame && w > 0 && h > 0;
        if (fresh) {
            last_frame = cnt;
            if (!gpu) voodoo_get_frame(raw, 2048 * 2048, &w, &h);
            if (w != tw || h != th) {
                /* a new aspect ratio (enhanced mode, widescreen): the window keeps its height,
                 * unless it already has that ratio (a window restored from the settings) */
                if ((long)w * th != (long)h * tw && !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
                    int ww, wh;
                    SDL_GetWindowSize(win, &ww, &wh);
                    if ((long)w * wh != (long)h * ww) SDL_SetWindowSize(win, (int)((long)wh * w / h), wh);
                }
                if (!gpu) {
                    SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                    SDL_RenderSetLogicalSize(ren, w, h);
                }
                int new_ratio = (long)w * th != (long)h * tw;
                tw = w; th = h;
                if (new_ratio) report_display(win, gpu, max_tex, tw, th);
                overlay_drawn = 1;          /* the layer below is cleared at the new size */
            }
        }
        if (gpu) {
            /* the menus on a layer of their own, cleared only after something was drawn on it */
            /* the layer is at most 768 lines high (as at 2X): the game's fonts are low-resolution
             * anyway, and a larger one would cost a big upload each frame with the fps counter on */
            int oh = th > 768 ? 768 : th, ow = (int)((long)tw * oh / th);
            if (overlay_drawn) memset(frame, 0, (size_t)ow * oh * 4);
            overlay_drawn = g_enhanced && last_frame && enh_draw_overlay_layer(frame, ow, oh);
#ifdef VIPER_IOS
            if (g_touch) {
                int nq = touch_quads(win, g_ui_quads, MAX_UI_QUADS), aw, ah;
                unsigned version;
                const uint32_t *atlas = touch_atlas(&aw, &ah, &version);
                gpu_gl_ui(atlas, aw, ah, version, g_ui_quads, nq);
            }
#endif
            gpu_gl_frame(win, overlay_drawn ? frame : NULL, ow, oh, g_show_at);   /* vsync paces */
#ifdef VIPER_IOS
            if (g_gpu_guard_frames >= 0 && last_frame && ++g_gpu_guard_frames > 300) gpu_guard_off();
#endif
            g_show_at = vsync_tick();
            if (window_dirty && (Uint32)(SDL_GetTicks() - window_changed) >= 500) {
                save_window(win);
                report_display(win, gpu, max_tex, tw, th);
                window_dirty = 0;
            }
            continue;
        }
        /* the overlay is redrawn over the last game frame every loop, so the enhanced menus
         * respond while the game is paused */
        if (fresh || (g_enhanced && last_frame)) {
            memcpy(frame, raw, (size_t)tw * th * 4);
            enh_draw_overlay(frame, tw, th);
            SDL_UpdateTexture(tex, NULL, frame, tw * 4);
        }
#ifdef VIPER_IOS
        if (g_touch) SDL_RenderSetLogicalSize(ren, tw, th);     /* the touch controls drew in pixels */
#endif
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
#ifdef VIPER_IOS
        if (g_touch) draw_touch_sdl(ren, &ui_tex, &ui_version, touch_quads(win, g_ui_quads, MAX_UI_QUADS));
#endif
        SDL_RenderPresent(ren);       /* vsync paces this loop */
        if (window_dirty && (Uint32)(SDL_GetTicks() - window_changed) >= 500) {
            save_window(win);
            report_display(win, gpu, max_tex, tw, th);
            window_dirty = 0;
        }
    }
    g_rumble_testing = 0;
    update_rumble(0);
    if (window_dirty) save_window(win);
    nvram_save();
#ifdef VIPER_IOS
    if (ui_tex) SDL_DestroyTexture(ui_tex);
#endif
    if (gpu) gpu_gl_shutdown();
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    close_pad();
#ifdef VIPER_NATIVE_HAPTICS
    controller_haptics_stop();
#endif
    SDL_Quit();
    return restart ? 2 : 0;
}
