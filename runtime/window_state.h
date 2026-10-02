/* Window geometry is independent of the game mode and its NVRAM. */
#pragma once
#include <SDL.h>
#include <stdio.h>
#include <string.h>

static int window_state_load(const char *path, SDL_Rect *rect) {
    if (!*path) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    SDL_Rect saved;
    int version;
    int valid = fscanf(file, "%d %d %d %d %d", &version, &saved.x, &saved.y, &saved.w, &saved.h) == 5;
    fclose(file);
    if (!valid || version != 1 || saved.w < 320 || saved.h < 240 ||
        saved.w > 16384 || saved.h > 16384 || saved.x < -1000000 || saved.x > 1000000 ||
        saved.y < -1000000 || saved.y > 1000000) return 0;
    *rect = saved;
    return 1;
}

static int window_state_save(const char *path, const SDL_Rect *rect) {
    if (!*path) return 1;
    char temporary[1100];
    if (snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary) return 0;
    FILE *file = fopen(temporary, "w");
    if (!file) return 0;
    int ok = fprintf(file, "1 %d %d %d %d\n", rect->x, rect->y, rect->w, rect->h) > 0;
    if (fclose(file)) ok = 0;
    if (ok && rename(temporary, path) == 0) return 1;
    remove(temporary);
    return 0;
}

static void window_state_fit(SDL_Rect *rect, const SDL_Rect *displays, int count) {
    if (count < 1) return;
    int chosen = 0, largest = 0;
    for (int i = 0; i < count; i++) {
        SDL_Rect intersection;
        if (SDL_IntersectRect(rect, &displays[i], &intersection) && intersection.w * intersection.h > largest) {
            largest = intersection.w * intersection.h;
            chosen = i;
        }
    }
    SDL_Rect display = displays[chosen];
    rect->w = SDL_min(rect->w, display.w);
    rect->h = SDL_min(rect->h, display.h);
    if (!largest) {
        rect->x = display.x + (display.w - rect->w) / 2;
        rect->y = display.y + (display.h - rect->h) / 2;
    } else {
        rect->x = SDL_clamp(rect->x, display.x, display.x + display.w - rect->w);
        rect->y = SDL_clamp(rect->y, display.y, display.y + display.h - rect->h);
    }
}

static int window_state_capture(SDL_Window *window, SDL_Rect *rect) {
    if (SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) return 0;
    SDL_Rect current;
    SDL_GetWindowPosition(window, &current.x, &current.y);
    SDL_GetWindowSize(window, &current.w, &current.h);
    if (current.x == rect->x && current.y == rect->y && current.w == rect->w && current.h == rect->h) return 0;
    *rect = current;
    return 1;
}
