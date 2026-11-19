#include "../runtime/enhanced.c"
#include <assert.h>
static int stick_response=2;
int frontend_stick_response(void) { return stick_response; }
void frontend_set_stick_response(int v) { stick_response = v>=0 && v<=2 ? v : 2; }
int frontend_rumble_multiplier(void) { return 100; }
void frontend_set_rumble_multiplier(int v) { (void)v; }
uint8_t *g_ram;
void voodoo_set_scale(int n) {(void)n;}
void voodoo_set_wide(int n) {(void)n;}
void rt_log(const char *s,...) {(void)s;}
uint8_t *hw_nvram(void) { static uint8_t nv[8192]; return nv; }
uint64_t rt_now(void) {return 0;}
int main(int argc, char **argv) {
 assert(argc==2); g_enhanced=1; g_font=(uint8_t*)1; g_booted=1;
 snprintf(g_settings_path,sizeof g_settings_path,"%s",argv[1]);
 assert(frontend_stick_response()==2);
 g_frame=g_attract_frame=100;g_screen=SCREEN_PAGE;g_page=PAGE_CONTROLS;
 int rows[MAX_GAME_OPTIONS+2], n=page_rows(PAGE_CONTROLS,rows);
 int cursor=0; while (rows[cursor] != -9) cursor++;   /* the STICK RESPONSE row, also in the pause menu */
 assert(cursor<n);
 g_page_cursor=cursor;
 enh_menu_action(ENH_LEFT);assert(frontend_stick_response()==1);
 enh_menu_action(ENH_LEFT);assert(frontend_stick_response()==0);
 enh_menu_action(ENH_LEFT);assert(frontend_stick_response()==2);
 enh_menu_action(ENH_RIGHT);assert(frontend_stick_response()==0);
 frontend_set_stick_response(2);settings_load();assert(frontend_stick_response()==0);
 FILE *f=fopen(g_settings_path,"w");assert(f);fputs("stick_response = 99\n",f);fclose(f);
 settings_load();assert(frontend_stick_response()==2);
 g_attract_frame=0;g_frame=500;assert(enh_escape());
 enh_menu_action(ENH_DOWN);enh_menu_action(ENH_OK);assert(g_pause_controls);
 g_controls_cursor=cursor;enh_menu_action(ENH_LEFT);assert(frontend_stick_response()==1);
 enh_menu_action(ENH_BACK);assert(!g_pause_controls && g_paused);
 remove(g_settings_path);
 puts("stick response menu, pause, persistence and invalid settings: passed");
}

int interp_supported(void) { return 0; }   /* interp.c, not linked here */
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
