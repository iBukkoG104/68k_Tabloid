# Harness M68K - build
#   make                       # Musashi in ../Musashi, finestra SDL2 se c'e'
#   make MUSASHI=/percorso/Musashi
#   make NOGUI=1               # senza SDL2: emulazione display headless
#   make roms                  # ROM di test in test/ (serve vasmm68k_mot)
MUSASHI ?= ../Musashi
BUILD   := build
CC      ?= gcc

CFLAGS   ?= -O2 -g
CPPFLAGS += -I. -I$(MUSASHI) -I$(BUILD) -DMUSASHI_CNF=\"harness_conf.h\"
WARN     := -Wall -Wextra -Wno-unused-parameter

# --- SDL2: opzionale, rilevata con pkg-config -------------------------
ifeq ($(NOGUI),1)
  SDL_CFLAGS :=
  SDL_LIBS   :=
  GUI_MSG    := disabilitata (NOGUI=1)
else
  SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
  ifeq ($(SDL_LIBS),)
    SDL_CFLAGS :=
    GUI_MSG    := disabilitata (SDL2 non trovata da pkg-config)
  else
    SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null) -DHAVE_SDL2
    GUI_MSG    := SDL2 $(shell pkg-config --modversion sdl2)
  endif
endif

# Sorgenti Musashi (m68kops.c/h vengono generati da m68kmake)
MUS_SRC := $(MUSASHI)/m68kcpu.c $(MUSASHI)/m68kdasm.c $(MUSASHI)/softfloat/softfloat.c
DEV_OBJS := $(BUILD)/ssd1963.o $(BUILD)/xpt2046.o $(BUILD)/panel.o \
            $(BUILD)/z8530.o
OBJS    := $(BUILD)/harness.o $(BUILD)/m68kcpu.o $(BUILD)/m68kdasm.o \
           $(BUILD)/m68kops.o $(BUILD)/softfloat.o $(DEV_OBJS)

all: m68k-harness

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/m68kmake: $(MUSASHI)/m68kmake.c | $(BUILD)
	$(CC) -o $@ $<

$(BUILD)/m68kops.h $(BUILD)/m68kops.c: $(BUILD)/m68kmake $(MUSASHI)/m68k_in.c
	$(BUILD)/m68kmake $(BUILD) $(MUSASHI)/m68k_in.c

$(BUILD)/m68kcpu.o: $(MUSASHI)/m68kcpu.c $(BUILD)/m68kops.h harness_conf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/m68kdasm.o: $(MUSASHI)/m68kdasm.c harness_conf.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/m68kops.o: $(BUILD)/m68kops.c $(BUILD)/m68kops.h harness_conf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/softfloat.o: $(MUSASHI)/softfloat/softfloat.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/harness.o: harness.c harness_conf.h devices/panel.h devices/ssd1963.h \
                    devices/xpt2046.h devices/z8530.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

# Periferiche: solo panel.c ha bisogno dei flag di SDL, ma passarli a tutte
# non fa male e tiene la regola unica.
$(BUILD)/%.o: devices/%.c devices/ssd1963.h devices/xpt2046.h devices/panel.h \
              devices/z8530.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(SDL_CFLAGS) -c -o $@ $<

m68k-harness: $(OBJS)
	@echo "  finestra pannello: $(GUI_MSG)"
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(SDL_LIBS) -lm

# --- ROM di test ------------------------------------------------------
VASM ?= vasmm68k_mot
TESTS := test/display.bin test/touch.bin test/features.bin test/penirq.bin \
         test/serial.bin

roms: $(TESTS)
	python3 mkroms.py test

test/%.bin: test/%.s test/ssdlib.s test/ssd1963.i
	$(VASM) -Fbin -m68010 -no-opt -Itest -o $@ $<

ifeq ($(SDL_LIBS),)
  E2E :=
else
  E2E := $(BUILD)/panel-e2e
endif

check: all roms $(E2E)
	@test/run-tests.sh

# La stessa prova end-to-end, ma contro il display vero invece che contro il
# driver dummy. Si salta da sola se il compositore non lascia spostare il
# puntatore (p.es. Wayland).
check-gui: $(E2E)
	@if [ -x "$(BUILD)/panel-e2e" ]; then \
	   $(BUILD)/panel-e2e; rc=$$?; \
	   if [ $$rc = 77 ]; then echo "  (saltata)"; exit 0; else exit $$rc; fi; \
	 else echo "  (saltata: compilato senza SDL2)"; fi

$(BUILD)/panel-e2e: test/panel-e2e.c $(BUILD)/panel.o $(BUILD)/ssd1963.o \
                    $(BUILD)/xpt2046.o | $(BUILD)
	$(CC) $(CPPFLAGS) -Idevices $(CFLAGS) $(WARN) $(SDL_CFLAGS) -o $@ $< \
	      $(BUILD)/panel.o $(BUILD)/ssd1963.o $(BUILD)/xpt2046.o $(SDL_LIBS) -lm

clean:
	rm -rf $(BUILD) m68k-harness
	rm -f test/*.bin

.PHONY: all check check-gui clean roms
