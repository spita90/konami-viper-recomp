#include "window_state.h"
#include <assert.h>
int main(int argc, char **argv) {
 assert(argc==2);
 SDL_Rect displays[]={{0,25,1440,850},{-1280,0,1280,800}};
 SDL_Rect rect={-1000,100,800,600};
 assert(window_state_save(argv[1], &rect));
 SDL_Rect read={0};assert(window_state_load(argv[1], &read));
 assert(read.x==rect.x&&read.y==rect.y&&read.w==rect.w&&read.h==rect.h);
 window_state_fit(&read,displays,2);assert(read.x==-1000&&read.y==100);
 window_state_fit(&read,displays,1);assert(read.x==320&&read.y==150);
 read=(SDL_Rect){1400,840,1000,700};window_state_fit(&read,displays,1);
 assert(read.x==440&&read.y==175);
 read=(SDL_Rect){0,0,3000,2000};window_state_fit(&read,displays,1);
 assert(read.w==1440&&read.h==850&&read.x==0&&read.y==25);
 FILE *file=fopen(argv[1],"w");fputs("invalid",file);fclose(file);
 assert(!window_state_load(argv[1], &read));remove(argv[1]);
 puts("window persistence, negative coordinates, monitor removal and bounds: passed");
}
