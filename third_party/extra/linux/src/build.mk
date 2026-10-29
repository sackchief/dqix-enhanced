# Build the recompiled game (both CPUs) + runtime (Linux / MSYS2 MinGW on Windows).
#   GEN  = generated ARM9 C (recomp/build.py --hle hle.txt)
#   GEN7 = generated ARM7 C (recomp/arm7.py --hle hle7.txt)
#   FRONTEND = sdl (window, input, sound) or none (headless: frame dumps, scripted input)
#   SDL_CFLAGS / SDL_LIBS default to sdl2-config when FRONTEND=sdl
GEN ?= ../build/recomp
GEN7 ?= ../build/recomp7
CC ?= gcc
CFLAGS ?= -O1 -g -w
OBJ ?= obj
FRONTEND ?= none
OUT ?= dqix

RT = $(wildcard runtime/*.c)
RT := $(filter-out runtime/stub_runtime.c runtime/frontend_%.c runtime/gl_render.c runtime/menu_win32.c runtime/controls.c runtime/discord.c runtime/exe_main.c,$(RT)) $(if $(filter sdl,$(FRONTEND)),third_party/extra/linux/src/runtime/frontend_sdl.c,runtime/frontend_$(FRONTEND).c)

ifeq ($(FRONTEND),sdl)
SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS ?= $(shell sdl2-config --libs)
RT += runtime/gl_render.c runtime/menu_win32.c runtime/controls.c runtime/discord.c
endif

# libslirp (user-mode network stack for the wireless emulation, BSD licence)
SLIRP_SRC = $(wildcard third_party/libslirp/src/*.c) third_party/libslirp/glib/glib.c
SLIRPOBJ = $(patsubst third_party/libslirp/%.c,$(OBJ)/slirp/%.o,$(SLIRP_SRC))
SLIRP_INC = -Ithird_party/libslirp/src -Ithird_party/libslirp/glib
ifeq ($(OS),Windows_NT)
NET_LIBS = -lws2_32 -liphlpapi -lgdi32 -lwinhttp -lcomdlg32
endif
NET_LIBS ?=

GENOBJ = $(patsubst $(GEN)/%.c,$(OBJ)/gen/%.o,$(wildcard $(GEN)/*.c))
GEN7OBJ = $(patsubst $(GEN7)/%.c,$(OBJ)/gen7/%.o,$(wildcard $(GEN7)/*.c))
RTOBJ = $(patsubst runtime/%.c,$(OBJ)/rt/%.o,$(filter runtime/%,$(RT))) $(if $(filter third_party/extra/linux/src/runtime/frontend_sdl.c,$(RT)),$(OBJ)/rt/frontend_sdl.o $(OBJ)/rt/linux_overlay.o $(OBJ)/rt/linux_overlay_gl.o)

# RES: optional extra objects for the program only, e.g. the Windows icon
# resource the build makes from the ROM (tools/rom_icon.py)
RES ?=
$(OUT): $(GENOBJ) $(GEN7OBJ) $(RTOBJ) $(SLIRPOBJ) $(OBJ)/rt/exe_main.o $(RES)
	$(CC) -o $@ $^ $(SDL_LIBS) -lm $(NET_LIBS) $(LDFLAGS)

# The game as a library (CORE=dqixcore.dll / dqixcore.so): in a session the
# other players' DSes run from copies of it on one thread (hosting.c). Same
# objects, without the window; on Linux everything needs PIC=-fPIC.
CORE_RTOBJ = $(patsubst runtime/%.c,$(OBJ)/rt/%.o,$(filter runtime/%,$(filter-out runtime/frontend_%.c third_party/extra/linux/src/runtime/frontend_%.c runtime/gl_render.c runtime/menu_win32.c runtime/controls.c runtime/discord.c,$(RT))) runtime/frontend_none.c)
ifeq ($(OS),Windows_NT)
CORE_LDFLAGS ?= -static-libgcc
else
CORE_LDFLAGS ?= -Wl,-Bsymbolic -lpthread
endif
core: $(CORE)
$(CORE): $(GENOBJ) $(GEN7OBJ) $(CORE_RTOBJ) $(SLIRPOBJ)
	$(CC) -shared -o $@ $^ -lm $(NET_LIBS) $(CORE_LDFLAGS)

$(OBJ)/slirp/%.o: third_party/libslirp/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w $(PIC) -DLIBSLIRP_STATIC_BUILD -DBUILDING_LIBSLIRP -DG_LOG_DOMAIN=\"Slirp\" $(SLIRP_INC) -c $< -o $@

$(OBJ)/rt/net.o: runtime/net.c runtime/nds.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall $(PIC) -DLIBSLIRP_STATIC_BUILD $(DEFS) $(SLIRP_INC) -Iruntime -c $< -o $@

$(OBJ)/gen/%.o: $(GEN)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PIC) $(DEFS) -Iruntime -I$(GEN) -c $< -o $@

$(OBJ)/gen7/%.o: $(GEN7)/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PIC) $(DEFS) -Iruntime -I$(GEN7) -c $< -o $@

$(OBJ)/rt/gl_render.o: runtime/gl_render.c runtime/gl_render.h runtime/gpu3d.h runtime/nds.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wno-unused-parameter $(PIC) $(SDL_CFLAGS) $(DEFS) -Iruntime -c $< -o $@

$(OBJ)/rt/controls.o: runtime/controls.c runtime/controls.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wno-unused-parameter $(PIC) $(SDL_CFLAGS) $(DEFS) -Iruntime -c $< -o $@

$(OBJ)/rt/discord.o: runtime/discord.c runtime/discord.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wno-unused-parameter $(PIC) $(SDL_CFLAGS) $(DEFS) -Iruntime -c $< -o $@

$(OBJ)/rt/menu_win32.o: runtime/menu_win32.c runtime/menu.h runtime/controls.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wno-unused-parameter $(PIC) $(SDL_CFLAGS) $(DEFS) -Iruntime -c $< -o $@

$(OBJ)/rt/frontend_sdl.o: third_party/extra/linux/src/runtime/frontend_sdl.c runtime/nds.h runtime/frontend.h runtime/gl_render.h runtime/menu.h runtime/version.h runtime/session.h runtime/air.h runtime/text.h runtime/controls.h runtime/discord.h runtime/music.h third_party/extra/linux/overlay/linux_overlay.h third_party/extra/linux/overlay/linux_overlay_gl.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wno-unused-parameter $(PIC) $(DETERMINISM) $(SDL_CFLAGS) $(DEFS) -Iruntime -c $< -o $@

$(OBJ)/rt/linux_overlay.o: third_party/extra/linux/overlay/linux_overlay.c third_party/extra/linux/overlay/linux_overlay.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall $(PIC) -Ithird_party/extra/linux/overlay -c $< -o $@

$(OBJ)/rt/linux_overlay_gl.o: third_party/extra/linux/overlay/linux_overlay_gl.c third_party/extra/linux/overlay/linux_overlay_gl.h third_party/extra/linux/overlay/linux_overlay.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall $(PIC) $(SDL_CFLAGS) -Ithird_party/extra/linux/overlay -c $< -o $@

# Sessions need every PC to compute exactly the same: no stack garbage in uninitialised variables.
DETERMINISM ?= -ftrivial-auto-var-init=zero
$(OBJ)/rt/relay.o: runtime/relay.h runtime/relay_url.h
$(OBJ)/rt/music.o: runtime/stb_vorbis.h runtime/dr_flac.h runtime/music.h
$(OBJ)/rt/%.o: runtime/%.c runtime/nds.h runtime/cpu.h runtime/coro.h runtime/frontend.h runtime/version.h runtime/air.h runtime/session.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter $(PIC) $(DETERMINISM) $(DEFS) -Iruntime -I$(GEN) -c $< -o $@

clean:
	rm -rf $(OBJ) $(OUT) $(CORE)

.PHONY: core clean
