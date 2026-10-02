/* A saved window rectangle fitted to the displays connected now: on the display it overlaps the
 * most, no larger than it; centred on the first display if it overlaps none (a monitor gone). */
#pragma once
#include <SDL.h>

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
