# Konami Viper static recompilation - build
#   make games                 : list the supported games (games/<id>/game.json)
#   make check     GAME=<id>   : verify the files in roms/ against the expected SHA1s
#   make extract   GAME=<id>   : roms/ -> work/<id>/  (CF image, kernel, game modules)
#   make recomp    GAME=<id>   : work/<id>/ -> generated/<id>/*.c
#   make           GAME=<id>   : build the executable (./td2 for thrild2)
#   make game      GAME=<id>   : all three steps above (extract, recomp, build)
#   make distclean GAME=<id>   : remove everything derived from that game's data
# GAME defaults to thrild2. The sources and the flags come from tools/buildspec.py, shared with
# the release launcher (dist/launcher.py); the CHDs are read by tools/chdextract.c, built here.

GAME    ?= thrild2
BIN     := $(shell python3 tools/game.py $(GAME) binary)
ifeq ($(BIN),)
$(error GAME=$(GAME) has no profile in games/ (see 'make games'))
endif
GEN     := generated/$(GAME)
BUILD   := build/$(GAME)
.DEFAULT_GOAL := $(BIN)

CC      ?= cc
CXX     ?= c++
OPT     ?= -O2
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)

# RT_SRCS, VD_SRCS, GL_SRCS, TP_SRCS, CHD_SRCS, their flags and LIBS (make remakes this file
# first when tools/buildspec.py changes)
build/make-vars.mk: tools/buildspec.py
	@mkdir -p $(dir $@)
	python3 tools/buildspec.py make > $@
include build/make-vars.mk
-include $(GEN)/sources.mk

RT_OBJS := $(RT_SRCS:%.c=$(BUILD)/%.o)
GEN_OBJS := $(GEN_SRCS:%.c=$(BUILD)/%.o)
VD_OBJS := $(VD_SRCS:%.cpp=$(BUILD)/%.o)
GL_OBJS := $(GL_SRCS:%.cpp=$(BUILD)/%.o)
TP_OBJS := $(TP_SRCS:%.c=$(BUILD)/%.o)
CHDEXTRACT := build/tools/chdextract

ifeq ($(wildcard $(GEN)/sources.mk),)
$(BIN):
	@echo "$(GEN)/ is missing: run 'make extract GAME=$(GAME)' and 'make recomp GAME=$(GAME)' first" >&2
	@exit 1
else
$(BIN): $(RT_OBJS) $(VD_OBJS) $(GL_OBJS) $(GEN_OBJS) $(TP_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LIBS) $(SDL_LIBS)
endif

$(BUILD)/runtime/voodoo/%.o: runtime/voodoo/%.cpp runtime/voodoo/*.h runtime/voodoo/video/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(EXTRA) $(CXX_FLAGS) -c $< -o $@

$(BUILD)/runtime/gpu_gl.o: runtime/gpu_gl.cpp runtime/voodoo/voodoo_gpu.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(EXTRA) $(CXX_FLAGS) $(SDL_CFLAGS) -c $< -o $@

$(BUILD)/runtime/%.o: runtime/%.c runtime/*.h $(GEN)/modules.h $(GEN)/game_config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA) $(RT_FLAGS) $(SDL_CFLAGS) -c $< -o $@

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TP_FLAGS) -c $< -o $@

$(BUILD)/$(GEN)/%.o: $(GEN)/%.c runtime/ppc_rt.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA) $(GEN_FLAGS) -c $< -o $@

# reads the CHDs for check and extract (tools/game.py)
$(CHDEXTRACT): $(CHD_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(CHD_FLAGS) $(CHD_SRCS) -o $@

games:
	@python3 tools/game.py list

check: $(CHDEXTRACT)
	@python3 tools/game.py $(GAME) check

extract: $(CHDEXTRACT)
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
