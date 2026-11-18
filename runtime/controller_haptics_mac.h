#pragma once
/* Host main thread only. 0 = unavailable, 1 = submitted, -1 = failed. */
void controller_haptics_init(void);
int controller_haptics_rumble(float strength, double seconds);
void controller_haptics_stop(void);
