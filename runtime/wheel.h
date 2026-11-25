/* Steering wheels, pedals, shifters and button boxes: every USB game device, free bindings (several
 * per action) and force feedback from the cabinet's steering motor. See wheel.c. Host main thread. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* the port's actions: what the games read (steering, pedals, handbrake, shifter, START, COIN, TEST,
 * SERVICE) and the port's own (pause, the enhanced mode's menus) */
enum { ACT_STEER, ACT_STEER_LEFT, ACT_STEER_RIGHT, ACT_GAS, ACT_BRAKE, ACT_HANDBRAKE, ACT_SHIFT_UP, ACT_SHIFT_DOWN,
       ACT_START, ACT_COIN, ACT_TEST, ACT_SERVICE, ACT_PAUSE, ACT_MENU_UP, ACT_MENU_DOWN, ACT_MENU_LEFT,
       ACT_MENU_RIGHT, ACT_MENU_OK, ACT_MENU_BACK, ACT_COUNT };
#define WHEEL_MAX_BINDS 4           /* bindings per action */

void wheel_init(const char *settings_path);     /* the bindings' file, next to the port settings */
void wheel_start(void *sdl_window);             /* the devices connected (needs the window) */
void wheel_shutdown(void);
void wheel_devices_changed(void);               /* a device came or went: looked up again shortly */

/* once per frontend loop. keys: 0 the keyboard drives the game, 1 the name entry has the text keys,
 * 2 a menu has the keyboard (a key held from there stays released for the game until it is let go) */
void wheel_poll(int focus, int keys);
double wheel_value(int action);                 /* 0..1, the strongest binding */
int wheel_down(int action);
int wheel_pressed(int action);                  /* went down at this poll */
int wheel_steer(double *v);                     /* 1: a steering axis is bound and connected, *v -1..1 */
int wheel_claims(unsigned vid, unsigned pid);   /* a binding uses this device: not also a gamepad */
unsigned wheel_generation(void);                /* changes with the bindings and the devices */

/* force feedback: the motor command (hw.c: bit 7 on, bit 4 right, bits 3-0 torque), each loop */
void wheel_ffb_update(uint8_t motor, int active);
void wheel_ffb_test(void);                      /* a short left-right shake */
int wheel_ffb_detect(void);                     /* finds the motor's direction: 1 done, 0 the wheel did not move */
const char *wheel_ffb_status(void);

/* settings (saved at once) */
int wheel_ffb_strength(void);                   /* percent, 0 = off, 100 = the motor's torque as it is */
void wheel_set_ffb_strength(int percent);
int wheel_ffb_invert(void);
void wheel_set_ffb_invert(int on);
void wheel_ffb_device_step(int dir);            /* AUTO, then each device */
const char *wheel_ffb_device_name(void);        /* NULL: AUTO */
int wheel_range(void);                          /* percent of the axis that is full lock */
void wheel_set_range(int percent);
int wheel_deadzone(void);                       /* percent around the centre */
void wheel_set_deadzone(int percent);

/* the bindings, for the menu */
const char *wheel_action_name(int action, int lang);
int wheel_bind_count(int action);
void wheel_bind_text(int action, int i, char *buf, size_t n);    /* "KEY UP", "J1 AXIS X", in the menu font's characters */
void wheel_bind_remove_last(int action);
void wheel_bind_defaults(void);
void wheel_capture_begin(int action);           /* the next key, button or axis moved is bound to it */
int wheel_capture_active(void);                 /* the action waiting, or -1 */
void wheel_capture_key(int scancode);           /* a key while capturing (Esc cancels) */
int wheel_device_count(void);
void wheel_device_text(int i, char *buf, size_t n);              /* "J1 FANATEC WHEEL [A8 B108] FFB" */
