/* Real frontend sampling with deterministic sensor data; no controller or ROMs needed. */
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
static float sensor_accel[3] = {0, 9.81f, 0}, sensor_gyro[3];
static int sensor_error;
static SDL_bool has_sensor(SDL_GameController *p, SDL_SensorType s) { (void)p; (void)s; return SDL_TRUE; }
static int enable_sensor(SDL_GameController *p, SDL_SensorType s, SDL_bool e) { (void)p; (void)s; (void)e; return 0; }
static int read_sensor(SDL_GameController *p, SDL_SensorType s, float *v, int n) {
    (void)p; for(int i=0;i<n;i++) v[i]=(s==SDL_SENSOR_ACCEL ? sensor_accel : sensor_gyro)[i]; return sensor_error;
}
static SDL_bool attached(SDL_GameController *p) { return p ? SDL_TRUE : SDL_FALSE; }
static SDL_Joystick *joystick(SDL_GameController *p) { return (SDL_Joystick*)p; }
static SDL_JoystickID instance(SDL_Joystick *p) { (void)p; return 1; }
static Uint8 button(SDL_GameController *p, SDL_GameControllerButton b) { (void)p; (void)b; return 0; }
#define SDL_GameControllerHasSensor has_sensor
#define SDL_GameControllerSetSensorEnabled enable_sensor
#define SDL_GameControllerGetSensorData read_sensor
#define SDL_GameControllerGetAttached attached
#define SDL_GameControllerGetJoystick joystick
#define SDL_JoystickInstanceID instance
#define SDL_GameControllerGetButton button
#include "../runtime/frontend_sdl.c"
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
void enh_menu_action(int a) { (void)a; }
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


int main(void) {
    g_pad=(SDL_GameController*)1;
    frontend_gyro_set_enabled(1); paused=1;
    assert(gyro_steering(.016)==0 && frontend_gyro_ready());
    double angle=20*3.141592653589793/180;
    sensor_accel[0]=(float)(-9.81*sin(angle)); sensor_accel[1]=(float)(9.81*cos(angle));
    for(int i=0;i<200;i++) assert(gyro_steering(.016)==0);
    assert(frontend_gyro_position()>.5 && frontend_gyro_position()<.6);
    frontend_gyro_set_sensitivity(200);
    assert(gyro_steering(.016)==0 && frontend_gyro_position()==1);
    frontend_gyro_recenter(); assert(frontend_gyro_position()==0);
    assert(gyro_steering(.016)==0 && frontend_gyro_position()==0);
    paused=0; assert(gyro_steering(.016)==0); /* resume recenters */
    sensor_error=-1; assert(gyro_steering(.016)==0 && !frontend_gyro_ready());
    sensor_error=0; frontend_gyro_set_enabled(0);
    assert(gyro_steering(.016)==0 && !frontend_gyro_ready() && frontend_gyro_position()==0);
    g_pad=NULL;
    puts("live paused gyro preview, sensitivity, recenter and suppression: passed");
}

int enh_want_window(int *r) { (void)r; return 0; }
void enh_set_window(const int *r) { (void)r; }
int enh_texture_filter(void) { return 0; }
int enh_wheel_select_active(void) { return 0; }
void enh_wheel_select_step(int dir) { (void)dir; }
double enh_wheel_select_pos(void) { return 0; }
