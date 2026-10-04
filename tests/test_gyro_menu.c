#include "fixtures/track_explorer_stubs.h"
#include "../runtime/enhanced.c"
#include <assert.h>
static double stick_position=.5, steering_position=.25;
double frontend_stick_position(void) { return stick_position; }
double frontend_steering_position(void) { return steering_position; }
static int enabled, sensitivity=100, recentered;
uint8_t *g_ram;
int frontend_gyro_enabled(void) {return enabled;}
int frontend_gyro_available(void) {return 1;}
int frontend_gyro_ready(void) {return 1;}
double frontend_gyro_position(void) {return .5;}
int frontend_gyro_sensitivity(void) {return sensitivity;}
void frontend_gyro_set_enabled(int x) {enabled=x;}
void frontend_gyro_set_sensitivity(int x) {sensitivity=x<50?50:x>350?350:x;}
void frontend_gyro_recenter(void) {recentered++;}
void voodoo_set_scale(int n) {(void)n;}
void voodoo_set_wide(int n) {(void)n;}
void rt_log(const char *s,...) {(void)s;}
uint8_t *hw_nvram(void) { static uint8_t nv[8192]; return nv; }
uint64_t rt_now(void) {return 0;}
int main(int argc, char **argv) {
 assert(argc == 2);
 g_enhanced=1; g_font=(uint8_t*)1; g_booted=1;
 snprintf(g_settings_path,sizeof g_settings_path,"%s", argv[1]);
 assert(!g_set.show_gyro);
 assert(enh_escape()); assert(g_paused);
 enh_menu_action(ENH_DOWN); enh_menu_action(ENH_OK); assert(g_pause_controls);
 enh_menu_action(ENH_RIGHT);assert(!enabled);
 enh_menu_action(ENH_DOWN);enh_menu_action(ENH_OK); assert(enabled);
 enh_menu_action(ENH_RIGHT); assert(sensitivity==110);
 enh_menu_action(ENH_LEFT); assert(sensitivity==100);
 enh_menu_action(ENH_DOWN); enh_menu_action(ENH_RIGHT); assert(g_set.show_gyro);
 enh_menu_action(ENH_DOWN); enh_menu_action(ENH_OK); assert(recentered==1);
 enh_menu_action(ENH_DOWN); enh_menu_action(ENH_OK); assert(!g_pause_controls&&g_paused);
 enh_menu_action(ENH_BACK); assert(!g_paused);
 enabled=0;sensitivity=50;g_set.show_gyro=0;settings_load();assert(enabled&&sensitivity==100&&g_set.show_gyro);
 remove(g_settings_path);
 g_frame=g_attract_frame=100;g_screen=SCREEN_PAGE;g_page=PAGE_CONTROLS;g_page_cursor=1;
 enh_menu_action(ENH_RIGHT);assert(enabled&&sensitivity==110);
 for(int i=0;i<8;i++) enh_menu_action(ENH_LEFT);
 assert(!enabled&&sensitivity==50);
 enh_menu_action(ENH_LEFT); assert(!enabled);
 enabled=1;sensitivity=100;settings_load();assert(!enabled&&sensitivity==50);
 enh_menu_action(ENH_RIGHT);assert(enabled&&sensitivity==50);
 for(int i=0;i<40;i++) enh_menu_action(ENH_RIGHT);
 assert(enabled&&sensitivity==350);
 enh_menu_action(ENH_DOWN);enh_menu_action(ENH_DOWN);
 enh_menu_action(ENH_LEFT);assert(recentered==1);
 enh_menu_action(ENH_OK);assert(recentered==2);
 remove(g_settings_path);
 /* The shared gameplay meter shows raw stick and output even with gyro off. */
 g_attract_frame=0; g_frame=500; enabled=1;
 uint32_t frame[512*384]={0};
 enh_draw_overlay(frame,512,384);
 assert(frame[364*512+271]==0xffffd800u);
 assert(frame[358*512+286]==0xff40dfffu);
 assert(frame[100*512+256]==0);
 g_set.show_gyro=0; memset(frame,0,sizeof frame); enh_draw_overlay(frame,512,384);
 assert(frame[364*512+271]==0 && frame[358*512+286]==0);
 g_set.show_gyro=1; enabled=0; enh_draw_overlay(frame,512,384);
 assert(frame[364*512+271]==0xffffd800u);
 stick_position=-.5; steering_position=-.25;
 memset(frame,0,sizeof frame); enh_draw_overlay(frame,512,384);
 assert(frame[364*512+241]==0xffffd800u && frame[358*512+226]==0xff40dfffu);
 puts("gyro menu navigation and settings persistence: passed");
}

void voodoo_set_texture_filter(int n) { (void)n; }
