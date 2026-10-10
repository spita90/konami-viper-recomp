/*
 * Konami Viper recompiled game - entry point.
 *
 * Does what BIOS 941B01 does before handing over: the kernel (decompressed
 * boot block of the game file on the CF card) is placed at RAM 0 and entered
 * at 0x10 with MSR = 0x2070 (FP, IP, IR, DR).
 */
#include "runtime.h"
#include "modules.h"
#include "game_config.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <windows.h>
/* laptops with two GPUs: run OpenGL (the GPU renderer) on the dedicated one */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

uint8_t *g_ram;
static int g_verbose;
static double g_run_seconds = 0;
static FILE *g_wav;
static uint32_t g_wav_bytes;

int rt_verbose(void) { return g_verbose; }

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%9.4f ", (double)rt_now() / CPU_HZ);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void voodoo_stats(void);

static void wav_close(void) {
    if (!g_wav) return;
    uint32_t riff = 36 + g_wav_bytes;
    fseek(g_wav, 4, SEEK_SET); fwrite(&riff, 4, 1, g_wav);
    fseek(g_wav, 40, SEEK_SET); fwrite(&g_wav_bytes, 4, 1, g_wav);
    fclose(g_wav);
    g_wav = NULL;
}

static uint8_t *g_kernel_img;
static size_t g_kernel_len;

static void diff_kernel_text(void) {
    if (!g_kernel_img || g_kernel_len < 0x1c) return;
    uint32_t end = bswap32(*(uint32_t *)(g_kernel_img + 0x18));   /* text end, as read by recomp.py */
    if (end > g_kernel_len) end = (uint32_t)g_kernel_len;
    int shown = 0;
    for (uint32_t a = 0x10; a + 4 <= end && shown < 40; a += 4) {
        if (memcmp(g_ram + a, g_kernel_img + a, 4)) {
            uint32_t s = a;
            while (a + 4 <= end && memcmp(g_ram + a, g_kernel_img + a, 4)) a += 4;
            rt_log("  kernel text modified %08x-%08x: %08x -> %08x\n", s, a, bswap32(*(uint32_t *)(g_kernel_img + s)),
                   bswap32(*(uint32_t *)(g_ram + s)));
            shown++;
        }
    }
}

static const char *g_dump_ram;

void rt_fatal(const char *why) {
    if (!strcmp(why, "window closed")) { net_shutdown(); hw_shutdown(); wav_close(); fflush(stderr); _exit(0); }
    rt_log("STOP: %s\n", why);
    if (g_dump_ram) { FILE *f = fopen(g_dump_ram, "wb"); if (f) { fwrite(g_ram, 1, RAM_SIZE, f); fclose(f); } }
    diff_kernel_text();
    rt_dump_state();
    voodoo_stats();
    extern void epic_dump(void);
    epic_dump();
    hw_shutdown();
    wav_close();
    fflush(stderr);
    exit(strcmp(why, "time limit") ? 1 : 0);
}

/* Audio: each IRQ3 block is 0x800 bytes = 256 stereo frames of 32-bit words. For now
 * the blocks are dumped to a WAV file (16-bit, upper half of each word). */
void audio_push_block(const uint8_t *blk) {
    audio_frontend_push(blk);
    if (!g_wav) return;
    for (int i = 0; i < 512; i++) {
        uint32_t w = ((uint32_t)blk[4 * i] << 24) | ((uint32_t)blk[4 * i + 1] << 16) | ((uint32_t)blk[4 * i + 2] << 8) | blk[4 * i + 3];
        int16_t s = (int16_t)(w >> 16);
        fwrite(&s, 2, 1, g_wav);
        g_wav_bytes += 2;
    }
}

static void wav_open(const char *path) {
    g_wav = fopen(path, "wb");
    if (!g_wav) return;
    static const uint8_t hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                                    0x44,0xac,0,0,0x10,0xb1,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    fwrite(hdr, 1, 44, g_wav);
}

static const char *g_frames_dir;
static int g_frame_every = 30;

/* RT_INPUT_FIFO=path: live scripted input from a named pipe (mkfifo), lines "port=hex" with the
 * ports of RT_INPUT, read at every frame without blocking (a headless linked node driven by hand) */
static void input_fifo_poll(void) {
    static int fd = -2;
    static char line[256];
    static size_t len;
    extern uint8_t g_in[8];
    extern int16_t g_analog[4];
#ifdef _WIN32
    fd = -1;                                        /* no named pipes: a test tool for macOS/Linux */
#else
    if (fd == -2) { const char *p = getenv("RT_INPUT_FIFO"); fd = p ? open(p, O_RDONLY | O_NONBLOCK) : -1; }
#endif
    if (fd < 0) return;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c != '\n' && c != ',') { if (len < sizeof line - 1) line[len++] = c; continue; }
        line[len] = 0;
        len = 0;
        int port; unsigned v;
        if (sscanf(line, "%d=%x", &port, &v) != 2) continue;
        if (port >= 10) g_analog[(port - 10) & 3] = (int16_t)v;
        else g_in[port & 7] = (uint8_t)v;
        rt_log("input (fifo): %s%d = %02x\n", port >= 10 ? "AN" : "IN", port >= 10 ? port - 10 : port, v);
    }
}

void rt_frame_published(uint64_t cnt, const uint32_t *buf, int w, int h) {
    enh_on_frame(buf, w, h);
    net_frame();
    input_fifo_poll();
    static int fps_stats = -1;
    static uint64_t last_hash, uniq, last_sec;
    if (fps_stats < 0) fps_stats = getenv("RT_FPS_STATS") != NULL;
    if (!buf) return;                               /* GPU renderer: the picture is on the GPU */
    if (fps_stats) {
        uint64_t hsh = 1469598103934665603ull;
        for (int i = 0; i < w * h; i += 7) hsh = (hsh ^ buf[i]) * 1099511628211ull;
        if (hsh != last_hash) uniq++;
        last_hash = hsh;
        uint64_t sec = (uint64_t)((double)rt_now() / CPU_HZ);
        if (sec != last_sec) { rt_log("fps: %llu distinct frames in second %llu\n", (unsigned long long)uniq, (unsigned long long)last_sec); uniq = 0; last_sec = sec; }
    }
    if (!g_frames_dir || cnt % (uint64_t)g_frame_every) return;
    static uint32_t shot[2048 * 2048];
    if (g_enhanced && w * h <= 2048 * 2048) {     /* the dumped frames show the menu too */
        memcpy(shot, buf, (size_t)w * h * 4);
        enh_draw_overlay(shot, w, h);
        buf = shot;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06llu.ppm", g_frames_dir, (unsigned long long)cnt);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t px[3] = {(uint8_t)(buf[i] >> 16), (uint8_t)(buf[i] >> 8), (uint8_t)buf[i]};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

#ifdef _WIN32
static void dump_frames_loop(void) { for (;;) Sleep(INFINITE); }
#else
static void dump_frames_loop(void) { for (;;) pause(); }
#endif

/* scripted input for tests: RT_INPUT="20.0:3=fb,20.2:3=ff" (seconds:port=hex);
 * ports 0-7 = digital IN0-7, 10-13 = analog positions AN0-3 (16-bit two's complement, -255..255) */
extern uint8_t g_in[8];
extern int16_t g_analog[4];
typedef struct { double t; int port; uint16_t v; } InEv;
static InEv g_inev[200];
static void inev_fire(void *arg) {
    InEv *e = (InEv *)arg;
    if (e->port >= 10) g_analog[(e->port - 10) & 3] = (int16_t)e->v;
    else g_in[e->port & 7] = (uint8_t)e->v;
    rt_log("input: %s%d = %02x\n", e->port >= 10 ? "AN" : "IN", e->port >= 10 ? e->port - 10 : e->port, e->v);
}
static void inev_setup(void) {
    const char *s = getenv("RT_INPUT");
    int n = 0;
    while (s && *s && n < 200) {
        double t; int port; unsigned v; int used = 0;
        if (sscanf(s, "%lf:%d=%x%n", &t, &port, &v, &used) != 3) break;
        g_inev[n] = (InEv){t, port, (uint16_t)v};
        rt_sched_at((uint64_t)(t * CPU_HZ), inev_fire, &g_inev[n]);
        n++;
        s += used;
        while (*s == ',') s++;
    }
}

/* First-run calibration: drives the game's own TEST MODE -> CALIBRATION with scripted
 * inputs (steering centre/left/right, accelerator and brake rest/full), then SAVE AND EXIT.
 * The resulting NVRAM matches the frontend's analog ranges exactly. The script is per game
 * (games/<id>/game.json, "calibration"); NULL means the game has none yet. RT_FFB_WHEEL makes
 * the virtual wheel follow the force-feedback motor, for cabinets whose NVRAM declares one
 * (the motor-driven steering test would otherwise fail, as on a real cabinet without it). */
static const char *k_calibration_script = GAME_CALIBRATION_SCRIPT;

static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

/* Runs this executable headless with a scripted input sequence (TEST MODE automation) that ends
 * by saving the NVRAM to nvsave; an existing nvsave is loaded first, so passes can be chained. */
static int run_scripted_pass(const char *what, const char *script, int seconds, const char *self,
                             const char *work, const char *nvram, const char *nvsave) {
    char cmd[8192];
#ifdef _WIN32                           /* cmd.exe: the variables through the inherited environment */
    _putenv_s("RT_FFB_WHEEL", "1");
    _putenv_s("RT_INPUT", script);
    snprintf(cmd, sizeof cmd, "\"\"%s\" --headless --work \"%s\" --nvram \"%s\" --nvram-save \"%s\" --seconds %d >NUL 2>&1\"",
             self, work, nvram, nvsave, seconds);
    int rc = system(cmd);
    _putenv_s("RT_FFB_WHEEL", "");
    _putenv_s("RT_INPUT", "");
#else
    snprintf(cmd, sizeof cmd, "RT_FFB_WHEEL=1 RT_INPUT='%s' '%s' --headless --work '%s' --nvram '%s' --nvram-save '%s' --seconds %d >/dev/null 2>&1",
             script, self, work, nvram, nvsave, seconds);
    int rc = system(cmd);
#endif
    if (rc != 0 || !file_exists(nvsave)) { fprintf(stderr, "%s failed (rc=%d)\n", what, rc); return -1; }
    return 0;
}

/* settings written into the calibrated NVRAM, the profile's calibration.nvram_set: ones TEST MODE
 * cannot change (e.g. a region-locked currency) or defaults of the port (GTI Club 2's promotion mode) */
static void calibration_nvram_set(const char *nvsave) {
    static const struct { int addr, val; } k_set[] = GAME_CALIBRATION_NVRAM;
    if (k_set[0].addr < 0) return;
    static uint8_t nv[0x2000];
    FILE *f = fopen(nvsave, "rb");
    if (!f) return;
    size_t n = fread(nv, 1, sizeof nv, f);
    fclose(f);
    if (n != sizeof nv) return;
    for (int i = 0; k_set[i].addr >= 0; i++) nv[k_set[i].addr] = (uint8_t)k_set[i].val;
    hw_nvram_options_fix(nv);
    if ((f = fopen(nvsave, "wb"))) { fwrite(nv, 1, sizeof nv, f); fclose(f); }
}

static int run_first_time_calibration(const char *self, const char *work, const char *nvram, const char *nvsave) {
    if (!k_calibration_script) return 0;
    fprintf(stderr, "first run: calibrating steering and pedals in TEST MODE (a few seconds)...\n");
    if (run_scripted_pass("calibration", k_calibration_script, GAME_CALIBRATION_SECONDS, self, work, nvram, nvsave)) return -1;
    calibration_nvram_set(nvsave);
    fprintf(stderr, "calibration saved to %s\n", nvsave);
    return 0;
}

/* enhanced mode, first launch: after the calibration, the profile's setup pass (free play) */
static int run_enhanced_setup(const char *self, const char *work, const char *nvram, const char *nvsave) {
    static const char *k_setup = GAME_ENH_SETUP_SCRIPT;
    if (!k_setup) return 0;
    fprintf(stderr, "first run (enhanced mode): setting up the game options in TEST MODE...\n");
    if (run_scripted_pass("enhanced setup", k_setup, GAME_ENH_SETUP_SECONDS, self, work, nvram, nvsave)) return -1;
    fprintf(stderr, "enhanced settings saved to %s\n", nvsave);
    return 0;
}

static void stop_event(void *arg) { (void)arg; rt_fatal("time limit"); }
static void on_sigint(int s) { (void)s; rt_fatal("interrupted"); }


static const RtModuleInfo *const k_modules[] = { RT_ALL_MODULES };

static const char *g_argv0 = "";

/* The default files (work/, roms/, the saved NVRAM) live next to the executable, so it also
 * works when started from another directory, e.g. by double-clicking it in the file manager.
 * Paths given on the command line stay relative to the current directory. */
static char g_exe[PATH_MAX], g_exe_dir[PATH_MAX];


static void find_executable(const char *argv0) {
    char raw[PATH_MAX] = "";
#ifdef __APPLE__
    uint32_t size = sizeof raw;
    if (_NSGetExecutablePath(raw, &size) != 0) raw[0] = 0;
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, raw, sizeof raw);
    raw[n < sizeof raw ? n : 0] = 0;
#define realpath(p, out) _fullpath(out, p, PATH_MAX)
#else
    ssize_t n = readlink("/proc/self/exe", raw, sizeof raw - 1);
    raw[n > 0 ? n : 0] = 0;
#endif
    if (!raw[0] || !realpath(raw, g_exe)) {
        if (!realpath(argv0, g_exe)) snprintf(g_exe, sizeof g_exe, "%s", argv0);
    }
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", g_exe);
    char *slash = strrchr(g_exe_dir, '/');
#ifdef _WIN32
    char *bs = strrchr(g_exe_dir, '\\');
    if (bs > slash) slash = bs;
#endif
    if (slash) *slash = 0; else snprintf(g_exe_dir, sizeof g_exe_dir, ".");
}

static const char *beside_exe(const char *rel) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s/%s", g_exe_dir, rel);
    return strdup(buf);
}

static void usage(void) {
    fprintf(stderr,
            GAME_TITLE ", recompiled\n"
            "usage: %s [options]   (default paths are relative to the executable's directory)\n"
            "  --work DIR      extracted data (kernel.bin), default: " GAME_DEFAULT_WORK "\n"
            "  --cf FILE       raw CF image, default: DIR/cf.img\n"
            "  --nvram FILE    M48T58 dump, default: " GAME_DEFAULT_NVRAM "\n"
            "  --ds2430 FILE   DS2430A dump, default: " GAME_DEFAULT_DS2430 "\n"
            "  --bios FILE     BIOS (optional), default: " GAME_DEFAULT_BIOS "\n"
            "  --seconds N     stop after N seconds of emulated time\n"
            "  --wav FILE      dump game audio\n"
            "  --headless      no window/audio (tests); runs as fast as possible\n"
            "  --scale N       window scale (default 2, or the size of the last run)\n"
            "  --volume N      audio gain (default 16)\n"
            "  --nvram-save F  persistent NVRAM file (default: the mode's, see below)\n"
            "  --classic       classic mode, as the cabinet (coins, TEST MODE; NVRAM " GAME_NVRAM_SAVE ")\n"
            "  --enhanced      enhanced mode, the default (free play, menus; NVRAM " GAME_ENH_NVRAM_SAVE ")\n"
            "  --settings F    port settings (default " GAME_ENH_SETTINGS ", classic " GAME_SETTINGS ")\n"
            "  --frames DIR    dump every Nth video frame as PPM into DIR (headless)\n"
            "  --frame-every N (default 30)\n"
            "  --net-host      link play: host a session (NETWORK ID 1) at --net-port\n"
            "  --net-join H:P  link play: join the host H:P; it gives the NETWORK ID (enhanced mode)\n"
            "  --net-id N      link play, tests: this node's NETWORK ID (1 = host; 2-4 with --net-peer)\n"
            "  --net-peer H:P  link play, tests: the host of --net-id 2-4\n"
            "  --net-port P    link play: local UDP port (default 24700)\n"
            "  --net-buffer N  link play: playout buffer in cycles against jitter (default 2)\n"
            "  --realtime      headless at the speed of the clock (a linked node with no window)\n"
            "  -v              verbose\n", g_argv0);
    exit(2);
}

int main(int argc, char **argv) {
    g_argv0 = argv[0];
    find_executable(argv[0]);
    const char *settings = beside_exe(GAME_SETTINGS);
    const char *work = beside_exe(GAME_DEFAULT_WORK), *cf = NULL, *nvram = beside_exe(GAME_DEFAULT_NVRAM),
               *ds = beside_exe(GAME_DEFAULT_DS2430), *bios = beside_exe(GAME_DEFAULT_BIOS), *wav = NULL,
               *nvsave = beside_exe(GAME_NVRAM_SAVE);
    int headless = 0, scale = 2, scale_explicit = 0, nvsave_explicit = 0, settings_explicit = 0;
    int net_id = 0, net_port = NET_DEFAULT_PORT, net_buffer = 2;
    const char *net_peer = NULL, *net_join_to = NULL;
    int net_host_flag = 0, mode = 0;          /* mode: 0 default, 1 --enhanced, -1 --classic */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--work") && v) { work = v; i++; }
        else if (!strcmp(a, "--cf") && v) { cf = v; i++; }
        else if (!strcmp(a, "--nvram") && v) { nvram = v; i++; }
        else if (!strcmp(a, "--ds2430") && v) { ds = v; i++; }
        else if (!strcmp(a, "--bios") && v) { bios = v; i++; }
        else if (!strcmp(a, "--seconds") && v) { g_run_seconds = atof(v); i++; }
        else if (!strcmp(a, "--wav") && v) { wav = v; i++; }
        else if (!strcmp(a, "--headless")) headless = 1;
        else if (!strcmp(a, "--volume") && v) { extern int g_audio_gain; g_audio_gain = atoi(v); i++; }
        else if (!strcmp(a, "--scale") && v) { scale = atoi(v); scale_explicit = 1; i++; }
        else if (!strcmp(a, "--nvram-save") && v) { nvsave = v; nvsave_explicit = 1; i++; }
        else if (!strcmp(a, "--dump-ram") && v) { g_dump_ram = v; i++; }
        else if (!strcmp(a, "--frames") && v) { g_frames_dir = v; i++; }
        else if (!strcmp(a, "--frame-every") && v) { g_frame_every = atoi(v); i++; }
        else if (!strcmp(a, "-v")) g_verbose = 1;
        else if (!strcmp(a, "--enhanced")) mode = 1;
        else if (!strcmp(a, "--classic")) mode = -1;
        else if (!strcmp(a, "--settings") && v) { settings = v; settings_explicit = 1; i++; }
        else if (!strcmp(a, "--net-id") && v) { net_id = atoi(v); i++; }
        else if (!strcmp(a, "--net-host")) net_host_flag = 1;
        else if (!strcmp(a, "--net-join") && v) { net_join_to = v; i++; }
        else if (!strcmp(a, "--net-port") && v) { net_port = atoi(v); i++; }
        else if (!strcmp(a, "--net-peer") && v) { net_peer = v; i++; }
        else if (!strcmp(a, "--net-buffer") && v) { net_buffer = atoi(v); i++; }
        else if (!strcmp(a, "--realtime")) { extern int g_realtime; g_realtime = 1; }
        else usage();
    }
    /* the enhanced mode is the default where the profile has one; --classic is the cabinet */
    g_enhanced = mode >= 0 && GAME_HAS_ENHANCED;
    if (mode > 0 && !GAME_HAS_ENHANCED) { fprintf(stderr, "the enhanced mode is not available for " GAME_TITLE " yet\n"); return 2; }
    if (g_enhanced) {
        if (!nvsave_explicit) nvsave = beside_exe(GAME_ENH_NVRAM_SAVE);
        if (!settings_explicit) settings = beside_exe(GAME_ENH_SETTINGS);
    }
    if (net_id < 0 || net_id > 4) { fprintf(stderr, "--net-id wants 1-4\n"); return 2; }
    net_set_port(net_port);
    if (net_join_to && !g_enhanced) { fprintf(stderr, "--net-join wants the enhanced mode, not --classic (the NETWORK ID is taken live there)\n"); return 2; }
    int net_rc = 0;
    if (net_host_flag || (net_id == 1 && !net_peer)) net_rc = net_host(net_port, net_buffer);
    else if (net_join_to) net_rc = net_join(net_join_to, 0, 0, 0, net_buffer);
    else if (net_id && net_peer) net_rc = net_join(net_peer, net_port, net_id, 0, net_buffer);
    else if (net_id) { fprintf(stderr, "--net-id %d wants --net-peer host:port\n", net_id); return 2; }
    if (net_rc) return 1;
    atexit(net_shutdown);
    enh_set_headless(headless);
    enh_init(work, settings);
    if (!headless && enh_want_gpu()) voodoo_set_gpu(1);  /* the frontend falls back if it cannot */
    if (headless && getenv("RT_GPU_BENCH")) voodoo_set_gpu(1);   /* benchmark: GPU without a window */
    if (!headless && !file_exists(nvsave)) {
        run_first_time_calibration(g_exe, work, nvram, nvsave);
        if (g_enhanced) run_enhanced_setup(g_exe, work, nvram, nvsave);
    }
    static char cfbuf[1024], kbuf[1024];
    if (!cf) { snprintf(cfbuf, sizeof cfbuf, "%s/cf.img", work); cf = cfbuf; }
    snprintf(kbuf, sizeof kbuf, "%s/kernel.bin", work);

    g_ram = (uint8_t *)calloc(1, RAM_SIZE);
    FILE *f = fopen(kbuf, "rb");
    if (!f) { fprintf(stderr, "cannot open %s (run 'make extract GAME=" GAME_ID "' first)\n", kbuf); return 1; }
    size_t n = fread(g_ram, 1, RAM_SIZE, f);
    fclose(f);
    g_kernel_img = (uint8_t *)malloc(n);
    memcpy(g_kernel_img, g_ram, n);
    g_kernel_len = n;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
#ifndef _WIN32
    /* a write to a TCP connection the other side closed (UPnP on the router, link play) must
     * be an error the caller handles, not a silent end of the process */
    signal(SIGPIPE, SIG_IGN);
#endif
    if (wav) wav_open(wav);
    for (size_t i = 0; i < sizeof k_modules / sizeof k_modules[0]; i++) rt_register_module(k_modules[i]);
    rt_log("kernel: %zu bytes at 0x00000000\n", n);

    extern void rt_bp_init(void);
    rt_bp_init();
    HwConfig cfg = { cf, nvram, ds, bios, (headless && !nvsave_explicit) ? NULL : nvsave };
    hw_init(&cfg);
    inev_setup();
    if (g_run_seconds > 0) rt_sched_at((uint64_t)(g_run_seconds * CPU_HZ), stop_event, NULL);

    /* CPU state as BIOS 941B01 stage 2 leaves it (verified against MAME at kernel entry 0x10):
     * every GPR/SPRG/LR/CR = 0xdeadbeef, FPRs = 0x7ff5beef4afc0721, CTR = entry, MSR = 0x2070,
     * r31 = boot parameter word (IN2 << 24 | 0xffffff, see hw_boot_param; the kernel saves it at
     * 0xfc and the game derives the boot-time switch state from it, e.g. "TEST held -> initialise
     * RTC", and in gticlub2ea the DIP SW:3 that unlocks the game). */
    PPCContext *c = &g_ctx;
    for (int i = 0; i < 32; i++) c->r[i] = 0xdeadbeefu;
    for (int i = 0; i < 32; i++) c->f[i] = BITS_FPR(0x7ff5beef4afc0721ull);
    for (int i = 0; i < 4; i++) c->sprg[i] = 0xdeadbeefu;
    c->r[31] = hw_boot_param();
    c->lr = 0xdeadbeefu;
    rt_cr_unpack(c, 0xdeadbeefu, 0xff);
    c->ctr = 0x10;
    c->msr = 0x2070;
    rt_check(c, 0x10);          /* arms the first time slice */
    rt_start(0x10);
    if (headless && voodoo_gpu_active() && getenv("RT_GPU_BENCH")) {
        uint32_t vram_size = 0;
        voodoo_vram(&vram_size);
        gpu_gl_run_headless(vram_size);
    }
    if (headless) dump_frames_loop();   /* guest runs on fibers; rt_fatal() exits the process */
    if (frontend_run(scale, scale_explicit) == 2) {      /* enhanced mode: new game settings, reboot the game */
        hw_shutdown();
        fflush(NULL);
        setenv("RT_RESTARTED", "1", 1);  /* the frontend raises the new window */
        execv(g_exe, argv);
        fprintf(stderr, "restart failed: %s\n", g_exe);
    }
    rt_fatal("window closed");
}
