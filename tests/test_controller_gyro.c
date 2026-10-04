#include "controller_gyro.h"
#include <assert.h>
#include <stdio.h>
static const double radians = 3.141592653589793 / 180;
int main(void) {
    ControllerGyro state = {0};
    float accel[3] = {0,9.81f,0}, gyro[3] = {0};
    double range = 35*radians;
    assert(controller_gyro_step(&state,accel,gyro,.01,range,0) == 0);
    /* Rotate right at 35 degrees/s for one second, including matching gravity. */
    gyro[2] = (float)(-35*radians);
    double previous = 0, output = 0;
    for (int i=1; i<=100; i++) {
        double angle = i*.01*35*radians;
        accel[0] = (float)(-9.81*sin(angle)); accel[1] = (float)(9.81*cos(angle));
        output = controller_gyro_step(&state,accel,gyro,.01,range,0);
        assert(output >= previous - .00001 && output <= 1);
        previous = output;
    }
    assert(output > .99);
    gyro[2] = 0;
    assert(controller_gyro_step(&state,accel,gyro,.01,range,1) == 0);
    /* Bias remains bounded by the accelerometer correction. */
    state = (ControllerGyro){0}; accel[0]=0; accel[1]=9.81f; gyro[2]=.01f;
    for (int i=0; i<10000; i++) output=controller_gyro_step(&state,accel,gyro,.01,range,0);
    assert(output == 0);
    gyro[2] = NAN; assert(controller_gyro_step(&state,accel,gyro,.01,range,0) == 0);
    gyro[2] = 0;
    assert(controller_gyro_step(&state,accel,gyro,2,range,0) == 0 && !state.ready);
    accel[1]=0; accel[2]=9.81f;
    assert(controller_gyro_step(&state,accel,gyro,.01,range,0) == 0 && !state.ready);
    /* Crossing the atan2 wrap should not produce a full-lock discontinuity. */
    state = (ControllerGyro){.angle=179*radians,.centre=179*radians,.ready=1};
    accel[0]=(float)(-9.81*sin(-179*radians)); accel[1]=(float)(9.81*cos(-179*radians)); accel[2]=0;
    assert(fabs(controller_gyro_step(&state,accel,gyro,.01,range,0)) < .01);
    /* Camera pitch is independent of roll, and can be recentered. */
    ControllerGyro pitch = {0};
    float upright[3] = {0, 9.81f, 0}, still[3] = {0};
    assert(controller_gyro_pitch_step(&pitch, upright, still, .01, 0) == 0);
    float tilted[3] = {0, (float)(9.81*cos(20*radians)), (float)(-9.81*sin(20*radians))};
    double pitch_output = 0;
    for (int i=0; i<300; i++) pitch_output=controller_gyro_pitch_step(&pitch, tilted, still, .01, 0);
    assert(pitch_output > .4 && pitch_output < .43);
    assert(controller_gyro_pitch_step(&pitch, tilted, still, .01, 1) == 0);
    puts("gyro tilt, camera pitch, recenter, drift, stale and invalid samples: passed");
}
