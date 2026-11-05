/* EXTRA modes (runtime/modes.c) and the overlay primitives of enhanced.c they draw with */
#pragma once
#include <stdint.h>

enum { MODE_NONE, MODE_GRAVITY, MODE_SPEED, MODE_BOMB, MODE_POLICE, MODE_ELIMINATION, MODE_COUNT };
int modes_list(const int **ids);                       /* the modes this game offers, menu order */
const char *modes_text(int mode, int line, int lang);  /* line 0 the name, 1-2 the description */
void modes_select(int mode);                           /* the mode of the next game (0: none) */
void modes_tick(int attract);                          /* guest thread, every frame */
void modes_draw(uint32_t *fb, int w, int h);           /* frontend, over the game (logical units) */
int modes_active(void);
struct PPCContext;
void modes_car_marker(struct PPCContext *c);           /* hook: the marker over a car is drawn */
void modes_car_select(struct PPCContext *c);           /* hook: the car select screen, every frame */
void modes_car_draw(struct PPCContext *c);             /* hook: a car is about to be drawn */
void modes_car_effect(struct PPCContext *c);           /* hook: a car's smoke/fire effect flag is read */
extern volatile int g_mode_hold;                       /* hw.c: the car is held (pedals) */
extern volatile int g_mode_steer_hold;                 /* hw.c: the steering is held at the centre */

/* enhanced.c: the game font on the overlay, in logical units (x < 0: centred) */
enum { ENH_FONT_LARGE, ENH_FONT_MEDIUM, ENH_FONT_SMALL };
int enh_lang(void);                                    /* the menus' language: 0 English, 1 Italian */
void enh_hud_text(uint32_t *fb, int w, int h, int size, int x, int y, const char *s, uint32_t rgb);
int enh_hud_text_width(int size, const char *s);
int enh_hud_font_height(int size);
void enh_hud_dim(uint32_t *fb, int w, int h, int x0, int y0, int x1, int y1, int a, uint32_t rgb);
