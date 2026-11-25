/* The bindings of runtime/wheel.c without any hardware: the devices are made up here. Tested:
 * the names of devices that share a product name (a Fanatec base is two), finding a binding's
 * device again by its id, its name or its product, the capture of a key, a button, a hat and an
 * axis (a pedal resting at one end, the wheel), the values read from them, the key latch of the
 * menus, and the file written and read back. */
#include "../runtime/wheel.c"
#include <assert.h>
#include <stdarg.h>

void rt_log(const char *fmt, ...) { (void)fmt; }

static void device(const char *base, unsigned vid, unsigned pid, int axes, int buttons, int povs) {
    Dev *d = &g_dev[g_ndev++];
    memset(d, 0, sizeof *d);
    copy(d->base, sizeof d->base, base);
    d->vid = vid;
    d->pid = pid;
    for (int s = 0; s < axes; s++) d->present[s] = 1;
    d->naxes = axes;
    d->nbuttons = buttons;
    d->npovs = povs;
    d->ff_slot = -1;
    for (int p = 0; p < NPOV; p++) d->pov[p] = -1;
}

static void poll(int keys) {
    /* past the capture's end is far away, and no rescan is pending */
    wheel_poll(1, keys);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char settings[512], controls[512];
    snprintf(settings, sizeof settings, "%s/td2_enhanced_settings.ini", argv[1]);
    snprintf(controls, sizeof controls, "%s/td2_controls.ini", argv[1]);
    remove(controls);
    assert(SDL_Init(SDL_INIT_EVENTS) == 0);

    wheel_init(settings);
    assert(!strcmp(g_path, controls));
    assert(wheel_bind_count(ACT_GAS) == 2 && wheel_bind_count(ACT_STEER) == 0 && wheel_bind_count(ACT_MENU_OK) == 0);
    char text[160];
    wheel_bind_text(ACT_GAS, 0, text, sizeof text);
    assert(!strcmp(text, "KEY UP"));

    /* a base that is two devices of one name, pedals, a shifter */
    device("FANATEC Wheel", 0x0eb7, 0x0006, 8, 108, 1);
    device("FANATEC Wheel", 0x0eb7, 0x0006, 2, 3, 0);
    device("EndPedal", 0x2341, 0x8236, 3, 0, 0);
    device("FANATEC Wheel", 0x0eb7, 0x0006, 2, 3, 0);
    name_devices();
    resolve_all();
    assert(!strcmp(g_dev[0].name, "FANATEC Wheel [A8 B108]"));
    assert(!strcmp(g_dev[1].name, "FANATEC Wheel [A2 B3]"));
    assert(!strcmp(g_dev[3].name, "FANATEC Wheel [A2 B3] #2"));
    assert(!strcmp(g_dev[2].name, "EndPedal") && !strcmp(g_dev[2].id, "2341:8236 EndPedal"));
    assert(g_dev[0].siblings == 2 && g_dev[2].siblings == 0);

    /* the wheel: only an axis is taken, and the way it was turned is "right" */
    wheel_capture_begin(ACT_STEER);
    wheel_capture_key(SDL_SCANCODE_G);
    assert(wheel_capture_active() == ACT_STEER);
    g_dev[0].btn[4] = 1;
    poll(0);
    assert(wheel_capture_active() == ACT_STEER);
    g_dev[0].btn[4] = 0;
    g_dev[0].axis[0] = -0.6;
    poll(0);
    assert(wheel_capture_active() == -1 && wheel_bind_count(ACT_STEER) == 1);
    assert(g_bind[ACT_STEER][0].inverted && !g_bind[ACT_STEER][0].half && g_bind[ACT_STEER][0].di == 0);
    double v;
    poll(0);
    assert(wheel_steer(&v) && fabs(v - 0.6) < 1e-9);
    g_range = 50;                       /* half the travel is full lock */
    g_dev[0].axis[0] = 0.25;
    poll(0);
    assert(wheel_steer(&v) && fabs(v + 0.5) < 1e-9);
    g_dev[0].axis[0] = 0.9;
    poll(0);
    assert(wheel_steer(&v) && v == -1);
    g_range = 100;
    g_dev[0].axis[0] = 0;

    /* a pedal resting at +1, pressed towards -1 */
    g_dev[2].axis[1] = 1;
    wheel_capture_begin(ACT_GAS);
    g_dev[2].axis[1] = 0.4;
    poll(0);
    assert(wheel_capture_active() == -1 && wheel_bind_count(ACT_GAS) == 3);
    assert(g_bind[ACT_GAS][2].inverted && !g_bind[ACT_GAS][2].half);
    g_dev[2].axis[1] = 1;  poll(0); assert(wheel_value(ACT_GAS) == 0 && !wheel_down(ACT_GAS));
    g_dev[2].axis[1] = -1; poll(0); assert(wheel_value(ACT_GAS) == 1 && wheel_pressed(ACT_GAS));
    poll(0); assert(wheel_down(ACT_GAS) && !wheel_pressed(ACT_GAS));
    g_dev[2].axis[1] = 0;  poll(0); assert(fabs(wheel_value(ACT_GAS) - (0.5 - 0.02) / 0.98) < 1e-9);
    g_dev[2].axis[1] = 1;

    /* a button held when the capture starts is not the one bound; the next one is */
    g_dev[0].btn[7] = 1;
    poll(0);
    wheel_capture_begin(ACT_SHIFT_UP);
    poll(0);
    assert(wheel_capture_active() == ACT_SHIFT_UP);
    g_dev[0].btn[9] = 1;
    poll(0);
    assert(wheel_capture_active() == -1 && g_bind[ACT_SHIFT_UP][1].type == B_BUTTON && g_bind[ACT_SHIFT_UP][1].code == 9);
    poll(0);
    assert(!wheel_pressed(ACT_SHIFT_UP));       /* still held from the capture: not a press */
    g_dev[0].btn[9] = g_dev[0].btn[7] = 0; poll(0);
    g_dev[0].btn[9] = 1; poll(0);
    assert(wheel_pressed(ACT_SHIFT_UP) && wheel_down(ACT_SHIFT_UP));
    g_dev[0].btn[9] = 0;

    /* a hat direction, a key */
    wheel_capture_begin(ACT_MENU_LEFT);
    g_dev[0].pov[0] = 27000;
    poll(0);
    assert(wheel_capture_active() == -1 && g_bind[ACT_MENU_LEFT][0].type == B_HAT && g_bind[ACT_MENU_LEFT][0].code == 3);
    wheel_bind_text(ACT_MENU_LEFT, 0, text, sizeof text);
    assert(!strcmp(text, "J1 HAT LEFT"));
    g_dev[0].pov[0] = -1;
    wheel_capture_begin(ACT_HANDBRAKE);
    wheel_capture_key(SDL_SCANCODE_ESCAPE);     /* cancelled */
    assert(wheel_capture_active() == -1 && wheel_bind_count(ACT_HANDBRAKE) == 1);
    wheel_capture_begin(ACT_HANDBRAKE);
    wheel_capture_key(SDL_SCANCODE_H);
    assert(wheel_bind_count(ACT_HANDBRAKE) == 2);
    wheel_bind_text(ACT_HANDBRAKE, 1, text, sizeof text);
    assert(!strcmp(text, "KEY H"));
    /* the fifth binding takes the last one's place */
    for (int sc = SDL_SCANCODE_I; sc <= SDL_SCANCODE_L; sc++) { wheel_capture_begin(ACT_HANDBRAKE); wheel_capture_key(sc); }
    assert(wheel_bind_count(ACT_HANDBRAKE) == WHEEL_MAX_BINDS && g_bind[ACT_HANDBRAKE][3].code == SDL_SCANCODE_L);
    wheel_bind_remove_last(ACT_COIN);
    assert(wheel_bind_count(ACT_COIN) == 0);
    g_ffb_strength = 70;
    g_ffb_invert = 1;
    save();

    /* read back; the devices in another order, the base's shape changed (another rim) */
    Bind steer = g_bind[ACT_STEER][0];
    wheel_init(settings);
    assert(g_ffb_strength == 70 && g_ffb_invert == 1);
    assert(wheel_bind_count(ACT_COIN) == 0 && wheel_bind_count(ACT_GAS) == 3 && wheel_bind_count(ACT_HANDBRAKE) == 4);
    assert(wheel_bind_count(ACT_START) == 1 && g_bind[ACT_START][0].code == SDL_SCANCODE_1);
    assert(g_bind[ACT_STEER][0].type == B_AXIS && g_bind[ACT_STEER][0].inverted && !strcmp(g_bind[ACT_STEER][0].dev, steer.dev));
    assert(!strcmp(g_bind[ACT_STEER][0].dev, "0EB7:0006 FANATEC Wheel [A8 B108]"));
    g_ndev = 0;
    device("EndPedal", 0x2341, 0x8236, 3, 0, 0);
    device("FANATEC Wheel", 0x0eb7, 0x0006, 2, 3, 0);
    device("FANATEC Wheel", 0x0eb7, 0x0006, 8, 108, 1);
    name_devices();
    resolve_all();
    assert(g_bind[ACT_STEER][0].di == 2 && g_bind[ACT_GAS][2].di == 0 && g_bind[ACT_MENU_LEFT][0].di == 2);
    g_ndev = 0;
    device("FANATEC Wheel", 0x0eb7, 0x0006, 2, 3, 0);      /* this one has an x axis too, but not the buttons */
    device("FANATEC Wheel", 0x0eb7, 0x0006, 8, 60, 1);
    name_devices();
    resolve_all();
    assert(g_bind[ACT_SHIFT_UP][1].di == 1);    /* button 10: only the second has it */
    assert(g_bind[ACT_GAS][2].di == -1);        /* the pedals are unplugged */
    wheel_bind_text(ACT_GAS, 2, text, sizeof text);
    assert(!strcmp(text, "J- AXIS Y INV"));
    assert(wheel_claims(0x0eb7, 0x0006) && !wheel_claims(0x2341, 0x8236));

    wheel_bind_defaults();
    assert(wheel_bind_count(ACT_STEER) == 0 && wheel_bind_count(ACT_COIN) == 1);
    remove(controls);
    SDL_Quit();
    puts("wheel binding tests: passed");
    return 0;
}
