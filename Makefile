# Konami Viper static recompilation - build
#   make games                 : list the supported games (games/<id>/game.json)
#   make check     GAME=<id>   : verify the files in roms/ against the expected SHA1s
#   make extract   GAME=<id>   : roms/ -> work/<id>/  (CF image, kernel, game modules; needs chdman)
#   make recomp    GAME=<id>   : work/<id>/ -> generated/<id>/*.c
#   make           GAME=<id>   : build the executable (./td2 for thrild2)
#   make game      GAME=<id>   : all three steps above (extract, recomp, build)
#   make distclean GAME=<id>   : remove everything derived from that game's data
# GAME defaults to thrild2.

GAME    ?= thrild2
BIN     := $(shell python3 tools/game.py $(GAME) binary)
ifeq ($(BIN),)
$(error GAME=$(GAME) has no profile in games/ (see 'make games'))
endif
GEN     := generated/$(GAME)
BUILD   := build/$(GAME)

CC      ?= cc
OPT     ?= -O2
ARCHFLAGS := $(shell uname -m | grep -q x86_64 && echo -mfma -mavx2)
CFLAGS  += $(EXTRA) $(OPT) $(ARCHFLAGS) -g -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Iruntime -I$(GEN) \
           -ffp-contract=off -fno-strict-aliasing -Wall -Wno-unused-label -Wno-unused-variable \
           -Wno-unused-function
GENFLAGS := -Wno-unused-but-set-variable -Wno-parentheses-equality -frounding-math
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)
LDFLAGS += -lpthread -lm $(SDL_LIBS)

# WIN=1: cross-build for Windows x64 with mingw-w64 (brew install mingw-w64) and the SDL2 MinGW
# development package (SDL2-devel-*-mingw from libsdl.org) in SDL2_MINGW; gives a self-contained
# <bin>.exe (SDL2 linked statically). A console program, so the log shows and can be redirected.
ifeq ($(WIN),1)
SDL2_MINGW ?= build/win-deps/SDL2-2.32.10/x86_64-w64-mingw32
CC      := x86_64-w64-mingw32-gcc
CXX     := x86_64-w64-mingw32-g++
ARCHFLAGS := -mfma -mavx2
BIN     := $(BIN).exe
BUILD   := build/$(GAME)-win
SDL_CFLAGS := -I$(SDL2_MINGW)/include/SDL2 -Dmain=SDL_main
SDL_LIBS := -L$(SDL2_MINGW)/lib -lmingw32 -lSDL2main -lSDL2 -ldinput8 -ldxguid -ldxerr8 -luser32 -lgdi32 \
            -lwinmm -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid
LDFLAGS := -static -lpthread -lm $(SDL_LIBS) -lws2_32 -liphlpapi -lmswsock
endif

CXX     ?= c++
CXXFLAGS += $(EXTRA) $(OPT) $(ARCHFLAGS) -g -std=c++20 -Iruntime/voodoo -Wno-unused-private-field \
            -Wno-deprecated-declarations
RT_SRCS := runtime/cpu.c runtime/sched.c runtime/hw.c runtime/main.c runtime/frontend_sdl.c runtime/enhanced.c runtime/net.c runtime/portmap.c
# third-party, compiled in (third_party/README.md): UPnP and NAT-PMP for the link-play host
TP_SRCS := $(wildcard third_party/miniupnpc/src/*.c) third_party/libnatpmp/natpmp.c third_party/libnatpmp/getgateway.c \
           $(if $(filter 1,$(WIN)),third_party/libnatpmp/wingettimeofday.c)
TP_INC := -Ithird_party/miniupnpc/include -Ithird_party/libnatpmp -DMINIUPNP_STATICLIB -DNATPMP_STATICLIB
TP_CFLAGS := -O2 -w $(ARCHFLAGS) $(TP_INC) -Ithird_party/miniupnpc/src -DMINIUPNPC_SET_SOCKET_TIMEOUT \
             -DMINIUPNPC_GET_SRC_ADDR -D_BSD_SOURCE -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
VD_SRCS := runtime/voodoo/voodoo.cpp runtime/voodoo/voodoo_2.cpp runtime/voodoo/voodoo_banshee.cpp \
           runtime/voodoo/voodoo_render.cpp runtime/voodoo/voodoo_bridge.cpp runtime/voodoo/video/rgbutil.cpp
-include $(GEN)/sources.mk

RT_OBJS := $(RT_SRCS:%.c=$(BUILD)/%.o)
GEN_OBJS := $(GEN_SRCS:%.c=$(BUILD)/%.o)
VD_OBJS := $(VD_SRCS:%.cpp=$(BUILD)/%.o)
TP_OBJS := $(TP_SRCS:%.c=$(BUILD)/%.o)

ifeq ($(wildcard $(GEN)/sources.mk),)
$(BIN):
	@echo "$(GEN)/ is missing: run 'make extract GAME=$(GAME)' and 'make recomp GAME=$(GAME)' first" >&2
	@exit 1
else
$(BIN): $(RT_OBJS) $(VD_OBJS) $(GEN_OBJS) $(TP_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)
endif

$(BUILD)/runtime/voodoo/%.o: runtime/voodoo/%.cpp runtime/voodoo/*.h runtime/voodoo/video/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/runtime/%.o: runtime/%.c runtime/*.h $(GEN)/modules.h $(GEN)/game_config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(TP_INC) -c $< -o $@

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TP_CFLAGS) -c $< -o $@

$(BUILD)/$(GEN)/%.o: $(GEN)/%.c runtime/ppc_rt.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GENFLAGS) -c $< -o $@

games:
	@python3 tools/game.py list

check:
	@python3 tools/game.py $(GAME) check

extract:
	python3 tools/extract.py $(GAME)

recomp:
	python3 recomp/recomp.py $(GAME)

# the build step is a separate make, so that it sees the sources.mk that recomp generates
game:
	$(MAKE) extract GAME=$(GAME)
	$(MAKE) recomp GAME=$(GAME)
	$(MAKE) GAME=$(GAME)

clean:
	rm -rf $(BUILD) $(BIN)

distclean: clean
	rm -rf work/$(GAME) $(GEN) $(BIN)_nvram.bin

.PHONY: games check extract recomp game clean distclean
