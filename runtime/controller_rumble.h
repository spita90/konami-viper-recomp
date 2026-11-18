/* SDL rumble adapter for the cabinet's K-type steering motor. Host thread only. */
#pragma once
#include <SDL.h>
#include <math.h>
#include <stdint.h>

#ifndef CONTROLLER_RUMBLE_SEND
#define CONTROLLER_RUMBLE_SEND SDL_GameControllerRumble
#endif

typedef struct {
    SDL_JoystickID id;
    Uint16 strength;                /* the stronger of the two motors, 0: stopped */
    Uint16 low, high;
    Uint32 refreshed;
    int sent;
    int failed;
    Uint32 retry_at;
} ControllerRumble;

/* The motor command's torque (bits 3-0, bit 7 on) as a vibration level, 0..1. The games use a
 * narrow band of it in a race (GTI Club 2 mostly 5-7, Thrill Drive 2 7-9, little above 11), which
 * proportionally is 17-30% and feels constant, so 3..11 is stretched over 25-100%: each torque
 * step there is about 9%. 1 and 2 stay light. */
static inline double controller_motor_level(uint8_t motor) {
    int torque = motor & 15;
    if (!(motor & 0x80) || !torque) return 0;
    if (torque < 3) return 0.1 * torque;
    return fmin(1.0, 0.25 + 0.75 * (torque - 3) / 8.0);
}

/* The two motors of a pad: the heavy low-frequency one is on the left, the light high-frequency
 * one on the right. The side the wheel pulls to (bit 4: 1 = right) leads, the other follows at
 * CONTROLLER_RUMBLE_OTHER_SIDE. gain 0.5 is 1.0X: the level as it is. */
#define CONTROLLER_RUMBLE_OTHER_SIDE 0.35
static inline void controller_motor_rumble(uint8_t motor, double gain, Uint16 *low, Uint16 *high) {
    *low = *high = 0;
    if (!isfinite(gain) || gain <= 0) return;
    double level = fmin(1.0, controller_motor_level(motor) * fmin(gain, 2.0) * 2.0);
    Uint16 lead = (Uint16)lround(level * 65535.0);
    Uint16 other = (Uint16)lround(level * CONTROLLER_RUMBLE_OTHER_SIDE * 65535.0);
    if (motor & 0x10) { *high = lead; *low = other; }
    else { *low = lead; *high = other; }
}

/* the stronger motor */
static inline Uint16 controller_motor_strength(uint8_t motor, double gain) {
    Uint16 low, high;
    controller_motor_rumble(motor, gain, &low, &high);
    return low > high ? low : high;
}

static void controller_rumble_update(ControllerRumble *state, SDL_GameController *pad,
                                     uint8_t motor, double gain, int active, Uint32 now) {
    SDL_JoystickID id = pad ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) : -1;
    if (id != state->id) {
        *state = (ControllerRumble){ .id = id };
    }
    if (!pad) return;
    if (state->failed && (Sint32)(now - state->retry_at) < 0) return;
    Uint16 low = 0, high = 0;
    if (active) controller_motor_rumble(motor, gain, &low, &high);
    Uint16 strength = low > high ? low : high;
    /* Rate-limit changing strengths too: rapid Switch Bluetooth output can
     * disconnect the controller. Stops remain immediate; resume waits one interval. */
    if (!state->failed && !strength && !state->strength) return;
    if (strength && state->sent && (Uint32)(now - state->refreshed) < 50) return;
#if SDL_VERSION_ATLEAST(2, 0, 9)
    if (CONTROLLER_RUMBLE_SEND(pad, low, high, strength ? 100 : 0) < 0) {
        if (!state->failed) SDL_Log("Controller rumble failed; retrying: %s", SDL_GetError());
        state->failed = 1;
        state->retry_at = now + 1000; /* recover transient errors without per-frame retries */
        return;
    }
#endif
    state->sent = 1;
    state->failed = 0;
    state->strength = strength;
    state->low = low;
    state->high = high;
    state->refreshed = now;
}

/* Diagnostic pulse: one finite effect, without the race loop's refreshes. */
static int controller_rumble_pulse(ControllerRumble *state, SDL_GameController *pad,
                                    double gain, Uint32 now) {
    if (!pad) return -1;
    /* full torque, both motors alike */
    Uint16 strength = (Uint16)lround(fmin(1.0, isfinite(gain) && gain > 0 ? fmin(gain, 2.0) * 2.0 : 0) * 65535.0);
    if (CONTROLLER_RUMBLE_SEND(pad, strength, strength, strength ? 1000 : 0) < 0)
        return -1;
    *state = (ControllerRumble){
        .id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)),
        .strength = strength, .low = strength, .high = strength, .refreshed = now, .sent = 1
    };
    return 0;
}
