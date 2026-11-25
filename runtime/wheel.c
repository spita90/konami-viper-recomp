/*
 * Steering wheels and every other USB game device: bindings and force feedback.
 *
 * Devices. On Windows every DirectInput game device that is attached is opened, whatever it is: a
 * wheel base, pedals, a shifter, a handbrake, a button box, an arcade encoder (Xbox pads are left
 * to the gamepad code of frontend_sdl.c). Nothing is chosen by a list of known models. A device is
 * remembered by its USB ids and its product name ("0EB7:0006 FANATEC Wheel"), not by the order
 * Windows lists it in.
 *
 * One piece of hardware can be several devices with the same product name: a Fanatec base is two
 * "FANATEC Wheel"s, one with the steering axis and one for what is plugged into the wheel, and the
 * motor is on one of them, not necessarily the one that steers. All of them are kept and told apart
 * by their shape ("FANATEC Wheel [A8 B108]"), so that a binding means one of them; the force
 * feedback then looks for the motor among the devices of the same name, and the one on which the
 * effect really starts is used (the capability flag alone is not trusted).
 *
 * Bindings. Each action takes up to WHEEL_MAX_BINDS of them, of any kind and from any device: a
 * key, a button, a hat direction or an axis (learned when it is bound: which way it moves, and
 * whether it rests at one end, as a pedal, or in the middle). Kept in <binary>_controls.ini, shared
 * by the classic and the enhanced mode.
 *
 * Force feedback. The games drive the cabinet's K-type steering motor themselves (hw.c): on/off, a
 * direction and a torque of 0-15. That command is played as it is, as a constant force on the
 * wheel's motor, so the centring, the kerbs and the crashes are the game's own.
 *
 * Elsewhere than Windows there is no device backend yet: the keyboard bindings work, no wheel.
 */
#include "runtime.h"
#include "wheel.h"
#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAX 10
#define NBTN 128
#define NPOV 4
#define MAX_DEV 16
static const char *const k_axis_name[NAX] = { "x", "y", "z", "rx", "ry", "rz", "slider0", "slider1", "extra0", "extra1" };
static const char *const k_hat_name[4] = { "up", "right", "down", "left" };

typedef struct {
    char name[112];                 /* unique: the product name, plus "[A<axes> B<buttons>]" when several share it */
    char base[80];                  /* the product name alone: devices with the same base belong together */
    char id[128];                   /* "VVVV:PPPP name", what a binding remembers */
    unsigned vid, pid;
    int present[NAX], naxes, nbuttons, npovs;
    int ffb;                        /* reports a motor */
    int ff_slot;                    /* the axis the motor acts on, -1 = not said */
    int siblings;
    double axis[NAX];               /* -1..1 */
    uint8_t btn[NBTN];
    int pov[NPOV];                  /* hundredths of a degree, -1 = centred */
    void *h;                        /* the backend's device */
} Dev;
static Dev g_dev[MAX_DEV];
static int g_ndev;

enum { B_KEY = 1, B_BUTTON, B_HAT, B_AXIS };
typedef struct {
    int type;
    int code;                       /* scancode, button, hat * 4 + direction, axis slot */
    int inverted, half;             /* an axis: pressed towards its minimum; rests in the middle */
    char dev[128];                  /* the device's id */
    int di;                         /* the device now, -1 = not connected */
} Bind;
static Bind g_bind[ACT_COUNT][WHEEL_MAX_BINDS];
static int g_nbind[ACT_COUNT];

static const char *const k_action_key[ACT_COUNT] = {
    "steer", "steer_left", "steer_right", "gas", "brake", "handbrake", "shift_up", "shift_down", "start", "coin",
    "test", "service", "pause", "menu_up", "menu_down", "menu_left", "menu_right", "menu_ok", "menu_back" };
static const char *const k_action_name[ACT_COUNT][2] = {
    { "STEERING AXIS", "ASSE STERZO" }, { "STEER LEFT", "STERZA A SINISTRA" }, { "STEER RIGHT", "STERZA A DESTRA" },
    { "ACCELERATOR", "ACCELERATORE" }, { "BRAKE", "FRENO" }, { "HANDBRAKE", "FRENO A MANO" },
    { "SHIFT UP", "MARCIA SU" }, { "SHIFT DOWN", "MARCIA GIU" }, { "START - VIEW", "START - VISUALE" },
    { "COIN", "GETTONE" }, { "TEST", "TEST" }, { "SERVICE", "SERVICE" }, { "PAUSE", "PAUSA" },
    { "MENU UP", "MENU SU" }, { "MENU DOWN", "MENU GIU" }, { "MENU LEFT", "MENU SINISTRA" },
    { "MENU RIGHT", "MENU DESTRA" }, { "MENU OK", "MENU OK" }, { "MENU BACK", "MENU INDIETRO" } };

static char g_path[1040];
static int g_ffb_strength = 100, g_ffb_invert, g_range = 100, g_deadzone = 0;
static char g_ffb_device[128];      /* the id chosen, "" = AUTO */
static unsigned g_generation;
static void *g_window;
static Uint32 g_rescan_at;
static int g_rescan;
static double g_value[ACT_COUNT];
static uint8_t g_cur[ACT_COUNT], g_prev[ACT_COUNT], g_edge[ACT_COUNT];
static uint8_t g_key_block[SDL_NUM_SCANCODES];
static int g_steer_on;
static double g_steer;

/* capture */
static int g_cap = -1;
static Uint32 g_cap_until;
static double g_cap_axis[MAX_DEV][NAX];
static uint8_t g_cap_btn[MAX_DEV][NBTN];
static uint8_t g_cap_hat[MAX_DEV][NPOV];
#define CAPTURE_MS 10000
#define CAPTURE_AXIS_MOVE 0.45

static char g_ffb_status[200] = "NO WHEEL";
/* RT_WHEEL_LOG: what the devices cost the frontend's loop, per second (ms) */
static double g_t_poll_sum, g_t_poll_max, g_t_ffb_sum, g_t_ffb_max;
static double ms_since(Uint64 t0) { return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
static void ffb_end(void);

/* ------------------------------------------------------------------ small helpers */
static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static void copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src); }

/* a text in the characters the games' fonts have: capitals, digits and a little punctuation */
static void menu_text(char *s) {
    for (; *s; s++) {
        int c = toupper((unsigned char)*s);
        *s = (char)((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(" -,'[].!", c) ? c : ' ');
    }
}

/* the product name inside an id or a unique name: without "VVVV:PPPP ", " [A8 B108]" and " #2" */
static void base_of(const char *id, char *out, size_t n) {
    unsigned v, p;
    int skip = 0;
    if (sscanf(id, "%4x:%4x %n", &v, &p, &skip) < 2) skip = 0;
    copy(out, n, id + skip);
    char *tag = strstr(out, " [A");
    if (tag) *tag = 0;
    else if ((tag = strrchr(out, '#')) && tag > out && tag[-1] == ' ') tag[-1] = 0;
}

static int pov_dir(int pov, int dir) {
    if (pov < 0) return 0;
    switch (dir) {
    case 0: return pov >= 31500 || pov <= 4500;
    case 1: return pov >= 4500 && pov <= 13500;
    case 2: return pov >= 13500 && pov <= 22500;
    default: return pov >= 22500 && pov <= 31500;
    }
}

/* ------------------------------------------------------------------ the device a binding means */
static int dev_can(const Dev *d, const Bind *b) {
    if (b->type == B_BUTTON) return b->code < d->nbuttons;
    if (b->type == B_HAT) return b->code / 4 < d->npovs;
    return b->type == B_AXIS && b->code < NAX && d->present[b->code];
}

/* the same id; else the same name on another port or after a firmware update (other ids); else a
 * device of the same product that has what is bound (a base that is two devices, or whose shape
 * changed with the rim plugged into it) */
static int resolve(const Bind *b) {
    char base[80], other[80];
    for (int i = 0; i < g_ndev; i++)
        if (ieq(g_dev[i].id, b->dev)) return i;
    const char *name = strchr(b->dev, ' ');
    if (name)
        for (int i = 0; i < g_ndev; i++)
            if (ieq(g_dev[i].name, name + 1) && dev_can(&g_dev[i], b)) return i;
    base_of(b->dev, base, sizeof base);
    for (int i = 0; i < g_ndev; i++) {
        base_of(g_dev[i].id, other, sizeof other);
        if (ieq(base, other) && dev_can(&g_dev[i], b)) return i;
    }
    return -1;
}

static void resolve_all(void) {
    for (int a = 0; a < ACT_COUNT; a++)
        for (int i = 0; i < g_nbind[a]; i++)
            g_bind[a][i].di = g_bind[a][i].type == B_KEY ? -1 : resolve(&g_bind[a][i]);
    g_generation++;
}

/* several devices with one product name: each gets its shape, and a number if that is the same too */
static void name_devices(void) {
    for (int i = 0; i < g_ndev; i++) {
        g_dev[i].siblings = 0;
        for (int k = 0; k < g_ndev; k++) g_dev[i].siblings += k != i && ieq(g_dev[k].base, g_dev[i].base);
    }
    for (int i = 0; i < g_ndev; i++) {
        Dev *d = &g_dev[i];
        copy(d->name, sizeof d->name, d->base);
        if (d->siblings) {
            char shape[112];
            snprintf(shape, sizeof shape, "%s [A%d B%d]", d->base, d->naxes, d->nbuttons);
            int n = 1;
            for (int k = 0; k < i; k++) {
                size_t len = strlen(shape);
                n += ieq(g_dev[k].name, shape) || (!strncmp(g_dev[k].name, shape, len) && !strncmp(g_dev[k].name + len, " #", 2));
            }
            if (n > 1) snprintf(d->name, sizeof d->name, "%s #%d", shape, n);   /* two wheels of one kind */
            else copy(d->name, sizeof d->name, shape);
        }
        snprintf(d->id, sizeof d->id, "%04X:%04X %s", d->vid, d->pid, d->name);
    }
}

/* ================================================================== Windows: DirectInput 8 */
#ifdef _WIN32
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <SDL_syswm.h>

/* the GUIDs, here: nothing to link but dinput8.dll itself, loaded at run time */
#define WGUID(name, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID name = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
WGUID(W_IID_IDirectInput8A, 0xBF798030, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);
WGUID(W_GUID_XAxis, 0xA36D02E0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
WGUID(W_GUID_YAxis, 0xA36D02E1, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
WGUID(W_GUID_ZAxis, 0xA36D02E2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
WGUID(W_GUID_RxAxis, 0xA36D02F4, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
WGUID(W_GUID_RyAxis, 0xA36D02F5, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
WGUID(W_GUID_RzAxis, 0xA36D02E3, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
WGUID(W_GUID_Slider, 0xA36D02E4, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
WGUID(W_GUID_POV, 0xA36D02F2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
WGUID(W_GUID_ConstantForce, 0x13541C20, 0x8E33, 0x11D0, 0x9A, 0xD0, 0x00, 0xA0, 0xC9, 0xA0, 0x6E, 0x35);

#define DI_RANGE 10000
typedef struct { LONG axis[NAX]; DWORD pov[NPOV]; BYTE btn[NBTN]; } DiState;
#define AXIS_OFS(s) ((DWORD)(offsetof(DiState, axis) + (size_t)(s) * sizeof(LONG)))

static IDirectInput8A *g_di;
static HWND g_hwnd;
static GUID g_instance[MAX_DEV];    /* the devices open, as Windows knows them */
static int g_seen;                  /* how many were attached at the scan, opened or not */
static IDirectInputEffect *g_fx;
static int g_fx_dev = -1;
static DWORD g_fx_axis;
static double g_fx_last;
static Uint32 g_fx_retry, g_fx_check;
static int g_fx_tried;              /* since the devices or the settings changed */
static HRESULT g_fx_hr;             /* the last answer to a force */
static DWORD g_fx_status = ~0u;     /* the effect's status when last looked at, for the log */

/* the named axes of a joystick, two more for axes that have no name (a Fanatec base has one), the
 * hats and the buttons */
static const DIDATAFORMAT *data_format(void) {
    static DIOBJECTDATAFORMAT odf[NAX + NPOV + NBTN];
    static DIDATAFORMAT df;
    static const GUID *const axis_guid[NAX] = { &W_GUID_XAxis, &W_GUID_YAxis, &W_GUID_ZAxis, &W_GUID_RxAxis, &W_GUID_RyAxis,
                                                &W_GUID_RzAxis, &W_GUID_Slider, &W_GUID_Slider, NULL, NULL };
    if (df.dwSize) return &df;
    int n = 0;
    for (int s = 0; s < NAX; s++)
        odf[n++] = (DIOBJECTDATAFORMAT){ axis_guid[s], AXIS_OFS(s), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, DIDOI_ASPECTPOSITION };
    for (int p = 0; p < NPOV; p++)
        odf[n++] = (DIOBJECTDATAFORMAT){ &W_GUID_POV, (DWORD)(offsetof(DiState, pov) + (size_t)p * sizeof(DWORD)),
                                         DIDFT_POV | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0 };
    for (int b = 0; b < NBTN; b++)
        odf[n++] = (DIOBJECTDATAFORMAT){ NULL, (DWORD)(offsetof(DiState, btn) + (size_t)b),
                                         DIDFT_BUTTON | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0 };
    df.dwSize = sizeof df;
    df.dwObjSize = sizeof odf[0];
    df.dwFlags = DIDF_ABSAXIS;
    df.dwDataSize = sizeof(DiState);
    df.dwNumObjs = (DWORD)n;
    df.rgodf = odf;
    return &df;
}

static void dword_prop(IDirectInputDevice8A *dev, DWORD how, DWORD obj, REFGUID prop, DWORD value) {
    DIPROPDWORD dw;
    memset(&dw, 0, sizeof dw);
    dw.diph.dwSize = sizeof dw;
    dw.diph.dwHeaderSize = sizeof dw.diph;
    dw.diph.dwHow = how;
    dw.diph.dwObj = obj;
    dw.dwData = value;
    IDirectInputDevice8_SetProperty(dev, prop, &dw.diph);
}

static BOOL CALLBACK enum_device(const DIDEVICEINSTANCEA *inst, void *ctx) {
    (void)ctx;
    if (g_ndev >= MAX_DEV) return DIENUM_STOP;
    IDirectInputDevice8A *dev = NULL;
    if (FAILED(IDirectInput8_CreateDevice(g_di, &inst->guidInstance, &dev, NULL)) || !dev) return DIENUM_CONTINUE;
    /* Xbox pads ("IG_" in the path) are the gamepad code's; vJoy is a virtual device other programs feed */
    DIPROPGUIDANDPATH gp;
    memset(&gp, 0, sizeof gp);
    gp.diph.dwSize = sizeof gp;
    gp.diph.dwHeaderSize = sizeof gp.diph;
    gp.diph.dwHow = DIPH_DEVICE;
    int xinput = SUCCEEDED(IDirectInputDevice8_GetProperty(dev, DIPROP_GUIDANDPATH, &gp.diph)) &&
                 (wcsstr(gp.wszPath, L"IG_") || wcsstr(gp.wszPath, L"ig_"));
    if (xinput || strstr(inst->tszProductName, "vJoy") ||
        FAILED(IDirectInputDevice8_SetDataFormat(dev, data_format())) ||
        FAILED(IDirectInputDevice8_SetCooperativeLevel(dev, g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND))) {
        IDirectInputDevice8_Release(dev);
        return DIENUM_CONTINUE;
    }
    Dev *d = &g_dev[g_ndev];
    memset(d, 0, sizeof *d);
    g_instance[g_ndev] = inst->guidInstance;
    d->h = dev;
    d->ff_slot = -1;
    /* trimmed: cheap encoder boards pad their names with spaces */
    const char *nm = inst->tszProductName;
    while (*nm == ' ' || *nm == '\t') nm++;
    copy(d->base, sizeof d->base, *nm ? nm : "device");
    for (size_t e = strlen(d->base); e && (d->base[e - 1] == ' ' || d->base[e - 1] == '\t'); e--) d->base[e - 1] = 0;
    d->vid = LOWORD(inst->guidProduct.Data1);
    d->pid = HIWORD(inst->guidProduct.Data1);
    for (int s = 0; s < NAX; s++) {
        DIPROPRANGE r;
        memset(&r, 0, sizeof r);
        r.diph.dwSize = sizeof r;
        r.diph.dwHeaderSize = sizeof r.diph;
        r.diph.dwHow = DIPH_BYOFFSET;
        r.diph.dwObj = AXIS_OFS(s);
        if (FAILED(IDirectInputDevice8_GetProperty(dev, DIPROP_RANGE, &r.diph))) continue;
        d->present[s] = 1;
        d->naxes++;
        r.lMin = -DI_RANGE;
        r.lMax = DI_RANGE;
        IDirectInputDevice8_SetProperty(dev, DIPROP_RANGE, &r.diph);
        dword_prop(dev, DIPH_BYOFFSET, AXIS_OFS(s), DIPROP_DEADZONE, 0);          /* the port applies its own */
        dword_prop(dev, DIPH_BYOFFSET, AXIS_OFS(s), DIPROP_SATURATION, 10000);
        DIDEVICEOBJECTINSTANCEA oi;
        memset(&oi, 0, sizeof oi);
        oi.dwSize = sizeof oi;
        if (SUCCEEDED(IDirectInputDevice8_GetObjectInfo(dev, &oi, AXIS_OFS(s), DIPH_BYOFFSET)) &&
            (oi.dwFlags & DIDOI_FFACTUATOR) && d->ff_slot < 0) d->ff_slot = s;    /* the axis the motor acts on */
    }
    DIDEVCAPS caps;
    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    if (SUCCEEDED(IDirectInputDevice8_GetCapabilities(dev, &caps))) {
        d->nbuttons = caps.dwButtons > NBTN ? NBTN : (int)caps.dwButtons;
        d->npovs = caps.dwPOVs > NPOV ? NPOV : (int)caps.dwPOVs;
        d->ffb = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0 || d->ff_slot >= 0;
    }
    for (int p = 0; p < NPOV; p++) d->pov[p] = -1;
    IDirectInputDevice8_Acquire(dev);
    g_ndev++;
    return DIENUM_CONTINUE;
}

/* whether the devices attached are still the ones open (SDL announces every device once at its
 * start, and a base announces its parts one by one: neither is a reason to drop the motor) */
typedef struct { GUID guid[MAX_DEV + 1]; int n; } Attached;
static BOOL CALLBACK enum_attached(const DIDEVICEINSTANCEA *inst, void *ctx) {
    Attached *a = ctx;
    if (a->n <= MAX_DEV) a->guid[a->n++] = inst->guidInstance;
    return DIENUM_CONTINUE;
}
static int backend_changed(void) {
    Attached a = { .n = 0 };
    if (!g_di) return 1;
    IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, enum_attached, &a, DIEDFL_ATTACHEDONLY);
    for (int i = 0; i < g_ndev; i++) {      /* one of ours gone */
        int found = 0;
        for (int k = 0; k < a.n; k++) found |= !memcmp(&a.guid[k], &g_instance[i], sizeof(GUID));
        if (!found) return 1;
    }
    return a.n != g_seen;                   /* or a new one (with those not opened: Xbox pads, vJoy) */
}

static void backend_close(void) {
    ffb_end();
    for (int i = 0; i < g_ndev; i++) {
        IDirectInputDevice8A *dev = g_dev[i].h;
        IDirectInputDevice8_Unacquire(dev);
        IDirectInputDevice8_Release(dev);
    }
    g_ndev = 0;
}

static void backend_scan(void) {
    backend_close();
    if (!g_di) {
        HMODULE lib = LoadLibraryA("dinput8.dll");
        typedef HRESULT (WINAPI *Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
        Create create = lib ? (Create)(void *)GetProcAddress(lib, "DirectInput8Create") : NULL;
        if (!create || FAILED(create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &W_IID_IDirectInput8A, (LPVOID *)&g_di, NULL)) || !g_di) {
            g_di = NULL;
            rt_log("wheel: DirectInput is not available\n");
            return;
        }
    }
    IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, enum_device, NULL, DIEDFL_ATTACHEDONLY);
    Attached a = { .n = 0 };
    IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, enum_attached, &a, DIEDFL_ATTACHEDONLY);
    g_seen = a.n;
    g_fx_tried = 0;                         /* the motor is looked for again at once */
}

static void backend_poll(Dev *d) {
    IDirectInputDevice8A *dev = d->h;
    DiState st;
    if (!dev) return;                       /* (a device made up by tests/test_wheel.c) */
    memset(&st, 0, sizeof st);
    if (FAILED(IDirectInputDevice8_Poll(dev))) { IDirectInputDevice8_Acquire(dev); IDirectInputDevice8_Poll(dev); }
    if (FAILED(IDirectInputDevice8_GetDeviceState(dev, sizeof st, &st))) { IDirectInputDevice8_Acquire(dev); return; }
    for (int s = 0; s < NAX; s++)
        if (d->present[s]) d->axis[s] = SDL_clamp((double)st.axis[s] / DI_RANGE, -1.0, 1.0);
    for (int b = 0; b < d->nbuttons; b++) d->btn[b] = (st.btn[b] & 0x80) != 0;
    for (int p = 0; p < d->npovs; p++) d->pov[p] = (st.pov[p] & 0xffff) == 0xffff ? -1 : (int)st.pov[p];
}

/* ---- force feedback ---- */
static void fx_params(double force, DWORD flags) {
    DICONSTANTFORCE cf = { (LONG)lround(force * DI_FFNOMINALMAX) };
    DIEFFECT eff;
    memset(&eff, 0, sizeof eff);
    eff.dwSize = sizeof eff;
    eff.cbTypeSpecificParams = sizeof cf;
    eff.lpvTypeSpecificParams = &cf;
    HRESULT hr = IDirectInputEffect_SetParameters(g_fx, &eff, DIEP_TYPESPECIFICPARAMS | flags);
    if (hr != g_fx_hr) rt_log("wheel: force %+.2f: SetParameters %08lX\n", force, (unsigned long)hr);   /* each change of the answer */
    g_fx_hr = hr;
    if (FAILED(hr)) g_fx_check = 0;         /* looked at now */
}

static void ffb_set(double force) {
    if (!g_fx) return;
    force = SDL_clamp(force, -1.0, 1.0);
    if (force == g_fx_last) return;
    g_fx_last = force;
    fx_params(force, 0);
}

/* the wheel was taken away for a moment (another program, a sleep, the driver): take it back */
static void ffb_keep(Uint32 now) {
    if (!g_fx || (Sint32)(now - g_fx_check) < 0) return;
    g_fx_check = now + 500;
    DWORD status = 0;
    HRESULT hr = IDirectInputEffect_GetEffectStatus(g_fx, &status);
    if (FAILED(hr)) status = 0x80000000u | (hr & 0xffff);
    if (status != g_fx_status) rt_log("wheel: effect status %08lX (%08lX)\n", (unsigned long)status, (unsigned long)hr);
    g_fx_status = status;
    if (SUCCEEDED(hr) && (status & DIEGES_PLAYING)) return;
    IDirectInputDevice8A *dev = g_dev[g_fx_dev].h;
    IDirectInputDevice8_Acquire(dev);
    IDirectInputDevice8_SendForceFeedbackCommand(dev, DISFFC_SETACTUATORSON);
    fx_params(g_fx_last > 1 ? 0 : g_fx_last, DIEP_START);
}

static void ffb_end(void) {
    if (g_fx) {
        g_fx_last = 2;
        ffb_set(0);
        IDirectInputEffect_Stop(g_fx);
        IDirectInputEffect_Release(g_fx);
        g_fx = NULL;
    }
    if (g_fx_dev >= 0 && g_fx_dev < g_ndev) {
        IDirectInputDevice8A *dev = g_dev[g_fx_dev].h;
        IDirectInputDevice8_Unacquire(dev);
        IDirectInputDevice8_SetCooperativeLevel(dev, g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        IDirectInputDevice8_Acquire(dev);
    }
    g_fx_dev = -1;
}

/* a constant force on one device. On ONE axis: an effect across every force-feedback axis is
 * refused by some bases. The motor's own axis first, when the device says which it is (on a base
 * that is two devices the motor can be on the part that does not steer, where the steering
 * binding's axis means nothing), then the steering axis, then x. */
static int ffb_begin(int di, int steer_slot, char *err, size_t n) {
    Dev *d = &g_dev[di];
    IDirectInputDevice8A *dev = d->h;
    int slots[3], ns = 0;
    if (d->ff_slot >= 0) slots[ns++] = d->ff_slot;
    if (steer_slot >= 0 && d->present[steer_slot] && steer_slot != d->ff_slot) slots[ns++] = steer_slot;
    if (!ns) {
        for (int s = 0; s < NAX && !ns; s++)
            if (d->present[s]) slots[ns++] = s;
    }
    if (!ns) { snprintf(err, n, "NO AXIS"); return 0; }
    IDirectInputDevice8_Unacquire(dev);
    if (FAILED(IDirectInputDevice8_SetCooperativeLevel(dev, g_hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND)) ||
        FAILED(IDirectInputDevice8_Acquire(dev))) {
        IDirectInputDevice8_SetCooperativeLevel(dev, g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        IDirectInputDevice8_Acquire(dev);
        snprintf(err, n, "ANOTHER PROGRAM HOLDS THE WHEEL");
        return 0;
    }
    dword_prop(dev, DIPH_DEVICE, 0, DIPROP_AUTOCENTER, DIPROPAUTOCENTER_OFF);     /* the game centres the wheel */
    dword_prop(dev, DIPH_DEVICE, 0, DIPROP_FFGAIN, DI_FFNOMINALMAX);
    IDirectInputDevice8_SendForceFeedbackCommand(dev, DISFFC_SETACTUATORSON);
    HRESULT hr = E_FAIL;
    for (int k = 0; k < ns && !g_fx; k++) {
        DICONSTANTFORCE cf = { 0 };
        LONG dir = 0;
        DIEFFECT eff;
        memset(&eff, 0, sizeof eff);
        g_fx_axis = AXIS_OFS(slots[k]);
        eff.dwSize = sizeof eff;
        eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        eff.dwDuration = INFINITE;
        eff.dwGain = DI_FFNOMINALMAX;
        eff.dwTriggerButton = DIEB_NOTRIGGER;
        eff.cAxes = 1;
        eff.rgdwAxes = &g_fx_axis;
        eff.rglDirection = &dir;
        eff.cbTypeSpecificParams = sizeof cf;
        eff.lpvTypeSpecificParams = &cf;
        hr = IDirectInputDevice8_CreateEffect(dev, &W_GUID_ConstantForce, &eff, &g_fx, NULL);
        if (SUCCEEDED(hr) && g_fx && SUCCEEDED(hr = IDirectInputEffect_Start(g_fx, 1, 0))) break;
        if (g_fx) { IDirectInputEffect_Release(g_fx); g_fx = NULL; }
    }
    if (!g_fx) {
        IDirectInputDevice8_Unacquire(dev);
        IDirectInputDevice8_SetCooperativeLevel(dev, g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        IDirectInputDevice8_Acquire(dev);
        snprintf(err, n, "%s %08lX", d->ffb ? "EFFECT REFUSED" : "NO MOTOR", (unsigned long)hr);
        return 0;
    }
    g_fx_dev = di;
    g_fx_last = 2;
    g_fx_check = SDL_GetTicks() + 500;
    return 1;
}

static int same_hardware(int a, int b) { return ieq(g_dev[a].base, g_dev[b].base); }

/* The device chosen, else the one the steering axis is bound to, else the first that reports a
 * motor. Then among that device and the others of the same product name: those that report a
 * motor first, then the rest. A device that does not report one is still asked: the effect
 * starting is the real test. */
static void ffb_start(void) {
    int first = -1, steer_di = -1, steer_slot = -1;
    for (int i = 0; i < g_nbind[ACT_STEER] && steer_di < 0; i++)
        if (g_bind[ACT_STEER][i].type == B_AXIS && g_bind[ACT_STEER][i].di >= 0) {
            steer_di = g_bind[ACT_STEER][i].di;
            steer_slot = g_bind[ACT_STEER][i].code;
        }
    if (g_ffb_device[0]) {
        Bind b = { .type = B_BUTTON };
        copy(b.dev, sizeof b.dev, g_ffb_device);
        for (int i = 0; i < g_ndev && first < 0; i++)
            if (ieq(g_dev[i].id, g_ffb_device)) first = i;
        if (first < 0) first = resolve(&b);
    }
    if (first < 0) first = steer_di;
    for (int i = 0; i < g_ndev && first < 0; i++)
        if (g_dev[i].ffb) first = i;
    if (first < 0) { snprintf(g_ffb_status, sizeof g_ffb_status, g_ndev ? "NO MOTOR FOUND" : "NO WHEEL"); return; }
    char errs[160] = "";
    for (int pass = 0; pass < 2; pass++)
        for (int k = -1; k < g_ndev; k++) {
            int i = k < 0 ? first : k;
            if (k >= 0 && (i == first || !same_hardware(first, i))) continue;
            if (g_dev[i].ffb != (pass == 0)) continue;
            char err[64];
            if (ffb_begin(i, i == steer_di ? steer_slot : -1, err, sizeof err)) {
                snprintf(g_ffb_status, sizeof g_ffb_status, "%s", g_dev[i].name);
                rt_log("wheel: force feedback on %s (axis %s)\n", g_dev[i].id,
                       k_axis_name[(g_fx_axis - AXIS_OFS(0)) / sizeof(LONG)]);
                return;
            }
            size_t len = strlen(errs);
            snprintf(errs + len, sizeof errs - len, "%s%s", len ? ", " : "", err);
            rt_log("wheel: no force feedback on %s: %s\n", g_dev[i].id, err);
        }
    snprintf(g_ffb_status, sizeof g_ffb_status, "NO MOTOR FOUND [%s]", errs);
}

static void backend_start(void *sdl_window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    g_hwnd = sdl_window && SDL_GetWindowWMInfo(sdl_window, &info) ? info.info.win.window : NULL;
}

static void backend_stop(void) {
    backend_close();
    if (g_di) { IDirectInput8_Release(g_di); g_di = NULL; }
}

static void ffb_tick(double force, int wanted, Uint32 now) {
    if (!wanted) { if (g_fx) ffb_end(); g_fx_tried = 0; return; }
    if (!g_fx && g_ndev && (!g_fx_tried || (Sint32)(now - g_fx_retry) >= 0)) {
        g_fx_tried = 1;
        g_fx_retry = now + 5000;        /* a wheel still starting up, or held by its own software for a moment */
        ffb_start();
    }
    ffb_set(force);
    ffb_keep(now);
}
static int ffb_running(void) { return g_fx != NULL; }
static void ffb_retry_now(void) { g_fx_tried = 0; }
static int ffb_device_index(void) { return g_fx ? g_fx_dev : -1; }

#else
/* ================================================================== elsewhere: no devices yet */
static void backend_scan(void) {}
static int backend_changed(void) { return 0; }
static void backend_poll(Dev *d) { (void)d; }
static void backend_start(void *sdl_window) { (void)sdl_window; }
static void backend_stop(void) {}
static void ffb_end(void) {}
static void ffb_tick(double force, int wanted, Uint32 now) { (void)force; (void)wanted; (void)now; }
static int ffb_running(void) { return 0; }
static void ffb_retry_now(void) {}
static int ffb_device_index(void) { return -1; }
#endif

/* ================================================================== the bindings' file */
static void bind_add(int action, const Bind *b) {
    for (int i = 0; i < g_nbind[action]; i++) {
        const Bind *o = &g_bind[action][i];
        if (o->type == b->type && o->code == b->code && ieq(o->dev, b->dev)) { g_bind[action][i] = *b; return; }
    }
    if (g_nbind[action] == WHEEL_MAX_BINDS) g_nbind[action]--;      /* full: the last one gives way */
    g_bind[action][g_nbind[action]++] = *b;
}

static void bind_key(int action, SDL_Scancode sc) {
    Bind b = { .type = B_KEY, .code = sc, .di = -1 };
    bind_add(action, &b);
}

/* the keys of the port as it was before the bindings; the menus keep their own (arrows, Enter,
 * Backspace, Esc), which cannot be taken away */
static void defaults(void) {
    memset(g_nbind, 0, sizeof g_nbind);
    bind_key(ACT_STEER_LEFT, SDL_SCANCODE_LEFT);   bind_key(ACT_STEER_LEFT, SDL_SCANCODE_A);
    bind_key(ACT_STEER_RIGHT, SDL_SCANCODE_RIGHT); bind_key(ACT_STEER_RIGHT, SDL_SCANCODE_D);
    bind_key(ACT_GAS, SDL_SCANCODE_UP);            bind_key(ACT_GAS, SDL_SCANCODE_W);
    bind_key(ACT_BRAKE, SDL_SCANCODE_DOWN);        bind_key(ACT_BRAKE, SDL_SCANCODE_S);
    bind_key(ACT_HANDBRAKE, SDL_SCANCODE_SPACE);
    bind_key(ACT_SHIFT_UP, SDL_SCANCODE_E);
    bind_key(ACT_SHIFT_DOWN, SDL_SCANCODE_Q);
    bind_key(ACT_START, SDL_SCANCODE_1);
    bind_key(ACT_COIN, SDL_SCANCODE_5);
    bind_key(ACT_TEST, SDL_SCANCODE_F2);
    bind_key(ACT_SERVICE, SDL_SCANCODE_9);
}

static void bind_format(const Bind *b, char *buf, size_t n) {
    switch (b->type) {
    case B_KEY: snprintf(buf, n, "key %s", SDL_GetScancodeName((SDL_Scancode)b->code)); break;
    case B_BUTTON: snprintf(buf, n, "%s | button %d", b->dev, b->code + 1); break;
    case B_HAT: snprintf(buf, n, "%s | hat %d %s", b->dev, b->code / 4 + 1, k_hat_name[b->code % 4]); break;
    default: snprintf(buf, n, "%s | axis %s%s%s", b->dev, k_axis_name[b->code], b->inverted ? " inverted" : "", b->half ? " half" : ""); break;
    }
}

static int bind_parse(const char *v, Bind *b) {
    memset(b, 0, sizeof *b);
    b->di = -1;
    if (!strncmp(v, "key ", 4)) {
        SDL_Scancode sc = SDL_GetScancodeFromName(v + 4);
        if (sc == SDL_SCANCODE_UNKNOWN) return 0;
        b->type = B_KEY;
        b->code = sc;
        return 1;
    }
    const char *bar = NULL;
    for (const char *p = v; (p = strstr(p, " | ")); p += 3) bar = p;       /* the last one: a name may have its own */
    if (!bar || bar == v || (size_t)(bar - v) >= sizeof b->dev) return 0;
    memcpy(b->dev, v, (size_t)(bar - v));
    char what[16] = "", arg[16] = "", opt1[16] = "", opt2[16] = "";
    int fields = sscanf(bar + 3, "%15s %15s %15s %15s", what, arg, opt1, opt2);
    if (fields < 2) return 0;
    if (!strcmp(what, "button")) {
        b->type = B_BUTTON;
        b->code = atoi(arg) - 1;
        return b->code >= 0 && b->code < NBTN;
    }
    if (!strcmp(what, "hat")) {
        int hat = atoi(arg) - 1;
        for (int d = 0; d < 4; d++)
            if (!strcmp(opt1, k_hat_name[d]) && hat >= 0 && hat < NPOV) { b->type = B_HAT; b->code = hat * 4 + d; return 1; }
        return 0;
    }
    if (!strcmp(what, "axis"))
        for (int s = 0; s < NAX; s++)
            if (!strcmp(arg, k_axis_name[s])) {
                b->type = B_AXIS;
                b->code = s;
                b->inverted = !strcmp(opt1, "inverted") || !strcmp(opt2, "inverted");
                b->half = !strcmp(opt1, "half") || !strcmp(opt2, "half");
                return 1;
            }
    return 0;
}

/* the menus' own keys (frontend_sdl.c, menu_key, and Esc): they come as key events, so as bindings
 * of the menu actions they would count twice */
static int menu_fixed_key(int sc) {
    switch (SDL_GetKeyFromScancode((SDL_Scancode)sc)) {
    case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT: case SDLK_w: case SDLK_a: case SDLK_s: case SDLK_d:
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_1: case SDLK_BACKSPACE: case SDLK_ESCAPE: return 1;
    default: return 0;
    }
}

static void save(void) {
    if (!g_path[0]) return;
    FILE *f = fopen(g_path, "w");
    if (!f) { rt_log("wheel: cannot write %s\n", g_path); return; }
    fprintf(f, "# controls: bindings and force feedback (both modes). Written by the CONTROLS menu.\n\n[wheel]\n");
    fprintf(f, "# force feedback on the wheel's motor, percent: 0 = off, 100 = the cabinet motor's torque as it is\n"
               "ffb_strength = %d\n", g_ffb_strength);
    fprintf(f, "# 1 if the wheel pulls the wrong way (the menu's DETECT finds it)\nffb_invert = %d\n", g_ffb_invert);
    fprintf(f, "# the device with the motor; empty = found by itself (the steering device and those of the same name)\n"
               "ffb_device = %s\n", g_ffb_device);
    fprintf(f, "# percent of the steering axis that is full lock, and the dead zone around its centre\n"
               "steering_range = %d\nsteering_deadzone = %d\n", g_range, g_deadzone);
    fprintf(f, "\n[bindings]\n# one line per binding, up to %d per action; \"none\" = nothing bound\n"
               "#   key <name>                      a key, by its place on a US keyboard\n"
               "#   <device> | button <n>           a device is \"VID:PID product name\", as the menu lists them\n"
               "#   <device> | hat <n> up|right|down|left\n"
               "#   <device> | axis x|y|z|rx|ry|rz|slider0|slider1|extra0|extra1 [inverted] [half]\n", WHEEL_MAX_BINDS);
    for (int a = 0; a < ACT_COUNT; a++) {
        if (!g_nbind[a]) fprintf(f, "%s = none\n", k_action_key[a]);
        for (int i = 0; i < g_nbind[a]; i++) {
            char buf[220];
            bind_format(&g_bind[a][i], buf, sizeof buf);
            fprintf(f, "%s = %s\n", k_action_key[a], buf);
        }
    }
    fclose(f);
}

static void load(void) {
    FILE *f = fopen(g_path, "r");
    if (!f) return;
    char line[320], key[32], val[256];
    int seen[ACT_COUNT] = { 0 };
    while (fgets(line, sizeof line, f)) {
        val[0] = 0;
        if (line[0] == '#' || sscanf(line, " %31[a-z_0-9] = %255[^\r\n]", key, val) < 1) continue;
        for (size_t e = strlen(val); e && val[e - 1] == ' '; e--) val[e - 1] = 0;
        if (!strcmp(key, "ffb_strength")) g_ffb_strength = SDL_clamp(atoi(val), 0, 200);
        else if (!strcmp(key, "ffb_invert")) g_ffb_invert = atoi(val) != 0;
        else if (!strcmp(key, "ffb_device")) copy(g_ffb_device, sizeof g_ffb_device, val);
        else if (!strcmp(key, "steering_range")) g_range = SDL_clamp(atoi(val), 10, 100);
        else if (!strcmp(key, "steering_deadzone")) g_deadzone = SDL_clamp(atoi(val), 0, 30);
        else for (int a = 0; a < ACT_COUNT; a++)
            if (!strcmp(key, k_action_key[a])) {
                Bind b;
                if (!seen[a]) { seen[a] = 1; g_nbind[a] = 0; }      /* the file's, not the defaults */
                if (bind_parse(val, &b) && !(a >= ACT_PAUSE && b.type == B_KEY && menu_fixed_key(b.code))) bind_add(a, &b);
            }
    }
    fclose(f);
}

/* ================================================================== start, stop, devices */
static void scan(void) {
    backend_scan();
    name_devices();
    resolve_all();
    rt_log("wheel: %d device%s\n", g_ndev, g_ndev == 1 ? "" : "s");
    for (int i = 0; i < g_ndev; i++) {
        const Dev *d = &g_dev[i];
        rt_log("wheel:   %s: %d axes, %d buttons, %d hats%s%s%s\n", d->id, d->naxes, d->nbuttons, d->npovs,
               d->ffb ? ", force feedback" : "", d->ff_slot >= 0 ? " on " : "", d->ff_slot >= 0 ? k_axis_name[d->ff_slot] : "");
    }
}

void wheel_init(const char *settings_path) {
    defaults();
    g_path[0] = 0;
    if (!settings_path || !settings_path[0]) return;
    /* td2_enhanced_settings.ini and td2_settings.ini -> td2_controls.ini */
    char stem[1024];
    copy(stem, sizeof stem, settings_path);
    static const char *const ends[] = { "_enhanced_settings.ini", "_settings.ini", ".ini" };
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(stem), e = strlen(ends[i]);
        if (n > e && !strcmp(stem + n - e, ends[i])) { stem[n - e] = 0; break; }
    }
    snprintf(g_path, sizeof g_path, "%s_controls.ini", stem);
    load();
}

void wheel_start(void *sdl_window) {
    g_window = sdl_window;
    backend_start(sdl_window);
    scan();
}

void wheel_shutdown(void) {
    backend_stop();
    g_ndev = 0;
}

void wheel_devices_changed(void) {
    g_rescan = 1;
    g_rescan_at = SDL_GetTicks() + 700;     /* a base announces its devices one after the other */
}

unsigned wheel_generation(void) { return g_generation; }

int wheel_claims(unsigned vid, unsigned pid) {
    for (int a = 0; a < ACT_COUNT; a++)
        for (int i = 0; i < g_nbind[a]; i++) {
            int di = g_bind[a][i].di;
            if (di >= 0 && g_dev[di].vid == vid && g_dev[di].pid == pid) return 1;
        }
    return 0;
}

/* ================================================================== reading the bindings */
/* the keys the name entry types with (frontend_sdl.c, name_key) */
static int text_key(int sc) {
    SDL_Keycode k = SDL_GetKeyFromScancode((SDL_Scancode)sc);
    return k == SDLK_BACKSPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_LEFT || k == SDLK_RIGHT ||
           (k >= SDLK_SPACE && k <= SDLK_z) || (k >= SDLK_KP_1 && k <= SDLK_KP_PERIOD);
}

static double axis_value(const Bind *b, double v) {
    if (b->inverted) v = -v;
    v = b->half ? v : (v + 1) / 2;
    const double dz = 0.02;                 /* a pedal that does not rest exactly at its end */
    return v <= dz ? 0 : v >= 1 ? 1 : (v - dz) / (1 - dz);
}

static double bind_value(const Bind *b, int game, const Uint8 *keys, int focus) {
    if (b->type == B_KEY) return keys && keys[b->code] && !(game && g_key_block[b->code]) ? 1 : 0;
    if (b->di < 0 || !focus) return 0;
    const Dev *d = &g_dev[b->di];
    if (b->type == B_BUTTON) return d->btn[b->code] ? 1 : 0;
    if (b->type == B_HAT) return pov_dir(d->pov[b->code / 4], b->code % 4) ? 1 : 0;
    return axis_value(b, d->axis[b->code]);
}

static void capture_poll(Uint32 now);

void wheel_poll(int focus, int keys_mode) {
    Uint32 now = SDL_GetTicks();
    if (g_rescan && (Sint32)(now - g_rescan_at) >= 0 && g_cap < 0) { g_rescan = 0; if (backend_changed()) scan(); }
    Uint64 t0 = SDL_GetPerformanceCounter();
    for (int i = 0; i < g_ndev; i++) backend_poll(&g_dev[i]);
    double took = ms_since(t0);
    g_t_poll_sum += took;
    if (took > g_t_poll_max) g_t_poll_max = took;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (g_cap >= 0) {
        capture_poll(now);
        keys_mode = 2;
    }
    /* a key held when a menu had the keyboard stays released for the game until it is let go */
    for (int a = 0; a < ACT_COUNT; a++)
        for (int i = 0; i < g_nbind[a]; i++) {
            int sc = g_bind[a][i].code;
            if (g_bind[a][i].type != B_KEY) continue;
            if (!keys[sc]) g_key_block[sc] = 0;
            else if (keys_mode == 2 || (keys_mode == 1 && text_key(sc))) g_key_block[sc] = 1;
        }
    g_steer_on = 0;
    g_steer = 0;
    for (int a = 0; a < ACT_COUNT; a++) {
        double best = 0;
        for (int i = 0; i < g_nbind[a]; i++) {
            const Bind *b = &g_bind[a][i];
            if (a == ACT_STEER) {           /* the whole axis: its range and dead zone */
                if (b->type != B_AXIS || b->di < 0) continue;
                g_steer_on = 1;
                double v = g_dev[b->di].axis[b->code] * (b->inverted ? -1 : 1) * 100.0 / g_range;
                double dz = g_deadzone / 100.0, m = fabs(v);
                m = m <= dz ? 0 : m >= 1 ? 1 : (m - dz) / (1 - dz);
                if (focus && m > fabs(g_steer)) g_steer = copysign(m, v);
                continue;
            }
            if (a >= ACT_PAUSE && b->type == B_KEY && menu_fixed_key(b->code)) continue;
            double v = bind_value(b, a < ACT_PAUSE, keys, focus);
            if (v > best) best = v;
        }
        g_value[a] = g_cap >= 0 ? 0 : best;
        g_prev[a] = g_cur[a];
        g_cur[a] = g_value[a] > 0.5;
        g_edge[a] = g_cur[a] && !g_prev[a];
    }
}

double wheel_value(int action) { return g_value[action]; }
int wheel_down(int action) { return g_cur[action]; }
int wheel_pressed(int action) { return g_edge[action]; }
int wheel_steer(double *v) { *v = g_steer; return g_steer_on; }

/* ================================================================== capture */
void wheel_capture_begin(int action) {
    if (action < 0 || action >= ACT_COUNT) return;
    g_cap_until = SDL_GetTicks() + CAPTURE_MS;
    /* what is held or off-centre now is the rest state: only a change is bound */
    for (int i = 0; i < g_ndev; i++) {
        memcpy(g_cap_axis[i], g_dev[i].axis, sizeof g_cap_axis[i]);
        memcpy(g_cap_btn[i], g_dev[i].btn, sizeof g_cap_btn[i]);
        for (int p = 0; p < NPOV; p++) g_cap_hat[i][p] = g_dev[i].pov[p] >= 0;
    }
    g_cap = action;                         /* last: the scripted menus (RT_ENH_MENU) call this from the game's thread */
}

int wheel_capture_active(void) { return g_cap; }

static void capture_end(const Bind *b) {
    if (b) {
        bind_add(g_cap, b);
        resolve_all();
        save();
    }
    g_cap = -1;
    memset(g_cur, 1, sizeof g_cur);         /* what is held now is not a new press */
}

void wheel_capture_key(int scancode) {
    if (g_cap < 0) return;
    if (scancode == SDL_SCANCODE_ESCAPE) { capture_end(NULL); return; }
    if (g_cap == ACT_STEER || scancode <= 0 || scancode >= SDL_NUM_SCANCODES) return;   /* an axis only */
    if (g_cap >= ACT_PAUSE && menu_fixed_key(scancode)) return;     /* the menus have that key already */
    Bind b = { .type = B_KEY, .code = scancode, .di = -1 };
    g_key_block[scancode] = 1;
    capture_end(&b);
}

static void capture_poll(Uint32 now) {
    if ((Sint32)(now - g_cap_until) >= 0) { capture_end(NULL); return; }
    for (int i = 0; i < g_ndev; i++) {
        const Dev *d = &g_dev[i];
        Bind b = { .di = i };
        copy(b.dev, sizeof b.dev, d->id);
        /* the axis that moved the furthest from where it was */
        int slot = -1;
        double most = CAPTURE_AXIS_MOVE;
        for (int s = 0; s < NAX; s++)
            if (d->present[s] && fabs(d->axis[s] - g_cap_axis[i][s]) > most) { most = fabs(d->axis[s] - g_cap_axis[i][s]); slot = s; }
        if (slot >= 0) {
            b.type = B_AXIS;
            b.code = slot;
            b.inverted = d->axis[slot] < g_cap_axis[i][slot];
            b.half = g_cap != ACT_STEER && fabs(g_cap_axis[i][slot]) <= 0.5;
            capture_end(&b);
            return;
        }
        if (g_cap == ACT_STEER) continue;
        for (int k = 0; k < d->nbuttons; k++) {
            if (!d->btn[k]) g_cap_btn[i][k] = 0;
            else if (!g_cap_btn[i][k]) { b.type = B_BUTTON; b.code = k; capture_end(&b); return; }
        }
        for (int p = 0; p < d->npovs; p++) {
            if (d->pov[p] < 0) { g_cap_hat[i][p] = 0; continue; }
            if (g_cap_hat[i][p]) continue;
            for (int dir = 0; dir < 4; dir++)
                if (d->pov[p] == dir * 9000) { b.type = B_HAT; b.code = p * 4 + dir; capture_end(&b); return; }
        }
    }
}

/* ================================================================== force feedback */
static Uint32 g_test_until;
static double g_detect_force;       /* a raw force while the direction is being found */
static int g_detecting;

void wheel_ffb_update(uint8_t motor, int active) {
    Uint32 now = SDL_GetTicks();
    double force = 0;
    int torque = motor & 15;
    if (g_detecting) force = g_detect_force;
    else if (g_test_until && (Sint32)(now - g_test_until) < 0)
        force = ((g_test_until - now) / 65) & 1 ? 0.35 : -0.35;
    else {
        g_test_until = 0;
        /* DirectInput's sign is where the force comes from: a pull to the right is negative */
        if (active && (motor & 0x80) && torque)
            force = torque / 15.0 * g_ffb_strength / 100.0 * ((motor & 0x10) ? -1 : 1) * (g_ffb_invert ? -1 : 1);
    }
    Uint64 t0 = SDL_GetPerformanceCounter();
    ffb_tick(force, g_ffb_strength > 0, now);
    double took = ms_since(t0);
    g_t_ffb_sum += took;
    if (took > g_t_ffb_max) g_t_ffb_max = took;
    static int trace = -1;                  /* RT_WHEEL_LOG: the motor and the force, once a second */
    static Uint32 trace_at;
    static double peak;
    if (trace < 0) trace = getenv("RT_WHEEL_LOG") != NULL;
    if (fabs(force) > fabs(peak)) peak = force;
    if (trace && (Sint32)(now - trace_at) >= 0) {
        trace_at = now + 1000;
        rt_log("wheel: motor=%02x active=%d force=%+.2f peak=%+.2f running=%d | poll %.1f ms/s max %.2f, ffb %.1f ms/s max %.2f\n",
               motor, active, force, peak, ffb_running(), g_t_poll_sum, g_t_poll_max, g_t_ffb_sum, g_t_ffb_max);
        g_t_poll_sum = g_t_poll_max = g_t_ffb_sum = g_t_ffb_max = 0;
        peak = 0;
    }
}

void wheel_ffb_test(void) {
    g_test_until = SDL_GetTicks() + 850;
    if (!g_test_until) g_test_until = 1;
}

/* pushes one way for a moment and watches where the wheel goes: the motor's sign differs between
 * wheels, and the game must pull the way it means. Nothing has to be bound for it: the axes of
 * the device with the motor, and of the devices of the same name, are watched and the one that
 * moved the most is the wheel (a DirectInput wheel's axis grows to the right). A steering binding
 * on that hardware is used instead, with the direction it was bound in. */
int wheel_ffb_detect(void) {
    if (g_ffb_strength <= 0) return 0;
    g_detecting = 1;
    g_detect_force = 0;
    ffb_retry_now();
    wheel_ffb_update(0, 0);
    int fd = ffb_device_index();
    if (fd < 0) { g_detecting = 0; return 0; }
    static double from[MAX_DEV][NAX];
    for (int i = 0; i < g_ndev; i++) {
        backend_poll(&g_dev[i]);
        memcpy(from[i], g_dev[i].axis, sizeof from[i]);
    }
    g_detect_force = 0.55;
    for (int t = 0; t < 45; t++) {
        wheel_ffb_update(0, 0);
        SDL_Delay(10);
    }
    for (int i = 0; i < g_ndev; i++) backend_poll(&g_dev[i]);
    g_detecting = 0;
    wheel_ffb_update(0, 0);
    double moved = 0;
    const Bind *steer = NULL;
    for (int i = 0; i < g_nbind[ACT_STEER] && !steer; i++) {
        const Bind *b = &g_bind[ACT_STEER][i];
        if (b->type == B_AXIS && b->di >= 0 && ieq(g_dev[b->di].base, g_dev[fd].base)) steer = b;
    }
    if (steer) moved = (g_dev[steer->di].axis[steer->code] - from[steer->di][steer->code]) * (steer->inverted ? -1 : 1);
    else
        for (int i = 0; i < g_ndev; i++) {
            if (!ieq(g_dev[i].base, g_dev[fd].base)) continue;
            for (int s = 0; s < NAX; s++)
                if (g_dev[i].present[s] && fabs(g_dev[i].axis[s] - from[i][s]) > fabs(moved)) moved = g_dev[i].axis[s] - from[i][s];
        }
    if (fabs(moved) < 0.03) { rt_log("wheel: direction detection: the wheel did not move (%+.3f)\n", moved); return 0; }
    g_ffb_invert = moved > 0;               /* a positive force turned it right: the game's right is that sign */
    save();
    rt_log("wheel: force feedback direction %s (the wheel moved %+.2f)\n", g_ffb_invert ? "inverted" : "normal", moved);
    return 1;
}

const char *wheel_ffb_status(void) {
    static char text[200];
    if (g_ffb_strength <= 0) return "OFF";
    if (ffb_running()) return "OK";         /* (the device is in the mapping's list) */
    snprintf(text, sizeof text, "%.24s", g_ffb_status);
    menu_text(text);
    return text;
}

/* ================================================================== settings */
/* another device chosen: the motor is looked for at once, not at the next retry */
static void ffb_restart(void) { ffb_end(); ffb_retry_now(); wheel_ffb_update(0, 0); }

int wheel_ffb_strength(void) { return g_ffb_strength; }
void wheel_set_ffb_strength(int percent) { g_ffb_strength = SDL_clamp(percent, 0, 200); save(); }
int wheel_ffb_invert(void) { return g_ffb_invert; }
void wheel_set_ffb_invert(int on) { g_ffb_invert = !!on; save(); }
int wheel_range(void) { return g_range; }
void wheel_set_range(int percent) { g_range = SDL_clamp(percent, 10, 100); save(); }
int wheel_deadzone(void) { return g_deadzone; }
void wheel_set_deadzone(int percent) { g_deadzone = SDL_clamp(percent, 0, 30); save(); }

void wheel_ffb_device_step(int dir) {
    int cur = -1;                           /* -1 = AUTO */
    for (int i = 0; i < g_ndev; i++)
        if (ieq(g_dev[i].id, g_ffb_device)) cur = i;
    cur += dir < 0 ? -1 : 1;
    if (cur < -1) cur = g_ndev - 1;
    if (cur >= g_ndev) cur = -1;
    copy(g_ffb_device, sizeof g_ffb_device, cur < 0 ? "" : g_dev[cur].id);
    save();
    ffb_restart();
}

const char *wheel_ffb_device_name(void) {
    static char text[128];
    if (!g_ffb_device[0]) return NULL;
    const char *name = strchr(g_ffb_device, ' ');
    snprintf(text, sizeof text, "%.22s", name ? name + 1 : g_ffb_device);    /* what fits next to its label */
    menu_text(text);
    return text;
}

/* ================================================================== for the menu */
const char *wheel_action_name(int action, int lang) { return k_action_name[action][lang != 0]; }
int wheel_bind_count(int action) { return g_nbind[action]; }

void wheel_bind_text(int action, int i, char *buf, size_t n) {
    const Bind *b = &g_bind[action][i];
    char dev[8] = "J-";                     /* not connected */
    if (b->di >= 0) snprintf(dev, sizeof dev, "J%d", b->di + 1);
    switch (b->type) {
    case B_KEY: snprintf(buf, n, "KEY %s", SDL_GetScancodeName((SDL_Scancode)b->code)); break;
    case B_BUTTON: snprintf(buf, n, "%s BTN %d", dev, b->code + 1); break;
    case B_HAT: snprintf(buf, n, "%s HAT %s", dev, k_hat_name[b->code % 4]); break;
    default: snprintf(buf, n, "%s AXIS %s%s", dev, k_axis_name[b->code], b->inverted ? " INV" : ""); break;
    }
    menu_text(buf);
}

void wheel_bind_remove_last(int action) {
    if (!g_nbind[action]) return;
    g_nbind[action]--;
    g_generation++;
    save();
}

void wheel_bind_defaults(void) {
    defaults();
    resolve_all();
    save();
}

int wheel_device_count(void) { return g_ndev; }

void wheel_device_text(int i, char *buf, size_t n) {
    snprintf(buf, n, "J%d %s%s", i + 1, g_dev[i].name, i == ffb_device_index() ? " - FFB" : "");
    menu_text(buf);
}
