/* Exercise the actual frontend input path with SDL virtual controllers, no ROMs/window. */
#include "../runtime/frontend_sdl.c"
#include <assert.h>
#include <stdio.h>
uint8_t g_in[8];
int16_t g_analog[4];
int g_enhanced;
static int menu_active, paused;
uint64_t rt_now(void) { return 0; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void nvram_save(void) {}
int enh_turbo(void) { return 0; }
int enh_paused(void) { return paused; }
int enh_start_held(void) { return 0; }
int enh_inputs_owned(void) { return 0; }
int enh_menu_active(void) { return menu_active; }
int enh_name_entry_active(void) { return 0; }
static int menu_calls, last_menu_action;
void enh_menu_action(int a) { menu_calls++; last_menu_action=a; }
int enh_name_type(int a) { return a; }
void enh_name_step(int a) { (void)a; }
int enh_escape(void) { return 0; }
int enh_want_fullscreen(void) { return 0; }
void enh_set_fullscreen(int a) { (void)a; }
void enh_controller_settings_changed(void) {}
int enh_quit_requested(void) { return 0; }
int enh_restart_requested(void) { return 0; }
void enh_draw_overlay(uint32_t *f, int w, int h) { (void)f; (void)w; (void)h; }
uint64_t voodoo_get_frame(uint32_t *f, int n, int *w, int *h) { (void)f; (void)n; *w=*h=0; return 0; }

static void axis(SDL_GameControllerAxis a, Sint16 value) {
    assert(SDL_JoystickSetVirtualAxis(SDL_GameControllerGetJoystick(g_pad), a, value) == 0);
    SDL_JoystickUpdate();
}

static void button(SDL_GameControllerButton b, int down) {
    assert(SDL_JoystickSetVirtualButton(SDL_GameControllerGetJoystick(g_pad), b, down) == 0);
    SDL_JoystickUpdate();
}

int main(void) {
    menu_active=1; g_input_focus=1;
    menu_pad_press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 1000);
    assert(menu_calls==1 && last_menu_action==ENH_RIGHT);
    menu_pad_repeat(1399);assert(menu_calls==1);
    menu_pad_repeat(1400);assert(menu_calls==2);
    menu_pad_repeat(1479);assert(menu_calls==2);
    menu_pad_repeat(1480);assert(menu_calls==3);
    g_input_focus=0;menu_pad_repeat(2000);assert(menu_calls==3 && g_menu_repeat_button==-1);
    g_input_focus=1;menu_pad_press(SDL_CONTROLLER_BUTTON_A,2100);
    menu_pad_repeat(3000);assert(menu_calls==4 && last_menu_action==ENH_OK);
    menu_pad_press(SDL_CONTROLLER_BUTTON_DPAD_LEFT,3100);
    menu_active=0;menu_pad_repeat(4000);assert(menu_calls==5 && g_menu_repeat_button==-1);
    menu_active=1;menu_pad_press(SDL_CONTROLLER_BUTTON_DPAD_LEFT,UINT32_MAX-200);
    menu_pad_repeat(198);assert(menu_calls==6);
    menu_pad_repeat(199);assert(menu_calls==7 && last_menu_action==ENH_LEFT);
    menu_active=0;g_menu_repeat_button=-1;

    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    /* only the virtual controller (vendor and product 0): open_pad() takes the first one it finds */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000");
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                         SDL_CONTROLLER_BUTTON_MAX, 0);
    assert(index >= 0);
    open_pad();
    assert(g_pad);
    g_steer_speed = 1e9;    /* the stick's mapping first, without the ramp */
    assert(pad_matches(g_pad_id));
    assert(!pad_matches(g_pad_id + 1));
    /* A press consumed by Pause/Resume must still count as held accelerator. */
    axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    button(SDL_CONTROLLER_BUTTON_A, 1); pad_button(SDL_CONTROLLER_BUTTON_A, 1);
    apply_inputs(.016); assert(g_analog[1]==200);
    paused=1;apply_inputs(.016);
    paused=0;apply_inputs(.016);assert(g_analog[1]==200);
    paused=1;
    button(SDL_CONTROLLER_BUTTON_A, 0);pad_button(SDL_CONTROLLER_BUTTON_A, 0);
    button(SDL_CONTROLLER_BUTTON_A, 1); /* menu handles this press, so no pad_button down */
    paused=0;apply_inputs(.016);assert(g_analog[1]==200);
    button(SDL_CONTROLLER_BUTTON_A, 0);apply_inputs(.016);assert(g_analog[1]==-200);
    button(SDL_CONTROLLER_BUTTON_X, 1);paused=1;apply_inputs(.016);
    paused=0;apply_inputs(.016);assert(g_analog[2]==200);
    button(SDL_CONTROLLER_BUTTON_X, 0);apply_inputs(.016);assert(g_analog[2]==-200);
    /* SDL's virtual joystick maps -32768..32767 to controller trigger 0..32767. */
    axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    apply_inputs(.016);
    assert(g_analog[0] == 0 && g_analog[1] == -200 && g_analog[2] == -200);
    axis(SDL_CONTROLLER_AXIS_LEFTX, -32768); apply_inputs(.016); assert(g_analog[0] == -200);
    axis(SDL_CONTROLLER_AXIS_LEFTX, 32767); apply_inputs(.016); assert(g_analog[0] == 200);
    /* Partial travel must survive the real input path as distinct ADC values. */
    axis(SDL_CONTROLLER_AXIS_LEFTX, 8192); apply_inputs(.016); int quarter = g_analog[0];
    axis(SDL_CONTROLLER_AXIS_LEFTX, 16384); apply_inputs(.016); int half = g_analog[0];
    axis(SDL_CONTROLLER_AXIS_LEFTX, 24576); apply_inputs(.016); int three_quarters = g_analog[0];
    assert(quarter > 0 && half > quarter && three_quarters > half && three_quarters < 200);
    axis(SDL_CONTROLLER_AXIS_LEFTX, -16384); apply_inputs(.016); assert(abs(g_analog[0] + half) <= 1);
    axis(SDL_CONTROLLER_AXIS_LEFTX, 3000); apply_inputs(.016); assert(g_analog[0] == 0);
    axis(SDL_CONTROLLER_AXIS_RIGHTY, -32768); apply_inputs(.016); assert(g_analog[1] == 200 && g_analog[2] == -200);
    axis(SDL_CONTROLLER_AXIS_RIGHTY, 32767); apply_inputs(.016); assert(g_analog[2] == 200 && g_analog[1] == -200);
    axis(SDL_CONTROLLER_AXIS_RIGHTY, 0);
    axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);
    key(SDLK_w, 1); apply_inputs(.016); assert(g_analog[1] == 200);
    button(SDL_CONTROLLER_BUTTON_A, 1); pad_button(SDL_CONTROLLER_BUTTON_A, 1);
    key(SDLK_w, 0); apply_inputs(.016); assert(g_analog[1] == 200);
    key(SDLK_w, 1); close_pad(); apply_inputs(.016); assert(g_analog[1] == 200);
    key(SDLK_w, 0); apply_inputs(.016); assert(g_analog[1] == -200);
    open_pad(); button(SDL_CONTROLLER_BUTTON_A, 0);axis(SDL_CONTROLLER_AXIS_RIGHTY, -32768);
    g_input_focus = 0; apply_inputs(.016); assert(g_analog[1] == -200);
    g_input_focus = 1; menu_active = 1; apply_inputs(.016); assert(g_analog[1] == -200);
    menu_active=0;
    frontend_set_stick_response(0); assert(frontend_stick_response()==0);
    axis(SDL_CONTROLLER_AXIS_LEFTX,16384); apply_inputs(.016); int linear=g_analog[0];
    frontend_set_stick_response(1); apply_inputs(.016); int soft=g_analog[0];
    frontend_set_stick_response(2); apply_inputs(.016); int extra_soft=g_analog[0];
    assert(linear>soft && soft>extra_soft && extra_soft>0);
    frontend_set_stick_response(99); assert(frontend_stick_response()==2);
    /* a stick flicked to full lock for one frame turns the wheel only a little, then it ramps */
    g_steer_speed = 4.0;
    axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
    for (int i = 0; i < 5; i++) apply_inputs(.05);
    assert(g_analog[0] == 0);
    axis(SDL_CONTROLLER_AXIS_LEFTX, -32768); apply_inputs(.016);
    assert(g_analog[0] < 0 && g_analog[0] >= -13);
    axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
    for (int i = 0; i < 5; i++) apply_inputs(.05);
    assert(g_analog[0] == 0);
    axis(SDL_CONTROLLER_AXIS_LEFTX, 32767);
    for (int i = 0; i < 5; i++) apply_inputs(.05);
    assert(g_analog[0] == 200);
    /* D-pad steering ramps as the keyboard, also from a press the pause consumed */
    axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
    for (int i = 0; i < 5; i++) apply_inputs(.05);
    paused=1; button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1); paused=0;
    apply_inputs(.05); assert(g_analog[0] < 0 && g_analog[0] > -200);
    for (int i = 0; i < 5; i++) apply_inputs(.05);
    assert(g_analog[0] == -200);
    button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0);
    for (int i = 0; i < 5; i++) apply_inputs(.05);
    assert(g_analog[0] == 0);
    button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 1); pad_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 1);
    for (int i = 0; i < 5; i++) apply_inputs(.05);
    assert(g_analog[0] == 200);
    button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0); pad_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0);
    for (int i = 0; i < 5; i++) apply_inputs(.05);
    assert(g_analog[0] == 0);
    close_pad();
    assert(SDL_JoystickDetachVirtual(index) == 0);
    SDL_Quit();
    puts("frontend virtual-controller tests: passed");
}

int enh_want_window(int *r) { (void)r; return 0; }
void enh_set_window(const int *r) { (void)r; }
int enh_texture_filter(void) { return 0; }
int enh_wheel_select_active(void) { return 0; }
void enh_wheel_select_step(int dir) { (void)dir; }
double enh_wheel_select_pos(void) { return 0; }
int enh_code_entry_active(void) { return 0; }
int enh_text_input_active(void) { return 0; }
void enh_code_key(int ch) { (void)ch; }
const char *enh_take_url(void) { return NULL; }
void enh_code_paste(const char *text) { (void)text; }
int enh_draw_overlay_layer(uint32_t *fb, int w, int h) { (void)fb; (void)w; (void)h; return 0; }
const uint8_t *voodoo_vram(uint32_t *size) { (void)size; return NULL; }
void voodoo_set_gpu(int on) { (void)on; }
int voodoo_gpu_active(void) { return 0; }
int gpu_gl_init(SDL_Window *win, uint32_t vram_size) { (void)win; (void)vram_size; return 0; }
void gpu_gl_frame(SDL_Window *win, const uint32_t *overlay, int ow, int oh, uint64_t show_at, int unfiltered) {
    (void)win; (void)overlay; (void)ow; (void)oh; (void)show_at; (void)unfiltered;
}
int enh_filtering(void) { return 0; }
void gpu_gl_shutdown(void) {}
int gpu_gl_max_texture(void) { return 0; }
void enh_set_display(int picture_h, int max_scale) { (void)picture_h; (void)max_scale; }
void enh_set_refresh(int hz) { (void)hz; }
void interp_set_display(uint64_t vsync, uint64_t period) { (void)vsync; (void)period; }
const char *gpu_gl_name(void) { return ""; }
