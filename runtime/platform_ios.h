/* iOS services the runtime uses (runtime/platform_ios.m); host main thread unless noted. */
#pragma once

const char *ios_documents_dir(void);                 /* the app's Documents: saves and settings */
void ios_safe_area(float *left, float *top, float *right, float *bottom);   /* insets, points */
/* the display's refresh, from a display link on the main run loop: ios_vsync_wait() runs that run
 * loop (UIKit's touches, timers, animations) until the next one (or 50 ms), so the frame loop draws
 * once per refresh */
void ios_vsync_start(int fps);
void ios_vsync_set_rate(int fps);
void ios_vsync_wait(void);
int ios_hide_launch_screen(void);                    /* SDL's launch screen, if still up: 1 */
int ios_app_state(void);                             /* 0 active, 1 inactive, 2 background */
/* tilt steering (CoreMotion): the device's rotation in the screen's plane, radians, positive when
 * turned clockwise (to the right) as a steering wheel; 0 if unavailable */
int ios_tilt_available(void);
void ios_tilt_enable(int on);
int ios_tilt(double *angle);
void ios_haptic_tap(void);                           /* a light tap, for a touch button */
