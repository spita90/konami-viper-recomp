/* On-screen touch controls (runtime/touch.c): the iOS app only (VIPER_MOBILE). */
#pragma once
#include <SDL.h>
#include <stdint.h>

/* a sprite of the touch controls: destination in drawable pixels, source in the atlas (0..1),
 * opacity (the atlas is premultiplied 0xAARRGGBB, as the enhanced-mode overlay) */
typedef struct { float x, y, w, h, u0, v0, u1, v1, a; } UiQuad;

typedef struct {
    int steer_on;               /* the steering stick is held */
    double steer;               /* its position, -1..1, before the response curve */
    int gas, brake, shift_up, shift_down, start, coin;
    int keyboard;               /* KEYS was tapped: show or hide the on-screen keyboard */
} TouchOut;

int touch_init(void);                                   /* 1 if the touch controls are on */
int touch_event(SDL_Window *win, const SDL_Event *ev);  /* 1: the event was theirs */
void touch_hide(void);                                  /* a gamepad is in use: hidden until touched */
void touch_set_picture(int w, int h);                   /* the picture's size (its aspect on screen) */
void touch_frame(SDL_Window *win);                      /* every loop: the current screen's layout */
void touch_output(TouchOut *out);
int touch_quads(SDL_Window *win, UiQuad *q, int max);
const uint32_t *touch_atlas(int *w, int *h, unsigned *version);
