/* The saved window: fitting to the displays (window_state.h) and the port settings that keep it
 * (enhanced.c, one file per mode). No ROMs: game_config.h comes from a profile. */
#include "window_state.h"
#include "../runtime/enhanced.c"
#include <assert.h>

/* what enhanced.c calls outside these tests */
uint8_t *g_ram;
uint8_t g_in[8];
void rt_log(const char *fmt, ...) { (void)fmt; }
uint64_t rt_now(void) { return 0; }
uint8_t *hw_nvram(void) { static uint8_t nv[8192]; return nv; }
void hw_nvram_options_fix(uint8_t *nv) { (void)nv; }
void nvram_save(void) {}
void voodoo_set_scale(int n) { (void)n; }
void voodoo_set_wide(int n) { (void)n; }
unsigned long long voodoo_swap_count(void) { return 0; }

static char g_text[4096];
static const char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(g_text, 1, sizeof g_text - 1, f) : 0;
    if (f) fclose(f);
    g_text[n] = 0;
    return f ? g_text : NULL;
}
static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}
static void reload(void) { memset(g_set.win, 0, sizeof g_set.win); settings_load(); }

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *path = argv[1];
    int r[4];

    /* fitting to the displays connected now */
    SDL_Rect displays[] = { { 0, 25, 1440, 850 }, { -1280, 0, 1280, 800 } };
    SDL_Rect rect = { -1000, 100, 800, 600 };
    window_state_fit(&rect, displays, 2); assert(rect.x == -1000 && rect.y == 100);    /* second monitor */
    window_state_fit(&rect, displays, 1); assert(rect.x == 320 && rect.y == 150);      /* monitor gone */
    rect = (SDL_Rect){ 1400, 840, 1000, 700 }; window_state_fit(&rect, displays, 1);
    assert(rect.x == 440 && rect.y == 175);                                             /* pulled inside */
    rect = (SDL_Rect){ 0, 0, 3000, 2000 }; window_state_fit(&rect, displays, 1);
    assert(rect.w == 1440 && rect.h == 850 && rect.x == 0 && rect.y == 25);             /* shrunk */

    /* classic mode: no file, no window; a saved window is the only content of its file */
    remove(path);
    g_enhanced = 0;
    enh_init("", path);
    assert(!enh_want_window(r));
    enh_set_window((const int[4]){ -1000, 100, 800, 600 });
    const char *t = read_file(path);
    assert(t && !strstr(t, "enhanced") && !strstr(t, "fullscreen") && !strstr(t, "aspect"));
    assert(strstr(t, "window_x = -1000\n") && strstr(t, "window_height = 600\n"));
    reload();
    assert(enh_want_window(r) && r[0] == -1000 && r[1] == 100 && r[2] == 800 && r[3] == 600);
    remove(path);                                       /* the same window: nothing written */
    enh_set_window((const int[4]){ -1000, 100, 800, 600 });
    assert(!read_file(path));

    /* values out of range are not restored */
    write_file(path, "window_x = 0\nwindow_y = 0\nwindow_width = 100\nwindow_height = 600\n");
    reload(); assert(!enh_want_window(r));
    write_file(path, "window_x = 2000000\nwindow_y = 0\nwindow_width = 800\nwindow_height = 600\n");
    reload(); assert(!enh_want_window(r));
    write_file(path, "window_x = 0\nwindow_y = 0\nwindow_width = 800\nwindow_height = 20000\n");
    reload(); assert(!enh_want_window(r));

    /* enhanced mode: the display options are kept with the window */
    g_enhanced = 1;
    write_file(path, "fullscreen = 1\nshow_fps = 1\nrender_scale = 2\naspect = 2\n");
    settings_load();
    enh_set_window((const int[4]){ 50, 60, 1366, 768 });
    t = read_file(path);
    assert(t && strstr(t, "enhanced mode") && strstr(t, "fullscreen = 1\n") && strstr(t, "show_fps = 1\n"));
    assert(strstr(t, "render_scale = 2\n") && strstr(t, "aspect = 2\n") && strstr(t, "window_width = 1366\n"));
    g_set.fullscreen = g_set.aspect = 0;
    reload();
    assert(g_set.fullscreen == 1 && g_set.aspect == 2);
    assert(enh_want_window(r) && r[0] == 50 && r[1] == 60 && r[2] == 1366 && r[3] == 768);
    remove(path);
    puts("window fitting and port settings (classic, enhanced, out of range): passed");
}
