/* Optional GTI Club 2 JAB exploration cameras. No persisted settings or ROM modifications. */
#pragma once
#include "ppc_rt.h"
void explorer_camera(PPCContext *c, uint64_t frame);
void explorer_race(PPCContext *c);
void explorer_toggle(void);
void explorer_free_toggle(void);
int explorer_free(void);
void explorer_drive(float forward, float vertical);
int explorer_active(void);
float explorer_speed(void);  /* metres per second */
float explorer_height(void); /* tour clearance / free-roam world altitude, metres */
void explorer_adjust(float speed, float height);
void explorer_look(float steering); /* tour offset / free-roam turn rate, -1..1 */
void explorer_pitch(float tilt); /* tour offset / free-roam pitch rate, -1..1 */
void explorer_on_frame(uint64_t frame);
