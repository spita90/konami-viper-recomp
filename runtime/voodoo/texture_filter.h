// Texture magnification override; minification retains the game's mip/filter choice.
#pragma once
static inline int viper_texture_bilinear(int mode, int magnifying, int game_mag, int game_min) {
    if (!magnifying) return game_min;
    return mode == 1 ? 0 : mode == 2 ? 1 : game_mag;
}
