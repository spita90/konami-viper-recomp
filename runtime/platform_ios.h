/* iOS only services (runtime/platform_ios.m), beside the phones' common ones (platform_mobile.h). */
#pragma once

/* the display's refresh, from a display link on the main run loop: ios_vsync_wait() runs that run
 * loop (UIKit's touches, timers, animations) until the next one (or 50 ms), so the frame loop draws
 * once per refresh */
void ios_vsync_start(int fps);
void ios_vsync_set_rate(int fps);
void ios_vsync_wait(void);
