/* cc -std=c11 -Iruntime tests/test_track_explorer.c -lm -o /tmp/test_track_explorer */
#include "../runtime/track_explorer.c"
#include <assert.h>

uint8_t *g_ram;
static uint64_t cycles, frame;
uint64_t rt_now(void) { return cycles; }
void rt_log(const char *fmt, ...) { (void)fmt; }
RtFn rt_lookup(uint32_t addr) { (void)addr; assert(!"free roam queried terrain"); return NULL; }
uint32_t rt_mmio_r32(uint32_t a) { (void)a; assert(0); return 0; }
uint32_t rt_mmio_r16(uint32_t a) { (void)a; assert(0); return 0; }
void rt_mmio_w32(uint32_t a, uint32_t v) { (void)a; (void)v; assert(0); }
void rt_mmio_w16(uint32_t a, uint32_t v) { (void)a; (void)v; assert(0); }
static void step(PPCContext *c) {
    cycles += (uint64_t)(CPU_HZ / 20);
    explorer_camera(c, ++frame);
}
static void near(float a, float b) { assert(fabsf(a-b) < .001f); }

int main(void) {
    g_ram = calloc(1, RAM_SIZE);
    assert(g_ram);
    PPCContext c = {0};
    c.r[2] = 0x1000;
    ST32(0x100c, 0x2000); ST32(0x1558, 0x3000);
    STF32(0x8c1cf8, 100); STF32(0x8c1cfc, 20); STF32(0x8c1d00, 200);
    /* No route exists: free roam must still start at the current camera. */
    explorer_free_toggle(); step(&c);
    assert(explorer_active() && explorer_free());
    near(LDF32(0x8c1cf8), 100); near(LDF32(0x8c1d00), 200);
    step(&c); near(LDF32(0x8c1d00), 200);
    explorer_drive(1, 0); step(&c); near(LDF32(0x8c1d00), 198);
    explorer_drive(-1, 0); step(&c); near(LDF32(0x8c1d00), 200);
    explorer_drive(0, 1); step(&c); near(explorer_height(), 20.6f);
    explorer_drive(0, -1); step(&c); near(explorer_height(), 20);
    explorer_drive(0, 0); explorer_look(1); explorer_pitch(1); step(&c);
    assert(LDF32(0x8c1d08) < 0 && LDF32(0x8c1d04) > 0);
    float yaw = LDF32(0x8c1d08), tilt = LDF32(0x8c1d04);
    explorer_look(0); explorer_pitch(0); step(&c);
    near(LDF32(0x8c1d08), yaw); near(LDF32(0x8c1d04), tilt);
    explorer_drive(1, 0); step(&c);
    assert(LDF32(0x8c1cf8) > 100 && LDF32(0x8c1cfc) > 20);
    explorer_drive(0, 0);
    explorer_race(&c); assert(c.r[4] == 8);
    explorer_free_toggle(); step(&c); assert(!explorer_active());
    /* Supply a minimal closed course to exercise both live mode transitions. */
    ST32(0x8c0188, 0x40000);
    ST16(0x40000, 3); ST32(0x40004, 0x40100); ST32(0x4000c, 0x40000);
    STF32(0x40120, 100); STF32(0x40140, 100); STF32(0x40144, 100);
    explorer_toggle(); step(&c); assert(explorer_active() && !explorer_free());
    explorer_free_toggle(); step(&c); assert(explorer_free() && explorer_active());
    explorer_toggle(); step(&c); assert(explorer_active() && !explorer_free());
    explorer_toggle(); step(&c); assert(!explorer_active());
    explorer_free_toggle(); step(&c);
    explorer_on_frame(frame + 5); assert(!explorer_active() && !explorer_free());
    free(g_ram);
    puts("free roam movement, heading, independent height, toggles and cancellation: passed");
}
