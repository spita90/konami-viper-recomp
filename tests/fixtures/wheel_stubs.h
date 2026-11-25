/* runtime/wheel.c for the tests that include one runtime file alone: no device, nothing bound.
 * A test sets g_test_value[] to hold an action as a bound key or pedal would. */
#pragma once
#include "wheel.h"
static double g_test_value[ACT_COUNT];
void wheel_init(const char *settings_path) { (void)settings_path; }
void wheel_start(void *sdl_window) { (void)sdl_window; }
void wheel_shutdown(void) {}
void wheel_devices_changed(void) {}
void wheel_poll(int focus, int keys) { (void)focus; (void)keys; }
double wheel_value(int action) { return g_test_value[action]; }
int wheel_down(int action) { return g_test_value[action] > 0.5; }
int wheel_pressed(int action) { (void)action; return 0; }
int wheel_steer(double *v) { *v = 0; return 0; }
int wheel_claims(unsigned vid, unsigned pid) { (void)vid; (void)pid; return 0; }
unsigned wheel_generation(void) { return 0; }
void wheel_ffb_update(uint8_t motor, int active) { (void)motor; (void)active; }
void wheel_ffb_test(void) {}
int wheel_ffb_detect(void) { return 0; }
const char *wheel_ffb_status(void) { return "OFF"; }
int wheel_ffb_strength(void) { return 0; }
void wheel_set_ffb_strength(int percent) { (void)percent; }
int wheel_ffb_invert(void) { return 0; }
void wheel_set_ffb_invert(int on) { (void)on; }
void wheel_ffb_device_step(int dir) { (void)dir; }
const char *wheel_ffb_device_name(void) { return NULL; }
int wheel_range(void) { return 100; }
void wheel_set_range(int percent) { (void)percent; }
int wheel_deadzone(void) { return 0; }
void wheel_set_deadzone(int percent) { (void)percent; }
const char *wheel_action_name(int action, int lang) { (void)action; (void)lang; return ""; }
int wheel_bind_count(int action) { (void)action; return 0; }
void wheel_bind_text(int action, int i, char *buf, size_t n) { (void)action; (void)i; if (n) buf[0] = 0; }
void wheel_bind_remove_last(int action) { (void)action; }
void wheel_bind_defaults(void) {}
void wheel_capture_begin(int action) { (void)action; }
int wheel_capture_active(void) { return -1; }
void wheel_capture_key(int scancode) { (void)scancode; }
int wheel_device_count(void) { return 0; }
void wheel_device_text(int i, char *buf, size_t n) { (void)i; if (n) buf[0] = 0; }
