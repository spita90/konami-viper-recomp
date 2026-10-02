/* Pure controller transfer functions; raw axes use SDL's signed 16-bit range. */
#pragma once
#include <math.h>

static inline double controller_axis(int raw, double deadzone, double curve) {
    double x = raw < 0 ? raw / 32768.0 : raw / 32767.0;
    double magnitude = fabs(x);
    if (magnitude <= deadzone) return 0;
    magnitude = fmin(1.0, (magnitude - deadzone) / (1.0 - deadzone));
    return copysign(pow(magnitude, curve), x);
}

static inline double controller_trigger(int raw, double deadzone) {
    return fmax(0.0, controller_axis(raw, deadzone, 1.0));
}
