#include "voodoo/texture_filter.h"
#include <cassert>
#include <cstdio>
int main() {
    for (int mag=0;mag<2;mag++) for (int min=0;min<2;min++) {
        assert(viper_texture_bilinear(0,1,mag,min)==mag);
        assert(viper_texture_bilinear(1,1,mag,min)==0);
        assert(viper_texture_bilinear(2,1,mag,min)==1);
        for (int mode=0;mode<3;mode++) assert(viper_texture_bilinear(mode,0,mag,min)==min);
    }
    puts("texture magnification overrides preserve game minification: passed");
}
