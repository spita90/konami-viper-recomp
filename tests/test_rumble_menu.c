#include "../runtime/enhanced.c"
#include <assert.h>
static int multiplier=100;
uint8_t *g_ram;
int frontend_rumble_multiplier(void) {return multiplier;}
void frontend_set_rumble_multiplier(int v) {multiplier=v<0?0:v>400?400:v;}
int frontend_stick_response(void) { return 2; }
void frontend_set_stick_response(int v) { (void)v; }
void voodoo_set_scale(int n) {(void)n;}
void voodoo_set_wide(int n) {(void)n;}
void rt_log(const char *s,...) {(void)s;}
uint8_t *hw_nvram(void) {static uint8_t nv[8192];return nv;}
uint64_t rt_now(void) {return 0;}
int main(int argc,char **argv) {
 assert(argc==2); g_enhanced=1;g_font=(uint8_t*)1;g_booted=1;
 snprintf(g_settings_path,sizeof g_settings_path,"%s",argv[1]);
 assert(enh_escape()); enh_menu_action(ENH_DOWN);enh_menu_action(ENH_OK);
 assert(g_pause_controls);
 enh_menu_action(ENH_DOWN);enh_menu_action(ENH_RIGHT);assert(multiplier==150);
 for(int i=0;i<20;i++) enh_menu_action(ENH_RIGHT);
 assert(multiplier==400);
 multiplier=100;settings_load();assert(multiplier==400);
 for(int i=0;i<20;i++) enh_menu_action(ENH_LEFT);
 assert(multiplier==0);enh_menu_action(ENH_DOWN);enh_menu_action(ENH_OK);
 assert(!g_pause_controls&&g_paused);enh_menu_action(ENH_BACK);assert(!g_paused);
 g_frame=g_attract_frame=100;g_screen=SCREEN_PAGE;g_page=PAGE_CONTROLS;g_page_cursor=1;
 enh_menu_action(ENH_RIGHT);assert(multiplier==50);
 remove(g_settings_path);puts("rumble menu navigation, range and saved multiplier: passed");
}

void voodoo_set_texture_filter(int n) { (void)n; }
int modes_list(const int **ids) { *ids = NULL; return 0; }
void modes_select(int mode) { (void)mode; }
int net_active(void) { return 0; }
int net_game_halted(void) { return 0; }
int net_linked_count(void) { return 0; }
void net_request_host(void) {}
void net_request_start(void) {}
void net_request_stop(void) {}
int net_session_over(void) { return 1; }
void net_status(NetStatus *st) { memset(st, 0, sizeof *st); }
