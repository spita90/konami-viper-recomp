/* Keep unrelated frontend/menu tests independent of guest camera memory. */
#pragma once
#include "../../runtime/track_explorer.h"
int explorer_active(void) { return 0; }
int explorer_free(void) { return 0; }
float explorer_speed(void) { return 0; }
float explorer_height(void) { return 0; }
void explorer_toggle(void) {}
void explorer_free_toggle(void) {}
void explorer_adjust(float s, float h) { (void)s; (void)h; }
void explorer_look(float v) { (void)v; }
void explorer_pitch(float v) { (void)v; }
void explorer_drive(float f, float v) { (void)f; (void)v; }
void explorer_on_frame(uint64_t f) { (void)f; }
void explorer_camera(PPCContext *c, uint64_t f) { (void)c; (void)f; }
void explorer_race(PPCContext *c) { (void)c; }
