/* Roll steering: gyro response, with gravity correction to bound long-term drift. */
#pragma once
#include <math.h>

typedef struct { double angle, centre; int ready; } ControllerGyro;

static double controller_gyro_step(ControllerGyro *state, const float accel[3], const float gyro[3],
                                   double dt, double full_scale, int recenter) {
    if (!isfinite(dt) || !isfinite(full_scale) || full_scale <= 0) return 0;
    for (int i = 0; i < 3; i++)
        if (!isfinite(accel[i]) || !isfinite(gyro[i])) return 0;
    double gravity = sqrt(accel[0]*accel[0] + accel[1]*accel[1] + accel[2]*accel[2]);
    /* A controller held like a wheel has a useful X/Y gravity projection. When held
     * flat or accelerated strongly, ignore the unreliable gravity angle. */
    int gravity_valid = gravity > 7 && gravity < 12 && hypot(accel[0], accel[1]) > 3;
    double measured = atan2(-accel[0], accel[1]);
    if (!state->ready) {
        if (!gravity_valid) return 0;
        state->angle = state->centre = measured;
        state->ready = 1;
    } else if (dt > 0 && dt <= .1) {
        state->angle -= gyro[2] * dt;  /* SDL positive roll is counter-clockwise (left). */
        if (gravity_valid) {
            double correction = remainder(measured - state->angle, 2 * 3.141592653589793);
            state->angle += (1 - exp(-dt / .5)) * correction;
        }
    } else if (dt > .1) {
        /* Don't integrate across focus changes or a stalled frame. */
        state->ready = 0;
        return 0;
    }
    state->angle = remainder(state->angle, 2 * 3.141592653589793);
    if (recenter) state->centre = state->angle;
    double delta = remainder(state->angle - state->centre, 2 * 3.141592653589793);
    double deadzone = 2 * 3.141592653589793 / 180;
    double magnitude = fmax(0, fabs(delta) - deadzone) / fmax(full_scale - deadzone, deadzone);
    return copysign(fmin(magnitude, 1), delta);
}

/* Pitch relative to the held neutral pose. Project angular velocity onto the
 * horizontal right axis so rolling the controller does not become look-up/down. */
static double controller_gyro_pitch_step(ControllerGyro *state, const float accel[3],
                                         const float gyro[3], double dt, int recenter) {
    double horizontal = hypot(accel[0], accel[1]);
    float projected_accel[3] = {accel[2], (float)horizontal, 0};
    double rate = horizontal > 3 ? (gyro[0] * accel[1] - gyro[1] * accel[0]) / horizontal : gyro[0];
    float projected_gyro[3] = {0, 0, (float)-rate};
    return controller_gyro_step(state, projected_accel, projected_gyro, dt,
                                45 * 3.141592653589793 / 180, recenter);
}
