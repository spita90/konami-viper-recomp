/* Physical rumble diagnostic: no game or direct Apple haptics calls. */
#include <SDL.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t interrupted;
static void interrupt_test(int sig) { (void)sig; interrupted = 1; }
static int pump_for(Uint32 duration, SDL_GameController *pad) {
    Uint32 start = SDL_GetTicks();
    while (!interrupted && SDL_GetTicks() - start < duration) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) interrupted = 1;
        }
        if (pad && !SDL_GameControllerGetAttached(pad)) {
            fprintf(stderr, "Controller disconnected during test.\n");
            return 0;
        }
        SDL_Delay(5);
    }
    return !interrupted;
}
static const char *value(const char *s) { return s ? s : "(unset)"; }
int main(int argc, char **argv) {
    int list_only = 0;
    const char *driver = "default";
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--list")) list_only = 1;
        else if (!strcmp(argv[i], "--driver") && i+1<argc) driver=argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            puts("Usage: sdl-rumble [--list] [--driver default|hidapi|native]\n"
                 "Sends three equal one-second 50% pulses, separated by one second.\n"
                 "--list discovers controllers without sending rumble.\n"
                 "native disables HIDAPI; hidapi enables it (routing is still SDL's choice).\n"
                 "Quit games first; connect exactly one controller. Ctrl-C stops the test.");
            return 0;
        } else { fprintf(stderr,"Unknown/incomplete option: %s\n",argv[i]); return 2; }
    }
    if (strcmp(driver,"default") && strcmp(driver,"hidapi") && strcmp(driver,"native")) {
        fprintf(stderr,"Invalid driver: %s\n",driver); return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, interrupt_test);
    signal(SIGTERM, interrupt_test);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    if (strcmp(driver,"default")) {
        const char *enabled = !strcmp(driver,"hidapi") ? "1" : "0";
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI,enabled,SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_SWITCH,enabled,SDL_HINT_OVERRIDE);
    }
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr,"SDL init failed: %s\n",SDL_GetError()); return 1;
    }
    SDL_version version; SDL_GetVersion(&version);
    printf("SDL runtime %u.%u.%u; revision=%s\n",version.major,version.minor,version.patch,SDL_GetRevision());
    printf("Requested driver=%s; HIDAPI=%s; Switch HIDAPI=%s\n", driver,
        value(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI)),value(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH)));
    if (!pump_for(2000,NULL)) { SDL_Quit(); return 1; }
    int count=0, selected=-1;
    for (int i=0;i<SDL_NumJoysticks();++i) {
        if (!SDL_IsGameController(i)) continue;
        selected=i; ++count;
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),guid,sizeof guid);
        printf("GUID=%s\n",guid);
        printf("Controller %d: %s; path=%s\n",i,value(SDL_GameControllerNameForIndex(i)),value(SDL_GameControllerPathForIndex(i)));
    }
    if (list_only) { printf("Recognized controllers: %d\n",count); SDL_Quit(); return 0; }
    if (count!=1) { fprintf(stderr,"Connect exactly one controller (found %d). No pulses sent.\n",count); SDL_Quit(); return 1; }
    SDL_GameController *pad=SDL_GameControllerOpen(selected);
    if (!pad) { fprintf(stderr,"Open failed: %s\n",SDL_GetError()); SDL_Quit(); return 1; }
    printf("Controller type=%d; rumble advertised=%d\n",SDL_GameControllerGetType(pad),SDL_GameControllerHasRumble(pad));
    int result=0;
    for (int pulse=1;pulse<=3 && !interrupted;++pulse) {
        SDL_ClearError();
        int sent=SDL_GameControllerRumble(pad,32768,32768,1000);
        printf("Pulse %d/3: low=32768 high=32768 duration=1000ms result=%d%s%s\n",
            pulse,sent,sent<0?" error=":"",sent<0?SDL_GetError():"");
        if (sent<0 || !pump_for(1200,pad)) { result=1; break; }
        if (SDL_GameControllerRumble(pad,0,0,0)<0) {
            fprintf(stderr,"Stop failed: %s\n",SDL_GetError()); result=1; break;
        }
        if (!pump_for(1000,pad)) { result=1; break; }
    }
    SDL_GameControllerRumble(pad,0,0,0);
    SDL_GameControllerClose(pad);
    SDL_Quit();
    puts("Test finished. API success does not confirm physical vibration.");
    return interrupted ? 1 : result;
}
