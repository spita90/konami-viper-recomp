/* The phone and tablet apps' services (VIPER_MOBILE): runtime/platform_ios.m on iOS,
 * runtime/platform_android.c on Android. Host main thread unless noted. */
#pragma once

const char *mobile_data_dir(void);       /* writable: the saves and the settings (iOS: Documents) */
void mobile_safe_area(float *left, float *top, float *right, float *bottom);   /* insets, in window units */
/* tilt steering: the device's rotation in the screen's plane, radians, positive when turned
 * clockwise (to the right) as a steering wheel; 0 if unavailable */
int mobile_tilt_available(void);
void mobile_tilt_enable(int on);
int mobile_tilt(double *angle);
void mobile_haptic_tap(void);             /* a light tap, for a touch button */
#ifdef VIPER_ANDROID
/* the game's files (kernel, CF image, NVRAM, BIOS...), copied out of the APK's assets at the first
 * launch of this build: the runtime reads them with fopen, as beside the executable elsewhere */
const char *mobile_game_dir(void);
#endif
